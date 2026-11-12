# TradeMatch Engine for Python

Python bindings for [TradeMatch Engine](https://github.com/AdrianTrill/TradeMatch-Engine), a C++20 price-time-priority
order matching engine. They cover the order book itself, the engine's agent-based market simulator, and replay of
NASDAQ TotalView-ITCH 5.0 files. Prices are integer ticks: cents in the simulator, and 1/10,000 of a dollar in ITCH.

```bash
pip install .            # from a clone of the repository; builds the C++ extension with CMake
pip install ".[test]"    # with pytest, for python -m pytest
```

## The order book

```python
import tradematch as tm

book = tm.OrderBook()
book.submit(1, tm.Side.SELL, 100, 10_050)                         # rests: nothing to trade with
result = book.submit(2, tm.Side.BUY, 160, 10_050, time_in_force=tm.TimeInForce.IOC)
print(result.filled_quantity, result.trades)                       # 100 [Trade(100 @ 10050, buy=2, sell=1)]

book.submit(3, tm.Side.SELL, 300, 10_060, display_quantity=100)   # an iceberg showing 100 at a time
print(book.depth(5))                                               # ([], [(10060, 100, 1)])
```

Every order type and rule of the engine is there: market, stop, and stop-limit orders, IOC, FOK, and DAY time in
force, post-only orders, icebergs, self-trade prevention, replaces that keep their place in the queue, and call
auctions (`begin_auction()`, `indicative_auction()`, `uncross()`).

## The market simulator

```python
import pandas as pd

simulator = tm.MarketSimulator(seed=42, symbols=["ACME", "GLOBEX"], continuous_seconds=600)
simulator.run_day()                                    # an opening auction, ten minutes of trading, and a close
trades = pd.DataFrame(simulator.take_trades(),
                      columns=["time", "symbol", "price", "quantity", "aggressor", "buy_order_id", "sell_order_id"])
quotes = pd.DataFrame(simulator.take_quotes(),
                      columns=["time", "symbol", "phase", "bid", "bid_size", "ask", "ask_size", "fair"])
```

The simulator records every trade, with the side that took liquidity ('B', 'S', or 'X' for an auction) and the orders
on both sides. After every 10 ms step it records each symbol's top of book and the fair value the market makers quote
around, which the other agents can't see. A seed always replays the same market, and the numbers of market makers,
noise traders, and momentum traders, the volatility, and the length of each part of the day are all arguments, so
experiments are a loop away. `step()`, `run()`, and `run_day()` release the GIL, so simulators in separate threads run
in parallel.

## ITCH replay

```python
replica = tm.ItchReplica("20191230.BX_ITCH_50", watch=["SPY"])
replica.advance("10:00")                          # every book as of 10:00
print(replica.top_of_book("SPY"), replica.depth("SPY", levels=5))
replica.advance("16:00")                          # carries on from 10:00
spy_trades = replica.take_trades()                # (time, symbol, price, shares, aggressor) for the watched stocks
replica.advance()                                 # to the end of the file
print(replica.stats["anomalies"], replica.stock_stats("SPY"))
```

NASDAQ publishes historical ITCH files at [emi.nasdaq.com/ITCH](https://emi.nasdaq.com/ITCH/). The replica rebuilds
every stock's book order by order, and counts any message that doesn't fit the book it applies to. Times are
nanoseconds since midnight, or strings such as `"10:00:30.5"`, and `tm.time_of_day()` converts the one to the other.
A trade's aggressor is the side opposite the displayed order it executes; trades with hidden orders ('?') and crosses
('X') don't show one.

The [market microstructure notebook](https://github.com/AdrianTrill/TradeMatch-Engine/blob/main/notebooks/market_microstructure.ipynb)
uses all of this to measure the spread, how queue imbalance predicts the next price move, how order flow persists, and
how trades move prices, in the simulator and on a real day of NASDAQ data.
