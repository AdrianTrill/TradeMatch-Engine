#ifndef TRADEMATCH_SIMULATOR_HPP
#define TRADEMATCH_SIMULATOR_HPP

#include "tradematch/journal.hpp"
#include "tradematch/latency_histogram.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
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
    std::size_t execution_traders{2};
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
// side, noise traders post and cancel passive orders and send marketable ones, momentum traders chase trends,
// execution traders slice large parent orders into many small child orders, and a "whale" works large icebergs and
// stop orders. Everything is driven by one seeded random generator and simulated time, so a given configuration
// replays identically. With a journal, every engine command is recorded, and replaying the journal rebuilds the exact
// same books.
class MarketSimulator {
public:
    explicit MarketSimulator(SimulationConfig config = {}, JournalWriter* journal = nullptr);
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

    // Attaches an order-level (L3) listener to the simulated exchange, such as an ITCH feed publisher, or detaches it
    // with nullptr.
    void set_order_listener(OrderFeedListener* listener) {
        engine_.set_order_listener(listener);
    }

    // Orders from outside the simulation, such as a person trading in the browser playground, at the current
    // simulated time. They meet the agents' orders like any other, are journaled like theirs, and count in the stats.
    // Each new order takes the simulator's next order ID in place of order.order_id, and the result reports it. An
    // outside order's owner must not be an agent's (participant_owner() is one no agent uses), and cancels and
    // replaces reach only outside orders: any other ID is rejected as unknown.
    SubmitResult submit(std::size_t symbol_index, OrderRequest order);
    CancelResult cancel(std::size_t symbol_index, OrderId order_id);
    ReplaceResult replace(std::size_t symbol_index, const OrderRequest& replacement);

    OwnerId participant_owner() const noexcept;

    // The ID the next order will take, whether an agent's or an outside one, so that a caller can recognize its own
    // order in the events of the request that submits it.
    OrderId next_order_id() const noexcept {
        return next_order_id_;
    }

    // Flushes the journal and writes a snapshot of the engine; see JournaledEngine::save_snapshot().
    std::uint64_t save_snapshot(const std::filesystem::path& path) {
        return journaled_.save_snapshot(path);
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
    void trade_execution(Agent& agent, std::size_t index);
    Quantity send(Agent& agent, std::size_t index, OrderRequest order);
    SubmitResult place(std::size_t index, const OrderRequest& order);
    void check_outside_owner(OwnerId owner) const;
    void cancel(Agent& agent, std::size_t index, OrderId order_id);
    void prune(Agent& agent);
    void record_latency(std::chrono::steady_clock::time_point start);
    static Price tick(double price) noexcept;
    bool chance(double probability);
    std::uint64_t uniform(std::uint64_t low, std::uint64_t high);

    SimulationConfig config_;
    std::mt19937_64 rng_;
    MatchingEngine engine_;
    JournaledEngine journaled_;  // every state change goes through here, so it can be journaled
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<LatencyHistogram> latency_;
    std::vector<Instrument> symbols_;
    std::vector<Agent> agents_;
    SimulationStats stats_;
    SessionPhase phase_{SessionPhase::Closed};
    Timestamp now_{0};
    Timestamp phase_end_{0};
    std::unordered_set<OrderId> outside_orders_;
    OrderId next_order_id_{1};
    int day_{0};
};

}  // namespace tradematch

#endif
