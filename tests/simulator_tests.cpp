#include "tradematch/simulator.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
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

void test_seed_replays_on_every_platform() {
    tradematch::MarketSimulator sim(short_day(42));
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
    // Every platform in CI must reproduce this value: the simulator avoids the standard distributions, whose output
    // differs between standard libraries, and floating-point contraction.
    expect(fingerprint.value() == 0xB967CF06DD20626AU,
           "Seed 42 should replay the same market everywhere, but its fingerprint is " +
               std::to_string(fingerprint.value()) + ".");
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
