#include "support/differential.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tradematch::OrderType;
using tradematch::Side;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

// Applies one book's events to a depth replica and keeps the streamed trades for comparison.
class ReplicaListener final : public tradematch::BookListener {
public:
    void on_trade(const tradematch::Trade& trade) override {
        trades.push_back(trade);
    }
    void on_level_update(const tradematch::LevelUpdate& update) override {
        replica.apply(update);
        ++updates;
    }

    tradematch::DepthBook replica;
    std::vector<tradematch::Trade> trades;
    std::size_t updates{0};
};

bool same_trades(std::span<const tradematch::Trade> left, const std::vector<tradematch::Trade>& right) {
    return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
        return a.trade_id == b.trade_id && a.buy_order_id == b.buy_order_id && a.sell_order_id == b.sell_order_id &&
               a.price == b.price && a.quantity == b.quantity && a.aggressor_side == b.aggressor_side;
    });
}

void expect_replica_matches(const tradematch::OrderBook& book, const tradematch::DepthBook& replica, std::size_t step) {
    const auto snapshot = book.snapshot();
    const auto compare = [&](const std::vector<tradematch::PriceLevelView>& expected, Side side) {
        const auto actual = replica.levels(side);
        expect(actual.size() == expected.size(), "Replica level count differs at step " + std::to_string(step));
        for (std::size_t index = 0; index < actual.size(); ++index) {
            expect(actual[index].price == expected[index].price &&
                       actual[index].quantity == expected[index].total_quantity &&
                       actual[index].order_count == expected[index].order_count,
                   "Replica level differs at step " + std::to_string(step));
        }
    };
    compare(snapshot.bids, Side::Buy);
    compare(snapshot.asks, Side::Sell);
}

void test_depth_replica_tracks_the_engine_exactly() {
    using tradematch::testing::OpKind;
    std::size_t total_updates = 0;

    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        tradematch::OrderBook book;
        tradematch::testing::ReferenceBook generator_view;  // only used to aim replacements, like the differential test
        tradematch::testing::OperationGenerator generator(seed);
        ReplicaListener listener;
        book.set_listener(&listener);

        for (std::size_t step = 0; step < 2'000; ++step) {
            const auto op = generator.next(generator_view);
            listener.trades.clear();
            std::span<const tradematch::Trade> result_trades;
            switch (op.kind) {
                case OpKind::Submit:
                    result_trades = book.submit(op.order).trades;
                    generator_view.submit(op.order);
                    break;
                case OpKind::Cancel:
                    book.cancel(op.order.order_id);
                    generator_view.cancel(op.order.order_id);
                    break;
                case OpKind::Replace:
                    result_trades = book.replace(op.order).submit_result.trades;
                    generator_view.replace(op.order);
                    break;
                case OpKind::ExpireDay:
                    book.expire_day_orders();
                    generator_view.expire_day_orders();
                    break;
                case OpKind::BeginAuction:
                    book.begin_auction();
                    generator_view.begin_auction();
                    break;
                case OpKind::Uncross:
                    result_trades = book.uncross().trades;
                    generator_view.uncross();
                    break;
            }
            expect(same_trades(result_trades, listener.trades), "Streamed trades should equal the result's trades.");
            expect_replica_matches(book, listener.replica, step);
        }
        total_updates += listener.updates;
    }
    std::cout << "       " << total_updates << " level updates replayed into the replica\n";
    expect(total_updates > 20'000, "The flow should produce plenty of level updates.");
}

void test_detached_book_publishes_nothing() {
    tradematch::OrderBook book;
    ReplicaListener listener;
    book.set_listener(&listener);
    book.submit({1, Side::Buy, OrderType::Limit, 10, 10000});
    book.set_listener(nullptr);
    book.submit({2, Side::Sell, OrderType::Limit, 10, 10000});
    expect(listener.updates == 1U && listener.trades.empty(), "A detached book should stay silent.");
}

