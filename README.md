# TradeMatch Engine

[![CI](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/ci.yml)
[![Fuzz](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/fuzz.yml/badge.svg?branch=main)](https://github.com/AdrianTrill/TradeMatch-Engine/actions/workflows/fuzz.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
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
- Stop and stop-limit orders, triggered by the last trade price, with cascades
- Iceberg orders that display one tranche at a time
- Time in force: GTC, DAY (with end-of-day expiry), IOC, and FOK
- Post-only orders that are rejected rather than take liquidity
- Self-trade prevention: cancel newest, cancel oldest, or cancel both
- Partial fills
- Price-time priority
- Trade execution reporting
- Cancel by order ID
- Replace by order ID, keeping time priority when only the quantity goes down
- Resting-order lookup by order ID
- Multiple instruments through `MatchingEngine`, with one book per symbol
- Market data: L2 depth updates, top of book, trade tape, OHLCV candles, VWAP, and a gap-free sequenced feed
- Machine-readable reject reasons for every rejected request
- Zero heap allocations on the request path once capacity is reserved
- Deterministic benchmark/demo scenarios

## API Overview

Core public API:

- `submit(OrderRequest)` submits a new order and returns fills, remaining quantity, and any generated trades.
- `cancel(OrderId)` removes a resting order by ID and reports the cancelled quantity.
- `replace(OrderRequest)` modifies an active order. At the same price with the same or a smaller quantity, it amends in place and keeps time priority (`priority_kept`). Otherwise it cancels and resubmits under the same ID. The replacement must keep the side and the limit type, and use a time in force that can rest. Its quantity is the new open quantity.
- `expire_day_orders()` runs end-of-day processing: it cancels every resting or pending DAY order and returns their IDs.
- `has_stop_order(OrderId)`, `stop_order_count()`, and `last_trade_price()` expose the stop book and its trigger reference.

`OrderRequest` grows with optional fields, all defaulted, so `{id, side, type, quantity, price}` still works: `time_in_force`, `post_only`, `stop_price`, `display_quantity` (iceberg tranche), `owner`, and `self_trade_prevention`.
- `get_order(OrderId)` returns the current resting state for an active order.
- `get_orders_at_level(Side, Price)` exposes FIFO order state for a price level.
- `best_bid()` / `best_ask()` return the top of book, and `level_count(Side)` the number of price levels.
- `snapshot()` returns visible bid/ask levels for inspection and tests.

Results are plain values that never own heap memory:

- `trades` is a `std::span<const Trade>` over a buffer the book reuses, valid until the book's next `submit` or `replace`. Copy the trades out if you need them longer.
- `message` is a `std::string_view` of a static string.
- `triggered_stops` and `self_trade_cancels` are spans of the stop orders the request activated and the resting orders that self-trade prevention removed. `pending_trigger` and `self_trade_prevented` flag the corresponding outcomes for the request itself.
- `reject_reason` is a `RejectReason` enum for programmatic handling, for example `DuplicateOrderId`, `PostOnlyWouldCross`, or `InvalidDisplayQuantity`.
- `RestingOrderView` reports `remaining_quantity` (total open, including any hidden iceberg reserve), `visible_quantity`, `display_quantity`, `time_in_force`, and `owner`.

`OrderBook` takes an optional `BookCapacity` (orders, price levels per side, trades per call). Pools grow automatically, but sizing them for the expected peak keeps the request path free of allocator calls.

Minimal usage:

```cpp
tradematch::OrderBook book(tradematch::BookCapacity{.orders = 100'000, .price_levels = 4'096});

auto add = book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10050});
auto replace = book.replace({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 80, 10075});
auto cancel = book.cancel(1);
auto snapshot = book.snapshot();
```

### Multiple instruments and market data

`MatchingEngine` owns one `OrderBook` per symbol and routes requests by `SymbolId`. Requests for unknown symbols are rejected with `UnknownSymbol`. Attaching a `MarketDataListener` turns on market data for every book: after each request, a book reports the trades it executed, then one `LevelUpdate` per price level it changed, carrying that level's final displayed quantity and order count (0 means the level is gone). Because every changed level is reported with its final state, a consumer can keep an exact copy of the book's depth from the updates alone. With no listener attached, books skip the bookkeeping entirely.

`MarketDataFeed` is a ready-made listener that maintains, per symbol:

- a `DepthBook` replica (L2 depth) and top of book
- a tape of recent trades, the last price, traded volume, and VWAP
- OHLCV `Candle`s over a configurable interval, timestamped by the caller's clock (real or simulated)
- optionally, a log of `FeedMessage`s with gap-free sequence numbers, as a downstream feed handler would receive them

```cpp
tradematch::MatchingEngine engine;
tradematch::MarketDataFeed feed;
engine.set_listener(&feed);

const auto acme = engine.add_symbol("ACME");
feed.set_time(now_ns);
engine.submit(acme, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 100, 10050});

const auto top = feed.top_of_book(acme);              // best bid/ask with sizes
const auto depth = feed.depth(acme).levels(tradematch::Side::Sell, 5);
const auto& bars = feed.candles(acme).candles();      // OHLCV
```

```text
requests ──► MatchingEngine ──► OrderBook per symbol ──► BookListener ──► MarketDataFeed
                                 (zero-allocation core)   trades,          ├─ DepthBook (L2 replica) + top of book
                                                          level updates    ├─ trade tape, last price, volume, VWAP
                                                                           ├─ OHLCV candles
                                                                           └─ sequenced FeedMessage log
```

## Matching Rules

The engine follows standard price-time priority:

- Buy orders match the lowest available ask first.
- Sell orders match the highest available bid first.
- At the same price level, older resting orders fill before newer ones.
- Trades execute at the resting order's price.
- Market orders consume available liquidity and never rest in the book. Unfilled market quantity expires immediately.
- IOC orders trade what they can at their limit, and any remainder is cancelled instead of resting.
- FOK orders trade their full quantity immediately or do nothing. The engine sums level totals across the crossing price range before touching the book, so a killed FOK leaves no trace.
- Post-only orders are rejected with `PostOnlyWouldCross` if they would take liquidity. They must be limit orders with a time in force that can rest.
- DAY orders behave like GTC until `expire_day_orders()` runs.
- **Stops**: a buy stop triggers when the last trade price rises to its stop price, and a sell stop when it falls to it. A stop becomes a market order (`Stop`) or a limit order (`StopLimit`). Stops wait in a separate stop book and never appear in depth. A stop whose trigger is already met on arrival activates immediately. Triggered stops activate one at a time within the request that caused the trade. Each activation can trade, move the price, and trigger more stops, until nothing more triggers. On each side, the stop closest to the market goes first, then the earliest arrival. If both sides trigger at once, which can only happen on a book's first trade, the earlier arrival goes first. The request's `trades` include every cascaded trade.
- **Icebergs**: an order with `display_quantity` shows one tranche at a time. Depth and `visible_quantity` report only the tranche. When a tranche is consumed, the next one is displayed and joins the back of the queue with a new sequence number, so hidden quantity never jumps ahead of displayed orders. An aggressor can sweep through refreshed tranches at the same price in one request. Hidden quantity counts toward FOK.
- **Self-trade prevention**: when an incoming order with a non-zero `owner` reaches a resting order with the same owner, its `self_trade_prevention` setting decides. `CancelOldest` cancels the resting order and keeps matching. `CancelNewest` cancels the incoming remainder. `CancelBoth` does both. The FOK pre-check follows the same rules, so a FOK that would be stopped by its own order is killed before touching the book.
- Replace keeps time priority when it only reduces quantity at the same price, as exchanges do, because a smaller order cannot disadvantage the orders queued behind it. A price change or a larger quantity is cancel plus resubmit, so the order moves to the back of the queue at its new price.
- Replace may never change the side or the limit order type, or move to IOC or FOK. Rejected requests leave the resting order untouched.
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

The book is index based: every structure is a flat array addressed by 32-bit slots, so nothing is allocated per order.

```text
 asks ladder (best at back)        price level (pooled)                 order nodes (pooled)
 ┌──────────┬────────┐            ┌────────────────────────┐          ┌───────┐   ┌───────┐   ┌───────┐
 │ 101.03   │ slot 7 │            │ price 101.01           │  head ──►│  #42  │◄─►│  #57  │◄─►│  #61  │◄── tail
 │ 101.02   │ slot 2 │            │ 900 shares, 3 orders   │          │  300  │   │  500  │   │  100  │
 │ 101.01   │ slot 4 │ ─────────► │ head slot, tail slot   │          └───────┘   └───────┘   └───────┘
 └──────────┴────────┘            └────────────────────────┘           intrusive FIFO list: time priority
   best ask ▲

 order index: open-addressing hash table, order ID ──► node slot (O(1) cancel and replace)
```

- **Order pool**: resting orders live in one `std::vector<OrderNode>` (48 bytes each) and are linked into an intrusive doubly linked FIFO list per price level. Freed slots go on a free list and are reused, so steady-state traffic never touches the allocator.
- **Price ladders**: each side keeps `{price, level slot}` entries in a vector sorted so the best price is at the back. Bids are ascending and asks descending. Taking or removing the best level is `pop_back`, joining the best price is a one-comparison fast path, and new levels, which usually appear near the touch, shift only a handful of entries.
- **Order index**: a purpose-built open-addressing hash table ([order_index.hpp](include/tradematch/detail/order_index.hpp)) with linear probing, Fibonacci hashing, and backward-shift deletion. It maps an order ID to its node slot in one or two cache lines, and heavy cancel traffic never leaves tombstones behind.
- **Trade buffer**: trades are appended to a reusable vector and returned as a span. The same applies to triggered-stop and self-trade-cancel lists.
- **Stop book**: pending stops live in their own pool and hash index. Each side has a vector sorted so the next stop to trigger sits at the back, so checking for triggers after a trade is O(1) per side.
- **Iceberg reserves**: each price level tracks displayed and hidden totals separately. Depth uses the displayed total, and FOK sums both without walking orders.

Cancel is O(1): an index lookup, then unlinking the node from its level. Only when a level empties does the ladder entry get erased.

### Design Tradeoffs

- The engine is single-threaded by design. That keeps sequencing deterministic and makes correctness easy to inspect. Concurrency belongs around the engine, in the gateway and market data layers, not inside the matching loop.
- The original engine (`std::map` levels, `std::list` queues, `std::unordered_map` index) is kept in [bench/baseline](bench/baseline) as the "before" in benchmarks. It was easy to read but allocated 1.83 times per operation, and those allocations dominated its tail latency.
- Sorted vectors beat trees for price levels because books are shallow and activity concentrates near the touch. The worst case (a new level deep in a very wide book) costs a short `memmove`, never an allocation.
- Rarely used features carry explicit hot/cold hints ([config.hpp](include/tradematch/detail/config.hpp)). Validation of stops, icebergs, and post-only, the FOK walk, self-trade handling, iceberg refresh, and stop processing are compiled out of line, and the small helpers that run on every request are forced inline. Without these hints, adding the features pushed `validate` and node removal out of line and cost about 18% throughput. With them, the features cost about 10% throughput, and the mean latency is within 1 ns of the engine without them.
- Returning trades as a span trades a small amount of API convenience (the view is invalidated by the next call) for zero allocations per request.
- Replace has two paths with explicit priority semantics. An in-place amend is O(1) and keeps the queue position. A price change or a size increase is cancel plus resubmit, which is easy to reason about and makes the priority reset visible in the result (`priority_kept`).

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
│   ├── book_listener.hpp
│   ├── detail/
│   │   ├── config.hpp
│   │   └── order_index.hpp
│   ├── market_data.hpp
│   ├── matching_engine.hpp
│   ├── order_book.hpp
│   └── types.hpp
├── src/
│   ├── formatting.cpp
│   ├── market_data.cpp
│   ├── matching_engine.cpp
│   └── order_book.cpp
├── bench/
│   ├── baseline/
│   │   ├── map_order_book.cpp
│   │   └── map_order_book.hpp
│   ├── benchmark_main.cpp
│   ├── latency_histogram.hpp
│   ├── system_info.hpp
│   ├── workload.cpp
│   └── workload.hpp
├── tests/
│   ├── allocation_tests.cpp
│   ├── bench_tests.cpp
│   ├── differential_tests.cpp
│   ├── market_data_tests.cpp
│   ├── order_book_tests.cpp
│   ├── order_index_tests.cpp
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

- A C++20 compiler (GCC 11+, Clang 16+, Apple Clang 15+, or MSVC 19.30+)
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

The demo executable (`./build/tradematch_demo`) walks through a deterministic scenario:

1. Two buy limit orders rest at 100.50.
2. Order 1 is reduced from 100 to 80 at the same price. It is amended in place and keeps its place at the front of the queue.
3. A sell order for 120 fills order 1 first (80), then order 2 (40).
4. A post-only ask rests at 101.00, and a post-only bid at 101.00 is rejected instead of trading.
5. A fill-or-kill bid for 80 is killed because only 50 is offered, and nothing trades.
6. A market order takes 40, and an IOC bid for 30 takes the last 10. Its remaining 20 is cancelled.
7. The leftover bid is cancelled by order ID.
8. An iceberg ask shows 20 of its 100 shares, and a buy stop waits at 101.50. A buy for 30 takes the first tranche and part of the next. Its trades at 101.50 trigger the stop, whose market order takes 30 more from the refreshed tranches.

Sample output excerpt:

```text
Replace order 1 side=BUY type=LIMIT qty=80 price=100.50
  replaced=true priority_kept=true previous_remaining=100
  Order quantity reduced in place; time priority kept.

Submit order 3 side=SELL type=LIMIT qty=120 price=100.50
  accepted=true filled=120 remaining=0 rested=false expired=false
  Order fully filled.
  trade#1 buy=1 sell=3 qty=80 price=100.50 aggressor=SELL
  trade#2 buy=2 sell=3 qty=40 price=100.50 aggressor=SELL

Submit order 5 side=BUY type=LIMIT qty=10 price=101.00 post_only
  accepted=false filled=0 remaining=10 rested=false expired=false reject=POST_ONLY_WOULD_CROSS
  Post-only order would take liquidity and was rejected.

Submit order 6 side=BUY type=LIMIT qty=80 price=101.00 tif=FOK
  accepted=true filled=0 remaining=80 rested=false expired=true
  Fill-or-kill order could not be filled in full and was cancelled.

Submit order 8 side=BUY type=LIMIT qty=30 price=101.00 tif=IOC
  accepted=true filled=10 remaining=20 rested=false expired=true
  Immediate-or-cancel order filled what it could; the remainder was cancelled.
  trade#4 buy=8 sell=4 qty=10 price=101.00 aggressor=BUY

Submit order 11 side=BUY type=LIMIT qty=30 price=101.50
  accepted=true filled=30 remaining=0 rested=false expired=false
  Order fully filled.
  trade#5 buy=11 sell=9 qty=20 price=101.50 aggressor=BUY
  trade#6 buy=11 sell=9 qty=10 price=101.50 aggressor=BUY
  trade#7 buy=10 sell=9 qty=10 price=101.50 aggressor=BUY
  trade#8 buy=10 sell=9 qty=20 price=101.50 aggressor=BUY
  triggered stop order 10
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
- reject reasons for every validation failure, unknown order, and illegal replace
- amend-down keeping time priority, including the level total and sequence number
- IOC partial fills and empty IOCs never resting
- FOK all-or-nothing across multiple levels, for limit and market orders
- post-only resting, rejection when crossing, invalid post-only combinations, and crossing post-only replacements
- DAY expiry returning IDs in order and removing emptied levels, while GTC orders survive
- stops that wait, trigger on the last trade, trigger immediately when already through the market, cascade in price-then-time order, break first-trade ties by arrival, cancel, expire, and validate
- icebergs that display one tranche, refresh to the back of the queue, count toward FOK, validate, and amend in place
- every self-trade prevention mode, including FOK orders that would be stopped by their own resting order
- a book that starts with room for one order, one level, and one trade, then grows transparently through a 1,000-trade sweep
- invariant checks after every submit, cancel, and replace

The market data suite ([tests/market_data_tests.cpp](tests/market_data_tests.cpp)) runs the full random flow (stops, icebergs, self-trade prevention, amends, and expiries) through a book with a listener attached. After every one of 80,000 operations, it checks that a depth replica built only from the published updates equals the engine's own snapshot, and that the streamed trades equal the request's trades. It also covers candle aggregation and interval alignment (including negative times), top of book, VWAP, tape length, gap-free sequencing and timestamps, symbol routing and isolation, unknown symbols, cross-symbol day expiry, and attaching or detaching listeners.

Two more suites target the v2 internals:

- **Zero-allocation proof** ([tests/allocation_tests.cpp](tests/allocation_tests.cpp)): replaces global `operator new` to count every heap allocation, replays the benchmark's 1,000,000-operation stream after warm-up, and fails if the engine allocates even once. The v1 baseline is counted on the same stream for comparison: 1.83 million allocations, against 0 for v2.
- **Order index** ([tests/order_index_tests.cpp](tests/order_index_tests.cpp)): 300,000 random inserts and erases over a small key space, checked against `std::unordered_map`. The small key space forces long probe runs, wrap-around, and constant backward-shift deletions. Further cases cover growth, reserved capacity, and the reserved key 0.

The differential test also starts every other seed with a book sized for a single order, level, and trade, so the pools and the index grow constantly while being checked.

The benchmark tooling has its own tests ([tests/bench_tests.cpp](tests/bench_tests.cpp)). They check that histogram percentiles stay within the documented error bound against exact sorted percentiles, that the workload generator is deterministic per seed, and that every generated stream replays with zero rejected commands.

## Differential Testing and Fuzzing

Hand-written scenarios only cover the cases someone thought of. To catch the rest, the engine is checked against a second, deliberately naive implementation ([tests/support/reference_book.hpp](tests/support/reference_book.hpp)). The reference keeps every resting order in one flat vector and finds the best match with a linear scan, so it is slow but easy to verify by reading.

- **Randomized differential test** (`tradematch_differential_tests`): runs 400,000 adversarial operations (200 seeds x 2,000) through both books. The flow uses a narrow price band so orders constantly cross, small sizes so partial fills are common, and a steady stream of invalid, duplicate, and unknown-ID requests. After every operation it compares result flags, quantities, every trade field, the full depth snapshot including FIFO order IDs, and order lookups. A failure prints the seed, the diverging operation, and a one-line reproduction command.
- **Coverage-guided fuzzing** ([fuzz/order_book_fuzzer.cpp](fuzz/order_book_fuzzer.cpp)): a libFuzzer target decodes raw bytes into order flow and aborts on the first engine/reference mismatch. CI fuzzes it for 60 seconds under AddressSanitizer and UndefinedBehaviorSanitizer and also runs a 10-million-operation differential soak. A standalone driver keeps the target compiling and smoke-tested on toolchains without libFuzzer, such as Apple clang and MSVC.

The random flow covers every time-in-force and post-only combination, stop and stop-limit orders around the trading band, icebergs, and orders from a small pool of owners with random self-trade prevention modes. The reference book shares none of the engine's algorithms. For example, its FOK check simply runs the real match on a throwaway copy of the whole book, which checks the engine's single-pass FOK walk against the real matching path. Half of all replacements target an order's current price with the same or a smaller size, to exercise in-place amends, and end-of-day expiries are interleaved, with the expired ID lists compared.

The harness has been checked by mutation testing. Each injected bug was caught within the first 1,000 operations, most within a few hundred:

- trading at the aggressor's price
- an off-by-one in the limit-price check
- LIFO instead of FIFO queue order
- killing a FOK that exactly fits
- a same-size amend losing priority
- an amend that forgets to update the level total
- a post-only replacement allowed to cross
- day expiry also removing GTC orders
- an IOC remainder that rests
- a refreshed iceberg keeping its queue position or its old sequence number
- a FOK that ignores iceberg reserves, or counts the order's own liquidity under cancel-oldest
- cancel-oldest stopping the match
- a buy stop that triggers only strictly above its stop price
- an iceberg amend that shrinks the displayed part before the reserve

The cross-side stop ordering cannot arise in random flow except on a book's first trade, so it is pinned by a dedicated unit test.

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

`tradematch_benchmark` replays one realistic order stream through both engines: the original `std::map`/`std::list` design (v1, kept in [bench/baseline](bench/baseline)) and the allocation-free v2 engine. It reports per-operation latency percentiles, not just average throughput.

Results on an Apple M4 (Apple clang 21, Release build, median of 5 runs):

| metric | v1 baseline | v2 | improvement |
| --- | ---: | ---: | ---: |
| throughput | 11.8 M ops/s | **54.6 M ops/s** | **4.6x** |
| mean latency | 80.4 ns | **19.1 ns** | **4.2x** |
| p99.9 | 919 ns | **84 ns** | **10.9x** |
| p99.99 | 3,759 ns | **209 ns** | **18.0x** |
| heap allocations per operation | 1.83 | **0** | |

v2 per-operation percentiles (1,000,000 measured operations, 138,242 trades):

| operation | count | p50 | p90 | p99 | p99.9 | p99.99 | mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| add | 459,626 | <41 ns | 42 ns | 42 ns | 84 ns | 208 ns | 20.5 ns |
| cancel | 390,716 | <41 ns | 42 ns | 42 ns | 84 ns | 208 ns | 15.4 ns |
| modify | 80,077 | 41 ns | 42 ns | 42 ns | 125 ns | 293 ns | 25.8 ns |
| aggressive | 69,581 | 41 ns | 42 ns | 83 ns | 125 ns | 293 ns | 23.3 ns |
| **all** | **1,000,000** | **<41 ns** | **42 ns** | **42 ns** | **84 ns** | **209 ns** | **19.1 ns** |

The biggest gain is in the deep tail. v1 allocates map nodes, list nodes, a trade vector, and a message string on most requests, and every trip into the allocator risks a slow path. v2 never calls the allocator, so its p99.99 is 18 times lower. v2 supports stops, icebergs, self-trade prevention, and every time in force while staying allocation-free, but the stream uses only the limit and market orders that both engines support. That keeps the comparison like for like.

**Workload.** A generator produces a deterministic stream that resembles real exchange traffic. Passive limit orders cluster near the touch, at a geometric distance behind the best price. Cancels and modifies always target orders that are still live. Aggressive orders are marketable limits that can sweep several levels, or market orders. The book is prefilled with 10,000 resting orders, and the flow keeps it near that depth, so measurements reflect a steady state rather than a growing book. The realized mix is 46% add, 39% cancel, 8% modify, and 7% aggressive, close to the message proportions of public exchange feeds.

**Method.** The whole stream is generated before timing starts, so generator cost never reaches the measured loop and both engines replay identical input. The benchmark aborts if the two engines report different trade or resting-order counts, so the comparison is guaranteed to measure the same work. After 100,000 warm-up operations, each of the 1,000,000 measured operations is timed individually and recorded in an HDR-style log-linear histogram. Its reported percentiles are never below the true value and at most 0.8% above it. Throughput comes from a second replay with no clock reads in the loop.

Notes for reading the numbers:

- Apple Silicon's user-space clock ticks at 24 MHz (about 41 ns), and most v2 operations finish within a single tick. Percentiles below one tick print as `<41 ns`, and the benchmark doesn't compute ratios for percentiles within a tick of zero. The mean is still accurate: it averages samples taken at random phases of the tick. Linux on x86 reports nanosecond resolution.
- Every sample includes one clock read, about 12 ns, which flatters neither engine.
- p99.9 and below are stable across runs. The maximum mostly reflects operating-system noise (interrupts and preemption) on a desktop machine.

## Continuous Integration

Every push runs two workflows:

| Workflow | Job | What it checks |
| --- | --- | --- |
| CI | Linux / GCC, Linux / Clang, macOS / Apple Clang, Windows / MSVC | Release build with warnings as errors, then the full test suite |
| CI | Sanitizers | Every test under AddressSanitizer (with leak detection) and UndefinedBehaviorSanitizer |
| CI | Format | `clang-format --dry-run --Werror` on every source file |
| CI | Lint | `clang-tidy` with the bugprone, performance, modernize, readability, and clang-analyzer families as errors |
| CI | Benchmark smoke test | `tradematch_benchmark` runs a short workload under CTest on every platform |
| CI | Coverage | Engine line coverage must stay at or above 95% (currently 98% of lines and 96% of branches); the HTML report is uploaded as an artifact and the summary appears on the run page |
| Fuzz | Differential soak | 10 million random operations compared against the reference book |
| Fuzz | libFuzzer | Coverage-guided fuzzing under ASan + UBSan: 60 seconds per push and 20 minutes nightly. The corpus is cached between runs, so coverage keeps growing |

## Why This Project Matters

For internship, new grad, and backend/systems roles, this repo demonstrates more than syntax. It shows the ability to take a compact but failure-prone codebase and turn it into a cleaner, testable engine with explicit trade-offs, measurable behavior, and documentation that matches reality.

It is also a strong stepping stone for deeper systems work: market data fan-out, persistence, or a lock-aware ingress layer around the matching core.

## Future Improvements

- Snapshot serialization and replayable input streams
