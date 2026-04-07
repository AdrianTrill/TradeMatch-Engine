# TradeMatch Engine

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
- Resting-order lookup by order ID
- Deterministic benchmark/demo scenarios

## Matching Rules

The engine follows standard price-time priority:

- Buy orders match the lowest available ask first.
- Sell orders match the highest available bid first.
- At the same price level, older resting orders fill before newer ones.
- Trades execute at the resting order's price.
- Market orders consume available liquidity and never rest in the book.
- Unfilled market quantity expires immediately.

## System Design

The matching core is intentionally single-threaded. That keeps sequencing deterministic and makes the behavior straightforward to test. Concurrency can be layered around the engine later, but the matching path itself should stay simple and predictable.

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

## Repository Layout

```text
.
├── CMakeLists.txt
├── Makefile
├── include/tradematch/
│   ├── order_book.hpp
│   └── types.hpp
├── src/
│   ├── formatting.cpp
│   └── order_book.cpp
├── tests/
│   └── order_book_tests.cpp
└── tools/
    ├── benchmark_main.cpp
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

CMake alternative:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

Demo:

```bash
./build/tradematch_demo
```

Benchmark:

```bash
./build/tradematch_benchmark
./build/tradematch_benchmark 500000
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
2. A sell limit order matches them in FIFO order.
3. A sell limit order rests on the ask side.
4. A buy market order consumes part of that liquidity.
5. The remaining ask is cancelled by order ID.

The output shows:

- whether the order was accepted
- how much filled
- whether any remainder rested or expired
- generated trades with buy/sell IDs, price, quantity, and aggressor side
- a book snapshot after each step

## Tests Included

The test suite covers deterministic scenarios that matter for correctness:

- exact match
- partial fill with resting remainder
- multiple matches across price levels
- price-time priority at the same price
- cancel order
- unmatched order resting in the book
- market-order expiry of any unfilled remainder
- invalid and duplicate-order rejection

## Why This Project Matters

For internship, new grad, and backend/systems roles, this repo demonstrates more than syntax. It shows the ability to take a compact but failure-prone codebase and turn it into a cleaner, testable engine with explicit trade-offs, measurable behavior, and documentation that matches reality.

It is also a strong stepping stone for deeper systems work: market data fan-out, persistence, order modification, IOC/FOK handling, or a lock-aware ingress layer around the matching core.

## Future Improvements

- Modify/replace order support
- IOC/FOK and post-only order policies
- Snapshot serialization and replayable input streams
- Latency histograms and richer benchmark reporting
- Market data feed generation from execution events
