#ifndef TRADEMATCH_ITCH_FEED_HPP
#define TRADEMATCH_ITCH_FEED_HPP

#include "tradematch/itch.hpp"
#include "tradematch/matching_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tradematch::itch {

// Publishes a matching engine's order-level market data as an ITCH 5.0 feed, in NASDAQ's file format. Each symbol is
// announced with a stock directory message, as stock locate symbol + 1, before its first order. From then on every
// change to a displayed order becomes an add, execute, cancel, delete, or replace message. A replace that re-enters
// the book without trading is a single replace message, and auction executions are non-printable executions whose
// volume a cross trade message reports. Order references are unique across the feed and renewed whenever an order,
// or an iceberg's next tranche, joins the back of a queue, as ITCH requires. Match numbers are unique too.
class FeedPublisher final : public OrderFeedListener {
public:
    // Engine prices times `price_multiplier` are ITCH prices, so 100 suits engine prices in cents.
    explicit FeedPublisher(const MatchingEngine& engine, std::uint32_t price_multiplier = 100);

    // Nanoseconds since midnight, stamped on every message published from now on.
    void set_time(Timestamp time) noexcept {
        time_ = time;
    }

    // The cross type of the cross trade messages for the next auctions: 'O' opening (the default), 'C' closing, 'H'
    // halt or IPO, 'I' intraday.
    void set_cross_type(char cross_type) noexcept {
        cross_type_ = cross_type;
    }

    void system_event(char event_code);
    // Publishes the symbol's stock directory message, unless it has been published already.
    void announce(SymbolId symbol);

    void on_order_events(SymbolId symbol, std::span<const OrderEvent> events, std::span<const Trade> trades) override;

    // The framed messages published since the last call, which are removed.
    std::vector<std::uint8_t> take_bytes();
    std::size_t pending_bytes() const noexcept {
        return bytes_.size();
    }

    std::uint64_t message_count() const noexcept {
        return total_messages_;
    }
    std::uint64_t message_count(char type) const noexcept {
        return counts_[static_cast<unsigned char>(type)];
    }
    std::uint32_t price_multiplier() const noexcept {
        return price_multiplier_;
    }

    // The reference an engine order is displayed under now, if it is displayed.
    std::optional<std::uint64_t> reference_of(SymbolId symbol, OrderId order_id) const;

private:
    struct Displayed {
        std::uint64_t reference{};
        Quantity shown{};
    };

    Header header(SymbolId symbol) const noexcept;
    std::uint32_t feed_price(Price price) const;
    void publish(const Message& message);
    std::unordered_map<OrderId, Displayed>& orders_of(SymbolId symbol);

    const MatchingEngine& engine_;
    std::uint32_t price_multiplier_;
    Timestamp time_{0};
    char cross_type_{'O'};
    std::uint64_t next_reference_{1};
    std::uint64_t next_match_{1};
    std::vector<bool> announced_;
    std::vector<std::unordered_map<OrderId, Displayed>> displayed_;  // by symbol
    std::vector<std::uint8_t> bytes_;
    std::array<std::uint64_t, 256> counts_{};
    std::uint64_t total_messages_{0};
};

struct StockStats {
    std::uint64_t volume{0};  // printable executions, non-displayed trades, and crosses, in shares
    double value{0.0};        // the same in dollars
    std::uint64_t executions{0};
    std::uint32_t last_price{0};
};

struct ReplicaStats {
    std::array<std::uint64_t, 256> messages{};  // by type
    std::uint64_t total_messages{0};
    std::uint64_t invalid_messages{0};  // undecodable: an unknown type or the wrong length
    std::uint64_t anomalies{0};         // well formed, but at odds with the book, such as a delete of an unknown order
    std::uint64_t live_orders{0};
    std::uint64_t peak_orders{0};
    Timestamp last_timestamp{0};
};

