import itertools
import statistics
import threading

import pytest

import tradematch as tm


def run_day(**options):
    simulator = tm.MarketSimulator(**options)
    simulator.run_day()
    return simulator, simulator.take_trades(), simulator.take_quotes()


def continuous_spreads(quotes):
    return [ask - bid for _, _, phase, bid, _, ask, _, _ in quotes if phase == "CONTINUOUS" and bid and ask]


def test_a_seed_replays_the_same_market():
    first, first_trades, first_quotes = run_day(seed=5, symbols=["ACME", "GLOBEX"])
    second, second_trades, second_quotes = run_day(seed=5, symbols=["ACME", "GLOBEX"])
    other, other_trades, _ = run_day(seed=6, symbols=["ACME", "GLOBEX"])
    assert first_trades == second_trades and first_quotes == second_quotes
    assert first.state_hash() == second.state_hash() and first.stats == second.stats
    assert other_trades != first_trades and other.state_hash() != first.state_hash()


def test_a_day_runs_through_its_auctions():
    simulator, trades, quotes = run_day(seed=3, symbols=["ACME", "GLOBEX"])
    assert simulator.session == "CLOSED" and simulator.day == 1
    assert [phase for phase, _ in itertools.groupby(quote[2] for quote in quotes)] == [
        "OPENING AUCTION",
        "CONTINUOUS",
        "CLOSING AUCTION",
        "CLOSED",
    ]
    # Every step records each symbol's quote: 98 simulated seconds of 10 ms steps.
    assert len(quotes) == 2 * 9_800 and simulator.now == 98 * 10**9
    assert simulator.stats["trades"] == len(trades) and simulator.stats["auctions"] == 4
    assert {trade[4] for trade in trades} == {"B", "S", "X"}
    auction_times = sorted({trade[0] for trade in trades if trade[4] == "X"})
    assert auction_times == [4 * 10**9, 98 * 10**9]
    assert [time for time, *_ in trades] == sorted(time for time, *_ in trades)


def test_trades_name_their_orders():
    _, trades, _ = run_day(seed=3)
    buyers = {trade[5] for trade in trades}
    sellers = {trade[6] for trade in trades}
    assert all(buy > 0 and sell > 0 and buy != sell for *_, buy, sell in trades)
    assert not buyers & sellers, "an order is either a buy or a sell"
    # An aggressive order takes all its fills within its own request, so its fills sit together on the tape.
    takers = [buy if aggressor == "B" else sell for *_, aggressor, buy, sell in trades if aggressor != "X"]
    runs = [taker for index, taker in enumerate(takers) if index == 0 or takers[index - 1] != taker]
    assert len(runs) == len(set(runs)) < len(takers)


def test_continuous_trading_never_leaves_the_book_crossed():
    _, _, quotes = run_day(seed=3)
    spreads = continuous_spreads(quotes)
    assert len(spreads) > 8_000 and min(spreads) > 0


def test_trades_agree_with_the_market_data_feed():
    simulator, trades, _ = run_day(seed=3, symbols=["ACME", "GLOBEX"])
    for symbol in simulator.symbols:
        candles = simulator.candles(symbol)
        tape = [(price, quantity) for _, name, price, quantity, *_ in trades if name == symbol]
        assert sum(quantity for _, quantity in tape) == sum(candle[5] for candle in candles)
        assert min(price for price, _ in tape) == min(candle[3] for candle in candles)
        assert max(price for price, _ in tape) == max(candle[2] for candle in candles)
        bid, bid_size, ask, ask_size = simulator.top_of_book(symbol)
        bids, asks = simulator.depth(symbol, 5)
        assert (bids[0][:2], asks[0][:2]) == ((bid, bid_size), (ask, ask_size))
        assert [level[0] for level in bids] == sorted((level[0] for level in bids), reverse=True)


def test_market_makers_keep_the_spread_tight():
    _, _, with_makers = run_day(seed=3)
    _, _, without_makers = run_day(seed=3, market_makers=0)
    assert statistics.mean(continuous_spreads(without_makers)) > 2 * statistics.mean(continuous_spreads(with_makers))


def aggressive_order_signs(trades):
    # +1 for a buy and -1 for a sell, once per aggressive order however many resting orders it fills.
    signs, last_taker = [], None
    for *_, aggressor, buy, sell in trades:
        taker = buy if aggressor == "B" else sell
        if aggressor != "X" and taker != last_taker:
            signs.append(1 if aggressor == "B" else -1)
            last_taker = taker
    return signs


def lag_one_autocorrelation(values):
    mean = statistics.fmean(values)
    centered = [value - mean for value in values]
    return statistics.fmean(a * b for a, b in zip(centered, centered[1:])) / statistics.fmean(c * c for c in centered)


def test_execution_traders_make_order_flow_persistent():
    _, sliced, _ = run_day(seed=3, record_quotes=False)
    _, unsliced, _ = run_day(seed=3, execution_traders=0, record_quotes=False)
    sliced_signs, unsliced_signs = aggressive_order_signs(sliced), aggressive_order_signs(unsliced)
    assert len(sliced_signs) > 2 * len(unsliced_signs), "most aggressive orders are execution traders' children"
    assert lag_one_autocorrelation(sliced_signs) > 0.4
    assert lag_one_autocorrelation(unsliced_signs) < 0.25


def test_records_are_taken_once():
    simulator = tm.MarketSimulator(seed=1, record_quotes=False)
    simulator.run(10.0)
    assert simulator.session == "CONTINUOUS" and simulator.now == 10 * 10**9
    assert simulator.take_trades() and not simulator.take_trades()
    assert simulator.take_quotes() == []
    simulator.step(5)
    assert simulator.now == 10 * 10**9 + 5 * 10**7


def test_bad_arguments_are_refused():
    with pytest.raises(ValueError):
        tm.MarketSimulator(symbols=[])
    for seconds in [0, -1.0, 1e-12, float("nan"), 1e10]:
        with pytest.raises(ValueError):
            tm.MarketSimulator(continuous_seconds=seconds)
    simulator = tm.MarketSimulator()
    for seconds in [-1.0, float("nan"), float("inf")]:
        with pytest.raises(ValueError):
            simulator.run(seconds)
    simulator.run(0.004)
    assert simulator.now == 0, "a run rounds to whole 10 ms steps"
    simulator.run(0.006)
    assert simulator.now == 10**7
    with pytest.raises(KeyError):
        simulator.top_of_book("NOPE")


def test_simulators_run_in_parallel_threads():
    results = {}

    def run(seed):
        simulator = tm.MarketSimulator(seed=seed, continuous_seconds=300)
        simulator.run_day()
        results[seed] = (simulator.state_hash(), len(simulator.take_trades()))

    threads = [threading.Thread(target=run, args=(seed,)) for seed in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for seed in range(4):
        simulator = tm.MarketSimulator(seed=seed, continuous_seconds=300)
        simulator.run_day()
        assert results[seed] == (simulator.state_hash(), len(simulator.take_trades()))


def test_a_busy_simulator_refuses_other_threads():
    simulator = tm.MarketSimulator(seed=1, continuous_seconds=7_200, record_quotes=False)
    errors = []
    runner = threading.Thread(target=simulator.run_day)
    runner.start()
    while runner.is_alive():
        try:
            simulator.top_of_book("ACME")
        except RuntimeError as error:
            errors.append(str(error))
    runner.join()
    assert errors and all("busy" in error for error in errors)
    assert simulator.session == "CLOSED"
