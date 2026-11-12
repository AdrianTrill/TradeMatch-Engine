#include "tradematch/journal.hpp"
#include "tradematch/simulator.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

constexpr tradematch::Timestamp kSecond = 1'000'000'000;

// FNV-1a over 64-bit values, byte by byte, so the result does not depend on the platform's std::hash.
class Fingerprint {
public:
    void add(std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash_ ^= (value >> (8 * byte)) & 0xFFU;
            hash_ *= 0x100000001B3U;
        }
    }

    std::uint64_t value() const {
        return hash_;
    }

private:
    std::uint64_t hash_{0xCBF29CE484222325U};
};

tradematch::SimulationConfig short_day(std::uint64_t seed) {
    tradematch::SimulationConfig config;
    config.seed = seed;
    config.opening_auction = 2 * kSecond;
    config.continuous = 20 * kSecond;
    config.closing_auction = 2 * kSecond;
    config.closed = kSecond;
    config.measure_latency = false;
    return config;
}

// A file in the temporary directory, removed when it goes out of scope.
class TempFile {
public:
    TempFile()
        : path_(std::filesystem::temp_directory_path() /
                ("tradematch-simulator-test-" + std::to_string(std::random_device{}()) + ".tmj")) {}
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// The signs of aggressive orders in continuous trading, +1 for a buy and -1 for a sell, each order counted once
// however many resting orders it fills, as the market microstructure notebook counts them.
class AggressorSigns final : public tradematch::OrderFeedListener {
public:
    void on_order_events(tradematch::SymbolId /*symbol*/,
                         std::span<const tradematch::OrderEvent> /*events*/,
                         std::span<const tradematch::Trade> trades) override {
        for (const tradematch::Trade& trade : trades) {
            const bool buy = trade.aggressor_side == tradematch::Side::Buy;
            const tradematch::OrderId taker = buy ? trade.buy_order_id : trade.sell_order_id;
            if (!trade.auction && taker != last_taker_) {
                signs_.push_back(buy ? 1.0 : -1.0);
                last_taker_ = taker;
            }
        }
    }

    const std::vector<double>& signs() const {
        return signs_;
    }