// A trade the feed prints: an execution of a displayed order, a trade with a non-displayed order, or a cross.
struct FeedTrade {
    SymbolId symbol{};
    Timestamp timestamp{};
    std::uint32_t price{};
    std::uint64_t shares{};
    std::uint64_t match_number{};
    char type{};                    // the message that printed it: 'E' or 'C' an execution, 'P' a trade, 'Q' a cross
    std::optional<Side> aggressor;  // the side that took liquidity, which only an execution of a displayed order shows
};

// Receives a replica's trades as it applies the messages that print them.
class FeedTradeListener {
public:
    FeedTradeListener() = default;
    FeedTradeListener(const FeedTradeListener&) = default;
    FeedTradeListener& operator=(const FeedTradeListener&) = default;
    FeedTradeListener(FeedTradeListener&&) = default;
    FeedTradeListener& operator=(FeedTradeListener&&) = default;
    virtual ~FeedTradeListener() = default;

    virtual void on_trade(const FeedTrade& trade) = 0;
};

// Rebuilds every stock's order book from an ITCH 5.0 feed. The books are TradeMatch's own, held in auction mode:
// orders rest exactly where the feed puts them and never match, because the feed reports each execution itself.
// Prices stay in ITCH units, and orders keep their feed references as IDs.
class BookReplica {
public:
    explicit BookReplica(const BookCapacity& capacity = default_capacity());

    // Applies one message. A message at odds with the book, such as an execution of an unknown order, is counted as
    // an anomaly and otherwise ignored.
    void apply(const Message& message);
    // Decodes and applies one message; returns false, after counting it, if it isn't a valid ITCH message.
    bool apply(std::span<const std::uint8_t> bytes);

    const MatchingEngine& engine() const noexcept {
        return engine_;
    }
    std::optional<SymbolId> find(std::string_view symbol) const {
        return engine_.find_symbol(symbol);
    }
    const StockStats& stock_stats(SymbolId symbol) const {
        return stock_stats_.at(symbol);
    }
    const ReplicaStats& stats() const noexcept {
        return stats_;
    }

    // Reports each trade the feed prints from now on to the listener, or to no one if it is null. The listener must
    // outlive the replica or be removed first.
    void set_trade_listener(FeedTradeListener* listener) noexcept {
        trade_listener_ = listener;
    }

    // Small books that grow as needed: NASDAQ lists thousands of stocks, most of them with thin books.
    static BookCapacity default_capacity() noexcept;

private:
    struct Applier;

    void apply_decoded(const Message& message);
    std::optional<SymbolId> symbol_at(std::uint16_t locate) const noexcept;
    SymbolId add_stock(std::uint16_t locate, std::string_view name);
    void add(const AddOrder& order);
    // Takes shares off a resting order; returns the order as it was, or nothing if it is unknown.
    std::optional<RestingOrderView> take(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares);
    void remove(std::uint16_t locate, std::uint64_t reference);
    void replace(const OrderReplace& replace);
    // Adds a trade to its stock's volume and reports it.
    void record_trade(std::uint16_t locate,
                      std::uint64_t shares,
                      std::uint32_t price,
                      std::uint64_t match_number,
                      char type,
                      std::optional<Side> aggressor);
    void count_order(bool added) noexcept;

    MatchingEngine engine_;
    BookCapacity capacity_;
    std::vector<std::optional<SymbolId>> symbols_;  // by stock locate
    std::vector<StockStats> stock_stats_;           // by symbol
    ReplicaStats stats_;
    FeedTradeListener* trade_listener_{nullptr};
};

// Compares a replica with the engine whose feed built it: each symbol's levels must hold the same orders, under
// their feed references, in the same queue order and showing the same quantities. Returns a description of the first
// difference, or an empty string if there is none.
std::string first_difference(const MatchingEngine& engine, const FeedPublisher& publisher, const BookReplica& replica);

}  // namespace tradematch::itch

#endif
