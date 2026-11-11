#ifndef TRADEMATCH_SIMULATOR_HPP
#define TRADEMATCH_SIMULATOR_HPP

#include "tradematch/latency_histogram.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace tradematch {

enum class SessionPhase : std::uint8_t {
    OpeningAuction,
    Continuous,
    ClosingAuction,
    Closed
};

const char* to_string(SessionPhase phase) noexcept;

struct SimulationConfig {
    std::uint64_t seed{7};
    std::vector<std::string> symbols{"ACME"};
    Price initial_price{10'000};
    Timestamp step{10'000'000};  // simulated time per step: 10 ms
    Timestamp opening_auction{4'000'000'000};
    Timestamp continuous{90'000'000'000};
    Timestamp closing_auction{4'000'000'000};
    Timestamp closed{2'000'000'000};
    std::size_t market_makers{4};
    std::size_t noise_traders{16};
    std::size_t momentum_traders{3};
    double volatility{0.35};        // fair-value standard deviation per step, in ticks
    double news_probability{4e-4};  // chance per step of a jump in fair value
    bool measure_latency{true};
};

struct SimulationStats {
    std::uint64_t requests{0};
    std::uint64_t rejects{0};
    std::uint64_t trades{0};
    std::uint64_t auctions{0};
    std::uint64_t stops_triggered{0};
    std::uint64_t self_trade_cancels{0};
    std::map<RejectReason, std::uint64_t> rejects_by_reason;
};

// A deterministic, agent-based market around a random-walk fair value, running full exchange sessions: an opening call
// auction, continuous trading, a closing call auction, and day-order expiry. Market makers quote several levels per
// side, noise traders post and cancel passive orders and send marketable ones, momentum traders chase trends, and a
// "whale" works large icebergs and stop orders. Everything is driven by one seeded random generator and simulated
// time, so a given configuration replays identically.
class MarketSimulator {
public:
    explicit MarketSimulator(SimulationConfig config = {});
    MarketSimulator(const MarketSimulator&) = delete;
    MarketSimulator& operator=(const MarketSimulator&) = delete;
    MarketSimulator(MarketSimulator&&) = delete;
    MarketSimulator& operator=(MarketSimulator&&) = delete;
    ~MarketSimulator();

    void step();
    void run_for(Timestamp duration);

    Timestamp now() const noexcept {
        return now_;
    }

    SessionPhase session() const noexcept {
        return phase_;
    }

    // Simulated time left in the current session phase.
    Timestamp phase_time_remaining() const noexcept {
        return phase_end_ - now_;
    }

    int day() const noexcept {
        return day_;
    }

    const MatchingEngine& engine() const noexcept {
        return engine_;
    }

    const MarketDataFeed& feed() const noexcept {
        return *feed_;
    }

    SymbolId symbol(std::size_t index) const {
        return symbols_.at(index).id;
    }

    std::size_t symbol_count() const noexcept {
        return symbols_.size();
    }

    Price fair_value(std::size_t index) const;
    std::optional<Price> previous_close(std::size_t index) const;
    std::optional<Price> day_open(std::size_t index) const;

    const SimulationStats& stats() const noexcept {
        return stats_;
    }

    // Wall-clock duration of every engine request so far, market data publishing included.
    const LatencyHistogram& request_latency() const noexcept {
        return *latency_;
    }

private:
    struct Agent;
    struct Instrument {
        SymbolId id{};
        double fair{};
        std::optional<Price> previous_close;
        std::optional<Price> open;
        double fast_average{};
        double slow_average{};
    };

    void enter_phase(SessionPhase phase);
    void act(Agent& agent, std::size_t index);
    void quote(Agent& agent, std::size_t index);
    void trade_noise(Agent& agent, std::size_t index);
    void trade_momentum(Agent& agent, std::size_t index);
    void trade_whale(Agent& agent, std::size_t index);
    void send(Agent& agent, std::size_t index, OrderRequest order);
    void cancel(Agent& agent, std::size_t index, OrderId order_id);
    void prune(Agent& agent);
    void record_latency(std::chrono::steady_clock::time_point start);
    static Price tick(double price) noexcept;
    bool chance(double probability);
    std::uint64_t uniform(std::uint64_t low, std::uint64_t high);

    SimulationConfig config_;
    std::mt19937_64 rng_;
    MatchingEngine engine_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<LatencyHistogram> latency_;
    std::vector<Instrument> symbols_;
    std::vector<Agent> agents_;
    SimulationStats stats_;
    SessionPhase phase_{SessionPhase::Closed};
    Timestamp now_{0};
    Timestamp phase_end_{0};
    OrderId next_order_id_{1};
    int day_{0};
};

}  // namespace tradematch

#endif