    double autocorrelation(std::size_t lag) const {
        double mean = 0.0;
        for (const double sign : signs_) {
            mean += sign;
        }
        mean /= static_cast<double>(signs_.size());
        double variance = 0.0;
        for (const double sign : signs_) {
            variance += (sign - mean) * (sign - mean);
        }
        double covariance = 0.0;
        for (std::size_t index = lag; index < signs_.size(); ++index) {
            covariance += (signs_[index] - mean) * (signs_[index - lag] - mean);
        }
        return (covariance / static_cast<double>(signs_.size() - lag)) /
               (variance / static_cast<double>(signs_.size()));
    }

private:
    std::vector<double> signs_;
    tradematch::OrderId last_taker_{0};
};

void test_same_seed_replays_identically() {
    tradematch::MarketSimulator first(short_day(11));
    tradematch::MarketSimulator second(short_day(11));
    tradematch::MarketSimulator other(short_day(12));
    for (int round = 0; round < 3; ++round) {
        first.run_for(10 * kSecond);
        second.run_for(10 * kSecond);
        other.run_for(10 * kSecond);
    }
    const auto symbol = first.symbol(0);
    expect(first.stats().requests == second.stats().requests && first.stats().trades == second.stats().trades,
           "The same seed should produce the same activity.");
    expect(first.feed().last_price(symbol) == second.feed().last_price(symbol), "The same seed ends at one price.");
    expect(first.engine().book(symbol).snapshot().bids.size() == second.engine().book(symbol).snapshot().bids.size(),
           "The same seed builds the same book.");
    expect(first.stats().trades != other.stats().trades ||
               first.feed().last_price(symbol) != other.feed().last_price(symbol),
           "A different seed should take a different path.");
}

// A digest of a simulated day: the stats, the last price, and every resting order, level by level, in queue order.
std::uint64_t fingerprint_of(const tradematch::SimulationConfig& config) {
    tradematch::MarketSimulator sim(config);
    sim.run_for(30 * kSecond);  // a full day, including both auctions and day-order expiry, and the next open
    const auto symbol = sim.symbol(0);
    const auto& stats = sim.stats();

    Fingerprint fingerprint;
    for (const std::uint64_t value :
         {stats.requests, stats.trades, stats.auctions, stats.stops_triggered, stats.self_trade_cancels}) {
        fingerprint.add(value);
    }
    fingerprint.add(static_cast<std::uint64_t>(sim.feed().last_price(symbol).value_or(0)));
    const auto snapshot = sim.engine().book(symbol).snapshot();
    for (const auto* levels : {&snapshot.bids, &snapshot.asks}) {
        fingerprint.add(levels->size());
        for (const auto& level : *levels) {
            fingerprint.add(static_cast<std::uint64_t>(level.price));
            fingerprint.add(level.total_quantity);
            for (const auto order_id : level.order_ids) {
                fingerprint.add(order_id);
            }
        }
    }
    return fingerprint.value();
}

void test_seed_replays_on_every_platform() {
    const std::uint64_t value = fingerprint_of(short_day(42));
    // Every platform in CI must reproduce this value: the simulator avoids the standard distributions, whose output
    // differs between standard libraries, and floating-point contraction.
    expect(value == 0x2BB181033465076AU,
           "Seed 42 should replay the same market everywhere, but its fingerprint is " + std::to_string(value) + ".");

    // Without execution traders the other agents draw the same random numbers as before there were any, so the market
    // is exactly the one they made on their own.
    auto without_execution = short_day(42);
    without_execution.execution_traders = 0;
    const std::uint64_t without_value = fingerprint_of(without_execution);
    expect(without_value == 0xB967CF06DD20626AU,
           "Without execution traders, seed 42 should replay the market of the other agents alone, but its "
           "fingerprint is " +
               std::to_string(without_value) + ".");
}

void test_session_names() {
    using tradematch::SessionPhase;
    expect(std::string{tradematch::to_string(SessionPhase::OpeningAuction)} == "OPENING AUCTION" &&
               std::string{tradematch::to_string(SessionPhase::Continuous)} == "CONTINUOUS" &&
               std::string{tradematch::to_string(SessionPhase::ClosingAuction)} == "CLOSING AUCTION" &&
               std::string{tradematch::to_string(SessionPhase::Closed)} == "CLOSED",
           "Every session phase should have a display name.");
}

void test_sessions_cycle_through_auctions() {
    tradematch::MarketSimulator sim(short_day(3));
    expect(sim.session() == tradematch::SessionPhase::OpeningAuction && sim.day() == 1, "Day 1 opens with an auction.");
    const auto symbol = sim.symbol(0);

    bool saw_continuous = false;
    bool saw_closing = false;
    bool saw_closed = false;
    bool saw_indicative = false;
    while (sim.day() < 3) {
        sim.step();
        const auto& book = sim.engine().book(symbol);
        switch (sim.session()) {
            case tradematch::SessionPhase::Continuous:
                saw_continuous = true;
                expect(book.phase() == tradematch::TradingPhase::Continuous, "The book trades continuously.");
                if (const auto bid = book.best_bid(), ask = book.best_ask(); bid && ask) {
                    expect(*bid < *ask, "Continuous books are never crossed.");
                }
                break;
            case tradematch::SessionPhase::OpeningAuction:
            case tradematch::SessionPhase::ClosingAuction:
                saw_closing = saw_closing || sim.session() == tradematch::SessionPhase::ClosingAuction;
                expect(book.phase() == tradematch::TradingPhase::Auction, "Auction sessions put the book in a call.");
                saw_indicative = saw_indicative || book.indicative_auction().has_value();
                break;
            case tradematch::SessionPhase::Closed:
                saw_closed = true;
                break;
        }
    }
    expect(saw_continuous && saw_closing && saw_closed, "Every session phase should occur.");
    expect(saw_indicative, "Auctions should attract crossing interest and an indicative price.");
    expect(sim.stats().auctions >= 3U, "Opening and closing auctions should execute.");
    expect(sim.previous_close(0).has_value(), "A finished day records its closing price.");
}

void test_agents_exercise_the_engine() {
    tradematch::MarketSimulator sim(short_day(5));
    sim.run_for(60 * kSecond);
    const auto& stats = sim.stats();
    expect(stats.requests > 10'000U, "Agents should send a steady stream of requests.");
    expect(stats.trades > 1'000U, "The market should trade.");
    expect(stats.self_trade_cancels > 0U, "Some agents cross their own orders and are protected.");
    expect(stats.rejects == 0U, "Agents should only send valid requests.");

    const auto symbol = sim.symbol(0);
    const auto bids = sim.feed().depth(symbol).levels(tradematch::Side::Buy);
    expect(bids.size() >= 5U, "Market makers should keep a deep book.");
    const auto fair = static_cast<double>(sim.fair_value(0));
    const auto last = static_cast<double>(sim.feed().last_price(symbol).value_or(0));
    expect(last > fair - 200.0 && last < fair + 200.0, "Prices should track fair value.");
}

bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void test_outside_orders_trade_with_the_agents() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::MarketSimulator sim(short_day(9));
    sim.run_for(6 * kSecond);
    expect(sim.session() == tradematch::SessionPhase::Continuous, "The test should start in continuous trading.");
    const tradematch::OrderBook& book = sim.engine().book(sim.symbol(0));
    const auto best_ask = book.best_ask();
    const auto best_bid = book.best_bid();
    if (!best_ask || !best_bid) {
        throw TestFailure("The agents should have quoted both sides.");
    }

    // A marketable order takes the agents' offers, under an ID of the simulator's.
    const std::uint64_t requests = sim.stats().requests;
    const tradematch::OrderId next = sim.next_order_id();
    tradematch::OrderRequest take{0, Side::Buy, OrderType::Limit, 500, *best_ask + 5, TimeInForce::ImmediateOrCancel};
    take.owner = sim.participant_owner();
    const auto taken = sim.submit(0, take);
    expect(taken.accepted && taken.order_id == next && taken.filled_quantity > 0U,
           "The order should trade, under the ID the simulator said it would take.");
    for (const auto& trade : taken.trades) {
        expect(trade.buy_order_id == taken.order_id && trade.sell_order_id != taken.order_id &&
                   trade.aggressor_side == Side::Buy,
               "Each trade should be against an agent's offer, with the outside order as the aggressor.");
    }
    expect(sim.stats().requests == requests + 1, "The request should count in the stats.");

    // A passive order rests, keeps its place in the queue when reduced, and can be cancelled.
    tradematch::OrderRequest rest{0, Side::Buy, OrderType::Limit, 300, *best_bid - 30, TimeInForce::GoodTillCancel};
    rest.owner = sim.participant_owner();
    const auto rested = sim.submit(0, rest);
    expect(rested.rested && book.has_order(rested.order_id) && rested.order_id > taken.order_id,
           "A passive outside order should rest under a new ID.");
    rest.order_id = rested.order_id;
    rest.quantity = 200;
    const auto reduced = sim.replace(0, rest);
    expect(reduced.replaced && reduced.priority_kept, "Reducing an outside order should keep its place.");

    // The agents' orders are out of reach, and the agents' owner IDs can't be borrowed.
    const auto agent_order = book.snapshot().asks.front().order_ids.front();
    expect(!sim.cancel(0, agent_order).cancelled && book.has_order(agent_order),
           "An outside cancel shouldn't reach an agent's order.");
    auto borrowed = rest;
    borrowed.order_id = agent_order;
    expect(!sim.replace(0, borrowed).replaced, "An outside replace shouldn't reach an agent's order.");
    auto impostor = take;
    impostor.owner = 1;
    expect(throws([&] { sim.submit(0, impostor); }), "An agent's owner ID should be refused.");
    expect(throws([&] { sim.submit(sim.symbol_count(), take); }), "An unknown symbol should be refused.");
    expect(sim.cancel(0, rested.order_id).cancelled && !book.has_order(rested.order_id),
           "An outside order should be cancellable.");

    // A bid joining the best price is filled by the agents' selling in time.
    const auto bid = book.best_bid();
    if (!bid) {
        throw TestFailure("The agents should still be bidding.");
    }
    tradematch::OrderRequest join{0, Side::Buy, OrderType::Limit, 100, *bid, TimeInForce::GoodTillCancel};
    join.owner = sim.participant_owner();
    const auto joined = sim.submit(0, join);
    for (int second = 0; second < 15 && book.has_order(joined.order_id); ++second) {
        sim.run_for(kSecond);
    }
    expect(!book.has_order(joined.order_id), "A bid at the best price should trade with the agents' selling.");

    // In a call auction, a market order waits for the uncross, then trades at its price.
    while (sim.session() != tradematch::SessionPhase::ClosingAuction) {
        sim.step();
    }
    const tradematch::OrderRequest market{0, Side::Sell, OrderType::Market, 100, 0, TimeInForce::Day};
    auto queued = market;
    queued.owner = sim.participant_owner();
    const auto waiting = sim.submit(0, queued);
    expect(waiting.queued_for_auction && book.has_auction_market_order(waiting.order_id),
           "A market order should wait for the uncross.");
    while (sim.session() == tradematch::SessionPhase::ClosingAuction) {
        sim.step();
    }
    expect(!book.has_auction_market_order(waiting.order_id), "The uncross should take the market order.");
}

void test_execution_traders_slice_parent_orders() {
    auto config = short_day(21);
    config.symbols = {"ACME", "GLOBEX"};
    const TempFile file;
    {
        tradematch::JournalWriter journal(file.path(),
                                          tradematch::JournalOptions{tradematch::Durability::Buffered, true, 1});
        tradematch::MarketSimulator sim(config, &journal);
        sim.run_for(25 * kSecond);  // one full day
        journal.close();
    }

    // The journal names every order's owner, and owners are numbered in turn: the market makers, the noise traders,
    // the momentum traders, then the execution traders.
    const auto first =
        static_cast<tradematch::OwnerId>(config.market_makers + config.noise_traders + config.momentum_traders + 1);
    const auto last = static_cast<tradematch::OwnerId>(first + config.execution_traders - 1);
    const tradematch::Timestamp open = config.opening_auction;
    const tradematch::Timestamp close = config.opening_auction + config.continuous;
    std::map<std::pair<tradematch::OwnerId, tradematch::SymbolId>, tradematch::Side> sides;
    std::size_t children = 0;
    std::size_t marketable = 0;
    std::size_t runs = 0;  // runs of children on one side; each new parent on the other side starts one
    tradematch::JournalReader reader(file.path());
    tradematch::Command command;
    while (reader.next(command)) {
        const tradematch::OrderRequest& order = command.order;
        if (command.type != tradematch::CommandType::Submit || order.owner < first || order.owner > last) {
            continue;
        }
        ++children;
        const bool immediate = order.time_in_force == tradematch::TimeInForce::ImmediateOrCancel;
        marketable += immediate ? 1U : 0U;
        expect(order.type == tradematch::OrderType::Limit && (order.quantity == 100U || order.quantity == 200U) &&
                   (immediate || order.time_in_force == tradematch::TimeInForce::Day),
               "Child orders should be IOC or DAY limit orders the size of other agents' orders.");
        expect(command.timestamp > open && command.timestamp <= close,
               "Execution traders should only trade in continuous trading.");
        const auto [entry, added] = sides.try_emplace({order.owner, command.symbol}, order.side);
        if (added || entry->second != order.side) {
            entry->second = order.side;
            ++runs;
        }
    }
    expect(children > 2'000U, "Execution traders should send a stream of child orders.");
    expect(sides.size() == 2 * config.execution_traders, "Each execution trader should work parents on every symbol.");
    expect(marketable > children * 3 / 4 && marketable < children,
           "Most children should take liquidity, and some should join the best price.");
    expect(children > 20 * runs, "Each parent should be worked in many child orders on its side.");
}

// One day with a minute of continuous trading, seen through its aggressive orders.
AggressorSigns aggressor_signs(std::uint64_t seed, std::size_t execution_traders) {
    auto config = short_day(seed);
    config.continuous = 60 * kSecond;
    config.execution_traders = execution_traders;
    tradematch::MarketSimulator sim(config);
    AggressorSigns signs;
    sim.set_order_listener(&signs);
    sim.run_for(65 * kSecond);
    sim.set_order_listener(nullptr);
    return signs;
}

void test_execution_traders_make_order_flow_persistent() {
    for (const std::uint64_t seed : {31U, 32U}) {
        const AggressorSigns sliced = aggressor_signs(seed, 2);
        const AggressorSigns unsliced = aggressor_signs(seed, 0);
        expect(sliced.signs().size() > 2 * unsliced.signs().size(),
               "Execution traders' children should be most of the aggressive orders.");
        // Without them, a buy follows a buy only a little more often than chance.
        expect(sliced.autocorrelation(1) > 0.4 && unsliced.autocorrelation(1) < 0.25,
               "Sliced parents should make the signs of aggressive orders persistent, but the lag-1 autocorrelation "
               "is " +
                   std::to_string(sliced.autocorrelation(1)) + " with execution traders and " +
                   std::to_string(unsliced.autocorrelation(1)) + " without.");
        expect(sliced.autocorrelation(100) < sliced.autocorrelation(1) - 0.2,
               "The persistence should fade as parents finish, within a hundred or so orders.");
        expect(aggressor_signs(seed, 2).signs() == sliced.signs(), "Execution traders should replay identically.");
    }
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"same_seed_replays_identically", test_same_seed_replays_identically},
        {"seed_replays_on_every_platform", test_seed_replays_on_every_platform},
        {"session_names", test_session_names},
        {"sessions_cycle_through_auctions", test_sessions_cycle_through_auctions},
        {"agents_exercise_the_engine", test_agents_exercise_the_engine},
        {"outside_orders_trade_with_the_agents", test_outside_orders_trade_with_the_agents},
        {"execution_traders_slice_parent_orders", test_execution_traders_slice_parent_orders},
        {"execution_traders_make_order_flow_persistent", test_execution_traders_make_order_flow_persistent},
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
        std::cout << "All simulator tests passed (" << tests.size() << ").\n";
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