void test_candles_aggregate_trades_by_interval() {
    tradematch::CandleSeries series(1'000);
    series.add_trade(100, 10000, 5);
    series.add_trade(400, 10100, 3);
    series.add_trade(999, 9950, 2);
    series.add_trade(1'000, 10000, 1);
    series.add_trade(3'500, 10200, 4);

    const auto& candles = series.candles();
    expect(candles.size() == 3U, "Three intervals had trades; the empty one produces no bar.");
    expect(candles[0] == tradematch::Candle{0, 10000, 10100, 9950, 9950, 10, 3}, "First bar should be exact.");
    expect(candles[1] == tradematch::Candle{1'000, 10000, 10000, 10000, 10000, 1, 1}, "Boundary trades open a bar.");
    expect(candles[2].start == 3'000 && candles[2].volume == 4U, "Later bars align to the interval.");

    tradematch::CandleSeries negative(1'000);
    negative.add_trade(-1, 10000, 1);
    expect(negative.candles().front().start == -1'000, "Negative times floor to the interval below.");

    bool threw = false;
    try {
        series.add_trade(0, 10000, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "Out-of-order trades are rejected.");
}

void test_feed_builds_views_and_sequences_messages() {
    tradematch::MatchingEngine engine;
    tradematch::FeedConfig config;
    config.tape_length = 2;
    config.record_messages = true;
    tradematch::MarketDataFeed feed(config);
    engine.set_listener(&feed);
    const auto symbol = engine.add_symbol("ACME");

    feed.set_time(1'000);
    engine.submit(symbol, {1, Side::Sell, OrderType::Limit, 10, 10100});
    engine.submit(symbol, {2, Side::Sell, OrderType::Limit, 10, 10200});
    engine.submit(symbol, {3, Side::Buy, OrderType::Limit, 5, 9900});
    const auto top = feed.top_of_book(symbol);
    expect(top.bid_price == 9900 && top.bid_quantity == 5U, "Top of book should show the best bid.");
    expect(top.ask_price == 10100 && top.ask_quantity == 10U, "Top of book should show the best ask.");

    feed.set_time(2'000'000'000);
    engine.submit(symbol, {4, Side::Buy, OrderType::Market, 15, 0});
    expect(feed.last_price(symbol) == 10200, "The last trade price should follow the tape.");
    expect(feed.traded_volume(symbol) == 15U, "Traded volume should add up.");
    const auto vwap = feed.vwap(symbol).value_or(0.0);
    expect(vwap > 10133.0 && vwap < 10134.0, "VWAP should be (10 x 101.00 + 5 x 102.00) / 15.");
    expect(feed.recent_trades(symbol).size() == 2U, "The tape keeps only the configured number of trades.");
    expect(feed.candles(symbol).candles().size() == 1U && feed.candles(symbol).candles().front().start == 2'000'000'000,
           "Trades are bucketed with the feed's clock.");

    const auto messages = feed.drain_messages();
    expect(!messages.empty() && messages.back().sequence == feed.last_sequence(), "Messages are sequenced.");
    for (std::size_t index = 0; index < messages.size(); ++index) {
        expect(messages[index].sequence == index + 1, "Sequence numbers have no gaps.");
        expect(messages[index].symbol == symbol, "Messages carry their symbol.");
    }
    expect(messages.front().timestamp == 1'000 && messages.back().timestamp == 2'000'000'000,
           "Messages carry the time they were published.");
    expect(feed.drain_messages().empty(), "Draining clears the log.");
}

void test_engine_routes_symbols_independently() {
    tradematch::MatchingEngine engine;
    tradematch::MarketDataFeed feed;
    const auto acme = engine.add_symbol("ACME");
    engine.set_listener(&feed);  // attaching after a symbol exists must cover it too
    const auto beta = engine.add_symbol("BETA");

    expect(engine.add_symbol("ACME") == acme, "Adding a symbol twice returns the same ID.");
    expect(engine.find_symbol("BETA") == beta && !engine.find_symbol("NOPE"), "Symbols are found by name.");
    expect(engine.symbol_name(beta) == "BETA" && engine.symbol_count() == 2U, "Names round-trip.");

    engine.submit(acme, {1, Side::Sell, OrderType::Limit, 10, 10000});
    const auto other = engine.submit(beta, {1, Side::Buy, OrderType::Limit, 10, 10000});
    expect(other.accepted && other.trades.empty(), "Books are isolated, so the same ID and price do not trade.");
    expect(feed.top_of_book(acme).ask_price == 10000 && !feed.top_of_book(acme).bid_price, "ACME has only an ask.");
    expect(feed.top_of_book(beta).bid_price == 10000 && !feed.top_of_book(beta).ask_price, "BETA has only a bid.");

    const auto unknown = engine.submit(7, {2, Side::Buy, OrderType::Limit, 10, 10000});
    expect(unknown.reject_reason == tradematch::RejectReason::UnknownSymbol, "Unknown symbols are rejected.");
    expect(engine.cancel(7, 1).reject_reason == tradematch::RejectReason::UnknownSymbol, "Cancel checks the symbol.");
    expect(engine.replace(7, {1, Side::Buy, OrderType::Limit, 5, 10000}).reject_reason ==
               tradematch::RejectReason::UnknownSymbol,
           "Replace checks the symbol.");

    tradematch::OrderRequest day_order{2, Side::Buy, OrderType::Limit, 5, 9000};
    day_order.time_in_force = tradematch::TimeInForce::Day;
    engine.submit(beta, day_order);
    const auto expired = engine.expire_day_orders();
    expect(expired.size() == 1U && expired.front() == std::pair<tradematch::SymbolId, tradematch::OrderId>{beta, 2},
           "Day expiry runs across every symbol.");
    expect(feed.depth(beta).level_count(Side::Buy) == 1U, "The feed sees the expiry.");

    engine.set_listener(nullptr);
    engine.submit(acme, {3, Side::Buy, OrderType::Limit, 10, 10000});
    expect(feed.top_of_book(acme).ask_price == 10000, "A detached feed keeps its last state.");
    expect(engine.book(acme).order_count() == 0U, "The trade still happened in the engine.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"depth_replica_tracks_the_engine_exactly", test_depth_replica_tracks_the_engine_exactly},
        {"detached_book_publishes_nothing", test_detached_book_publishes_nothing},
        {"candles_aggregate_trades_by_interval", test_candles_aggregate_trades_by_interval},
        {"feed_builds_views_and_sequences_messages", test_feed_builds_views_and_sequences_messages},
        {"engine_routes_symbols_independently", test_engine_routes_symbols_independently},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures == 0U) {
        std::cout << "All market data tests passed (" << tests.size() << ").\n";
        return 0;
    }
    std::cout << failures << " test(s) failed.\n";
    return 1;
}

}  // namespace

int main() {
    try {
        return run_all_tests();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
