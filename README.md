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
- Replace by order ID
- Resting-order lookup by order ID
- Deterministic benchmark/demo scenarios

## API Overview

Core public API:

- `submit(OrderRequest)` submits a new order and returns fills, remaining quantity, and any generated trades.
- `cancel(OrderId)` removes a resting order by ID and reports the cancelled quantity.
- `replace(OrderRequest)` validates, cancels, and resubmits an active order using the same ID.
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
├── .github/workflows/ci.yml
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
- unmatched order resting in the book
- market-order expiry of any unfilled remainder
- invalid and duplicate-order rejection
- invariant checks after every submit, cancel, and replace

## Why This Project Matters

For internship, new grad, and backend/systems roles, this repo demonstrates more than syntax. It shows the ability to take a compact but failure-prone codebase and turn it into a cleaner, testable engine with explicit trade-offs, measurable behavior, and documentation that matches reality.

It is also a strong stepping stone for deeper systems work: market data fan-out, persistence, order modification, IOC/FOK handling, or a lock-aware ingress layer around the matching core.

## Future Improvements

- IOC/FOK and post-only order policies
- Snapshot serialization and replayable input streams
- Latency histograms and richer benchmark reporting
- Market data feed generation from execution events
