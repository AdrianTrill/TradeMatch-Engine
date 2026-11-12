#ifndef TRADEMATCH_MARKET_DATA_HPP
#define TRADEMATCH_MARKET_DATA_HPP

#include "tradematch/book_listener.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace tradematch {

struct TopOfBook {
    std::optional<Price> bid_price;
    Volume bid_quantity{};
    std::optional<Price> ask_price;
    Volume ask_quantity{};

    bool operator==(const TopOfBook&) const = default;
};

struct DepthLevel {
    Price price{};
    Volume quantity{};
    std::uint32_t order_count{};

    bool operator==(const DepthLevel&) const = default;
};

// Price-level (L2) replica of an order book, maintained purely from LevelUpdate messages.
class DepthBook {
public:
    void apply(const LevelUpdate& update);

    // Best levels first, at most `max_levels` of them.
    std::vector<DepthLevel> levels(Side side, std::size_t max_levels = std::numeric_limits<std::size_t>::max()) const;
    TopOfBook top() const;

    std::size_t level_count(Side side) const noexcept {
        return side == Side::Buy ? bids_.size() : asks_.size();
    }

private:
    std::map<Price, DepthLevel, std::greater<>> bids_;
    std::map<Price, DepthLevel, std::less<>> asks_;
};

struct Candle {
    Timestamp start{};  // aligned to the series interval
    Price open{};
    Price high{};
    Price low{};
    Price close{};
    Volume volume{};
    std::uint32_t trade_count{};

    bool operator==(const Candle&) const = default;
};

// OHLCV bars over fixed time intervals. Intervals without trades produce no bar.
class CandleSeries {
public:
    explicit CandleSeries(Timestamp interval);

    void add_trade(Timestamp timestamp, Price price, Quantity quantity);

    const std::vector<Candle>& candles() const noexcept {
        return candles_;
    }

    Timestamp interval() const noexcept {
        return interval_;
    }

private:
    Timestamp interval_;
    std::vector<Candle> candles_;
};

struct TapeEntry {
    Timestamp timestamp{};
    Trade trade{};
};

// One sequenced market data message, as a feed handler downstream of the engine would receive it.
struct FeedMessage {
    enum class Kind : std::uint8_t {
        Trade,
        Level
    };

    std::uint64_t sequence{};
    Timestamp timestamp{};
    SymbolId symbol{};
    Kind kind{Kind::Trade};
    Trade trade{};
    LevelUpdate level{};
};

struct FeedConfig {
    Timestamp candle_interval{1'000'000'000};  // one second
    std::size_t tape_length{64};               // recent trades kept per symbol
    bool record_messages{false};               // keep every message for drain_messages()
};

// Builds market data views from a matching engine's event stream: depth replicas, top of book, a recent-trades tape,
// OHLCV candles, traded volume and VWAP, all per symbol, plus a gap-free sequenced message log when enabled.
class MarketDataFeed final : public MarketDataListener {
public:
    explicit MarketDataFeed(const FeedConfig& config = {});

    // Timestamps every subsequent message; call it before each request with the current (or simulated) time.
    void set_time(Timestamp now) noexcept {
        now_ = now;
    }

    void on_trade(SymbolId symbol, const Trade& trade) override;
    void on_level_update(SymbolId symbol, const LevelUpdate& update) override;

    const DepthBook& depth(SymbolId symbol) const;
    TopOfBook top_of_book(SymbolId symbol) const;
    const std::deque<TapeEntry>& recent_trades(SymbolId symbol) const;
    const CandleSeries& candles(SymbolId symbol) const;
    std::optional<Price> last_price(SymbolId symbol) const;
    Volume traded_volume(SymbolId symbol) const;
    std::optional<double> vwap(SymbolId symbol) const;

    std::uint64_t last_sequence() const noexcept {
        return sequence_;
    }

    // Returns and clears the messages recorded since the last call (requires FeedConfig::record_messages).
    std::vector<FeedMessage> drain_messages();

private:
    struct SymbolState {
        explicit SymbolState(Timestamp candle_interval) : candles(candle_interval) {}

        DepthBook depth;
        CandleSeries candles;
        std::deque<TapeEntry> tape;
        std::optional<Price> last_price;
        Volume volume{};
        long double notional{};
    };

    SymbolState& state(SymbolId symbol);
    const SymbolState& state(SymbolId symbol) const;
    void record(FeedMessage message);

    FeedConfig config_;
    std::vector<SymbolState> symbols_;
    std::vector<FeedMessage> messages_;
    std::uint64_t sequence_{0};
    Timestamp now_{0};
};

}  // namespace tradematch

#endif
