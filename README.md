# TradeMatch Engine

[![CI](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/ci.yml)
[![Fuzz](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/fuzz.yml/badge.svg?branch=main)](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/fuzz.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

TradeMatch Engine is a compact C++ order matching engine built to demonstrate the core mechanics behind a price-time-priority exchange book. The project focuses on the matching loop itself: deterministic order handling, clean data structures, strong tests, and a repository layout that is easy to inspect.

Prices are represented as integer ticks rather than floating-point values. In the demo and tests, one tick equals one cent.

## Why It Is Interesting

This is the kind of systems problem that rewards precision more than size. A matching engine needs predictable behavior, strict sequencing rules, efficient cancellation, and a design that is easy to reason about under load. Even in a compact codebase, it exercises the same skills that matter in backend and low-latency systems work:

- data structure selection under performance constraints
- deterministic state transitions
- correctness-first handling of edge cases
- clear interfaces for testing, extensions, and instrumentation

## Supported Behavior

- Limit orders
- Market orders
- Partial fills
- Price-time priority
- Trade execution reporting
- Cancel by order ID
- Replace by order ID
- Resting-order lookup by order ID
- Deterministic benchmark/demo scenarios

## API Overview

Core public API:

- `submit(OrderRequest)` submits a new order and returns fills, remaining quantity, and any generated trades.
- `cancel(OrderId)` removes a resting order by ID and reports the cancelled quantity.
- `replace(OrderRequest)` validates, cancels, and resubmits an active order using the same ID. The replacement must keep the resting order's side and limit type.
- `get_order(OrderId)` returns the current resting state for an active order.
- `get_orders_at_level(Side, Price)` exposes FIFO order state for a price level.
- `snapshot()` returns visible bid/ask levels for inspection and tests.

Minimal usage:

```cpp
tradematch::OrderBook book;

auto add = book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10050});
auto replace = book.replace({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 80, 10075});
auto cancel = book.cancel(1);
auto snapshot = book.snapshot();
```

## Matching Rules

The engine follows standard price-time priority:

- Buy orders match the lowest available ask first.
- Sell orders match the highest available bid first.
- At the same price level, older resting orders fill before newer ones.
- Trades execute at the resting order's price.
- Market orders consume available liquidity and never rest in the book.
- Unfilled market quantity expires immediately.
- Replace is implemented as cancel plus resubmit, so the replaced order loses its original time priority.
- Replace may change price and quantity, but never the side or the limit order type; such requests are rejected and the resting order is left untouched.
- Aggregated quantities (price-level totals) are 64-bit, so they never overflow even when many large orders share a level.

## System Design

The matching core is intentionally single-threaded. That keeps sequencing deterministic and makes the behavior straightforward to test. Concurrency can be layered around the engine later, but the matching path itself should stay simple and predictable.

### Request Flow

1. Validate the incoming request.
2. Match against the best opposing price levels.
3. Emit trade records for each execution.
4. Either rest the remaining quantity, expire it, or remove it on cancel.
5. Expose the resulting state through lookup and snapshot methods.

### Core Data Structures

- `std::map<Price, std::list<RestingOrder>, std::greater<>>` for bids
- `std::map<Price, std::list<RestingOrder>, std::less<>>` for asks
- `std::unordered_map<OrderId, OrderLocator>` for active-order lookup and cancel

Why this layout:

- integer price ticks avoid floating-point comparison issues in matching logic
- `std::map` keeps best bid / best ask at the front in sorted order.
- `std::list` preserves FIFO within a price level and gives stable iterators.
- `std::unordered_map` gives direct access to the resting order for cancellation.

This yields:

- clear price priority across levels
- clear time priority within a level
- efficient cancel without scanning the whole book

### Design Tradeoffs

- The engine is single-threaded by design. That keeps sequencing deterministic and makes correctness easier to inspect, but it is not a full multi-producer exchange gateway.
- `std::map` plus `std::list` is not the most cache-optimized possible layout, but it gives very clear price ordering, stable iterators for cancel, and compact code.
- Replace is intentionally modeled as validate, cancel, and resubmit. That keeps the implementation simple and makes the priority reset explicit instead of hiding it.

## Repository Layout

```text
.
├── .github/workflows/
│   ├── ci.yml
│   └── fuzz.yml
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── CMakeLists.txt
├── Makefile
├── include/tradematch/
│   ├── order_book.hpp
│   └── types.hpp
├── src/
│   ├── formatting.cpp
│   └── order_book.cpp
├── bench/
│   ├── benchmark_main.cpp
│   ├── latency_histogram.hpp
│   ├── system_info.hpp
│   ├── workload.cpp
│   └── workload.hpp
├── tests/
│   ├── bench_tests.cpp
│   ├── differential_tests.cpp
│   ├── order_book_tests.cpp
│   └── support/
│       ├── differential.hpp
│       └── reference_book.hpp
├── fuzz/
│   ├── order_book_fuzzer.cpp
│   └── standalone_main.cpp
└── tools/
    └── demo_main.cpp
```

## Build

Requirements:

- A C++17 compiler
- Optional: CMake 3.16+ if you want the CMake workflow

Fastest path:

```bash
make
```

The Makefile compiles each source file to an object with generated dependency files, so editing a header rebuilds exactly the objects that include it.

CMake alternative:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `TRADEMATCH_WARNINGS_AS_ERRORS` | `OFF` | Promote compiler warnings to errors (`-Werror` / `/WX`), as CI does |
| `TRADEMATCH_SANITIZERS` | empty | Instrument every target, e.g. `address,undefined` or `thread` |
| `TRADEMATCH_COVERAGE` | `OFF` | Build with gcov instrumentation for coverage reports |
| `TRADEMATCH_LIBFUZZER` | `OFF` | Build the coverage-guided libFuzzer target (LLVM clang) |

Developer targets (the same checks CI runs):

```bash
make format        # apply clang-format
make format-check  # verify formatting without changing files
make lint          # clang-tidy with the repository's .clang-tidy rules
make sanitize      # build and test under AddressSanitizer + UndefinedBehaviorSanitizer
make coverage      # line/branch coverage of the engine, with an HTML report
```

`format` and `lint` use clang-format and clang-tidy 22.1.8, which CI pins. Install them with `pip install clang-format==22.1.8 clang-tidy==22.1.8`. Coverage uses `pip install gcovr`.

## Run

Demo:

```bash
./build/tradematch_demo
```

Benchmark (see [Performance](#performance)):

```bash
./build/tradematch_benchmark                       # 1M measured operations, latency table + distribution
./build/tradematch_benchmark 500000                # custom measured operation count
./build/tradematch_benchmark --seed 7 --json out.json
```

Tests:

```bash
make test
```

If you build with CMake instead:

```bash
ctest --test-dir build --output-on-failure
```

## Example Workflow

The demo executable walks through a simple deterministic scenario:

1. Two buy limit orders rest at the same price.
2. One order is replaced with the same price but a new quantity, which resets its time priority.
3. A sell limit order matches the two bids in FIFO order after the replace.
4. A sell limit order rests on the ask side.
5. A buy market order consumes part of that liquidity.
6. The remaining ask is cancelled by order ID.

The output shows:

- whether the order was accepted
- whether a replace succeeded and how much quantity it replaced
- how much filled
- whether any remainder rested or expired
- generated trades with buy/sell IDs, price, quantity, and aggressor side
- a book snapshot after each step

Sample demo requests:

```text
BUY  LIMIT  id=1 qty=100 price=100.50
BUY  LIMIT  id=2 qty=60  price=100.50
REPLACE     id=1 qty=80  price=100.50
SELL LIMIT  id=3 qty=120 price=100.50
SELL LIMIT  id=4 qty=50  price=101.00
BUY  MARKET id=5 qty=40
CANCEL      id=4
```

Sample output excerpt:

```text
Replace order 1
  replaced=true previous_remaining=100
  Order replaced and re-entered the book with a new priority timestamp.
  accepted=true filled=0 remaining=80 rested=true expired=false

Submit order 3 side=SELL type=LIMIT qty=120 price=100.50
  trade#1 buy=2 sell=3 qty=60 price=100.50 aggressor=SELL
  trade#2 buy=1 sell=3 qty=60 price=100.50 aggressor=SELL
```

## Tests Included

The test suite covers deterministic scenarios that matter for correctness:

- exact match
- partial fill with resting remainder
- multiple matches across price levels
- price-time priority at the same price
- cancel order
- replace order priority reset
- replace rejecting side or order-type changes
- 64-bit price-level totals that do not overflow
- unmatched order resting in the book
- market-order expiry of any unfilled remainder
- invalid and duplicate-order rejection
- replace that crosses the book and fills immediately
- lookups for missing price levels and unknown order IDs
- price formatting at the extremes of the 64-bit range
- best bid / best ask tracking as levels are consumed and cancelled
- invariant checks after every submit, cancel, and replace

The benchmark tooling has its own tests ([tests/bench_tests.cpp](tests/bench_tests.cpp)). They check that histogram percentiles stay within the documented error bound against exact sorted percentiles, that the workload generator is deterministic per seed, and that every generated stream replays with zero rejected commands.

## Differential Testing and Fuzzing

Hand-written scenarios only cover the cases someone thought of. To catch the rest, the engine is checked against a second, deliberately naive implementation ([tests/support/reference_book.hpp](tests/support/reference_book.hpp)). The reference keeps every resting order in one flat vector and finds the best match with a linear scan, so it is slow but easy to verify by reading.

- **Randomized differential test** (`tradematch_differential_tests`): runs 400,000 adversarial operations (200 seeds x 2,000) through both books. The flow uses a narrow price band so orders constantly cross, small sizes so partial fills are common, and a steady stream of invalid, duplicate, and unknown-ID requests. After every operation it compares result flags, quantities, every trade field, the full depth snapshot including FIFO order IDs, and order lookups. A failure prints the seed, the diverging operation, and a one-line reproduction command.
- **Coverage-guided fuzzing** ([fuzz/order_book_fuzzer.cpp](fuzz/order_book_fuzzer.cpp)): a libFuzzer target decodes raw bytes into order flow and aborts on the first engine/reference mismatch. CI fuzzes it for 60 seconds under AddressSanitizer and UndefinedBehaviorSanitizer and also runs a 10-million-operation differential soak. A standalone driver keeps the target compiling and smoke-tested on toolchains without libFuzzer, such as Apple clang and MSVC.

The harness has been checked by mutation testing. Each of these injected bugs is caught within the first 63 operations: trading at the aggressor's price, an off-by-one in the limit-price check, and LIFO instead of FIFO queue order.

```bash
make test        # unit tests + 400k-operation differential test + fuzz smoke test
make fuzz-long   # 10M-operation soak: 2,000 seeds x 5,000 operations

# Reproduce a reported failure
./build/tradematch_differential_tests --seed 1234 --ops 5000

# Coverage-guided fuzzing (LLVM clang on Linux)
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DTRADEMATCH_LIBFUZZER=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --target tradematch_fuzzer
./build-fuzz/tradematch_fuzzer -max_total_time=60
```

## Performance

`tradematch_benchmark` measures per-operation latency on realistic order flow, not just average throughput.

**Workload.** A generator produces a deterministic stream that resembles real exchange traffic. Passive limit orders cluster near the touch, at a geometric distance behind the best price. Cancels and modifies always target orders that are still live. Aggressive orders are marketable limits that can sweep several levels, or market orders. The book is prefilled with 10,000 resting orders, and the flow keeps it near that depth, so measurements reflect a steady state rather than a growing book. The realized mix is 46% add, 39% cancel, 8% modify, and 7% aggressive, close to the message proportions of public exchange feeds.

**Method.** The whole stream is generated before timing starts, so generator cost never reaches the measured loop and every engine replays identical input. After 100,000 warm-up operations, each of the 1,000,000 measured operations is timed individually and recorded in an HDR-style log-linear histogram. Its reported percentiles are never below the true value and at most 0.8% above it. Throughput comes from a second replay with no clock reads in the loop.

Results on an Apple M4 (Apple clang 21, Release build, median of 5 runs by p99):

| operation | count | p50 | p90 | p99 | p99.9 | p99.99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| add | 459,871 | 83 ns | 125 ns | 166 ns | 375 ns | 3,967 ns |
| cancel | 390,361 | 42 ns | 84 ns | 167 ns | 543 ns | 3,583 ns |
| modify | 80,097 | 167 ns | 209 ns | 333 ns | 875 ns | 8,959 ns |
| aggressive | 69,671 | 125 ns | 209 ns | 375 ns | 959 ns | 4,639 ns |
| **all** | **1,000,000** | **83 ns** | **166 ns** | **250 ns** | **587 ns** | **4,767 ns** |

Throughput: **10.4 million operations per second** on a single core, producing 139,000 trades.

Notes for reading the numbers:

- Apple Silicon's user-space clock ticks at 24 MHz, so samples are quantized to about 41 ns. Linux on x86 reports nanosecond resolution.
- Every sample includes one clock read, about 13 ns.
- Percentiles up to p99.9 are stable across runs. p99.99 and the maximum mostly reflect operating-system noise (interrupts, page faults, scheduler preemption) on a desktop machine.
- Each submit in this engine allocates map nodes, list nodes, the trade vector, and the result message, and those allocations dominate the tail.

## Continuous Integration

Every push runs two workflows:

| Workflow | Job | What it checks |
| --- | --- | --- |
| CI | Linux / GCC, Linux / Clang, macOS / Apple Clang, Windows / MSVC | Release build with warnings as errors, then the full test suite |
| CI | Sanitizers | Every test under AddressSanitizer (with leak detection) and UndefinedBehaviorSanitizer |
| CI | Format | `clang-format --dry-run --Werror` on every source file |
| CI | Lint | `clang-tidy` with the bugprone, performance, modernize, readability, and clang-analyzer families as errors |
| CI | Benchmark smoke test | `tradematch_benchmark` runs a short workload under CTest on every platform |
| CI | Coverage | Engine line coverage must stay at or above 95% (currently 97% of lines and branches); the HTML report is uploaded as an artifact and the summary appears on the run page |
| Fuzz | Differential soak | 10 million random operations compared against the reference book |
| Fuzz | libFuzzer | Coverage-guided fuzzing under ASan + UBSan: 60 seconds per push and 20 minutes nightly. The corpus is cached between runs, so coverage keeps growing |

## Why This Project Matters

For internship, new grad, and backend/systems roles, this repo demonstrates more than syntax. It shows the ability to take a compact but failure-prone codebase and turn it into a cleaner, testable engine with explicit trade-offs, measurable behavior, and documentation that matches reality.

It is also a strong stepping stone for deeper systems work: market data fan-out, persistence, order modification, IOC/FOK handling, or a lock-aware ingress layer around the matching core.

## Future Improvements

- IOC/FOK and post-only order policies
- Snapshot serialization and replayable input streams
- Market data feed generation from execution events
