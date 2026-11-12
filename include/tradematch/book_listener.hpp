#ifndef TRADEMATCH_BOOK_LISTENER_HPP
#define TRADEMATCH_BOOK_LISTENER_HPP

#include "tradematch/types.hpp"

#include <cstdint>
#include <span>

namespace tradematch {

// The displayed state of one price level after a request changed it. A quantity of 0 means the level is gone.
struct LevelUpdate {
    Side side{Side::Buy};
    Price price{};
    Volume quantity{};  // displayed quantity, matching the order book's depth (iceberg reserves stay hidden)
    std::uint32_t order_count{};

    bool operator==(const LevelUpdate&) const = default;
};

// Receives market data from one order book. After each submit, cancel, replace, or end-of-day expiry, the book emits
// every trade the request caused, in execution order, followed by one update per price level the request changed,
// carrying that level's final state. Consumers can therefore maintain an exact replica of the book's depth from the
// updates alone. With no listener attached, the book skips all of this bookkeeping.
class BookListener {
public:
    BookListener() = default;
    BookListener(const BookListener&) = default;
    BookListener& operator=(const BookListener&) = default;
    BookListener(BookListener&&) = default;
    BookListener& operator=(BookListener&&) = default;
    virtual ~BookListener() = default;

    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_level_update(const LevelUpdate& update) = 0;
};

// One change to a resting order, the way an order-level (L3) feed such as NASDAQ ITCH reports it. Only displayed
// quantity counts: an iceberg's reserve stays invisible until a tranche of it is shown.
struct OrderEvent {
    enum class Type : std::uint8_t {
        Added,     // joined the back of its price level showing `quantity`: a new order, or an iceberg's next tranche
        Executed,  // traded `quantity` at `price` in trade `trade_id`; in an auction that is the auction price
        Reduced,   // shown quantity cut by `quantity` in place, keeping time priority
        Deleted    // left the book without trading while showing `quantity`: cancelled, expired, removed by self-trade
                   // prevention, or replaced at a new price or size, in which case an Added follows when it rests again
    };

    Type type{Type::Added};
    Side side{Side::Buy};
    OrderId order_id{};
    Price price{};  // the order's limit price, except for executions
    Quantity quantity{};
    TradeId trade_id{};  // executions only

    bool operator==(const OrderEvent&) const = default;
};

// Receives order-level market data from one order book. After each request that changed a resting order or traded,
// the book reports the changes in execution order, together with the request's trades. Applying the events in order
// reproduces every price level's queue exactly. With no listener attached, the book records nothing.
class OrderEventListener {
public:
    OrderEventListener() = default;
    OrderEventListener(const OrderEventListener&) = default;
    OrderEventListener& operator=(const OrderEventListener&) = default;
    OrderEventListener(OrderEventListener&&) = default;
    OrderEventListener& operator=(OrderEventListener&&) = default;
    virtual ~OrderEventListener() = default;

    virtual void on_order_events(std::span<const OrderEvent> events, std::span<const Trade> trades) = 0;
};

}  // namespace tradematch

#endif
