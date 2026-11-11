#ifndef TRADEMATCH_ORDER_BOOK_HPP
#define TRADEMATCH_ORDER_BOOK_HPP

#include "tradematch/detail/order_index.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace tradematch {

// Initial capacities. The book grows past them automatically, but sizing them for the expected peak means that
// submit, cancel, and replace never touch the heap allocator.
struct BookCapacity {
    std::size_t orders{1'024};
    std::size_t price_levels{256};  // per side
    std::size_t trades_per_call{64};
};

// Price-time-priority limit order book for a single instrument.
//
// Order handling: limit and market orders; GTC, DAY, IOC, and FOK time in force; post-only orders that are rejected
// rather than take liquidity; and replace, which keeps time priority when only the quantity goes down.
//
// All storage is index based and reused:
// - Resting orders live in a pooled array and form an intrusive FIFO list per price level.
// - Each side keeps its price levels in a vector sorted so the best price is at the back. Taking or removing the best
//   level is O(1), and new levels, which usually appear near the touch, shift only a few entries.
// - An open-addressing hash index maps order IDs to pool slots for O(1) cancel and replace.
// - Trades are written to a reusable buffer and returned as a span.
// Once capacity covers the peak book size, the request path performs no heap allocations.
class OrderBook {
public:
    explicit OrderBook(const BookCapacity& capacity = {});

    SubmitResult submit(const OrderRequest& order);
    CancelResult cancel(OrderId order_id);
    ReplaceResult replace(const OrderRequest& replacement);

    // End-of-day processing: cancels every resting DAY order and returns their IDs in ascending order.
    std::vector<OrderId> expire_day_orders();

    bool has_order(OrderId order_id) const noexcept;
    std::optional<RestingOrderView> get_order(OrderId order_id) const;
    std::vector<RestingOrderView> get_orders_at_level(Side side, Price price) const;
    std::optional<Price> best_bid() const noexcept;
    std::optional<Price> best_ask() const noexcept;
    BookSnapshot snapshot() const;

    std::size_t order_count() const noexcept {
        return index_.size();
    }

    std::size_t level_count(Side side) const noexcept {
        return ladder(side).size();
    }

private:
    using Slot = std::uint32_t;
    static constexpr Slot kNone = std::numeric_limits<Slot>::max();

    struct OrderNode {
        OrderId order_id{};
        SequenceNumber sequence{};
        Quantity remaining{};
        Slot level{kNone};
        Slot prev{kNone};
        Slot next{kNone};  // also links free nodes
        Side side{Side::Buy};
        TimeInForce time_in_force{TimeInForce::GoodTillCancel};
    };

    struct PriceLevel {
        Price price{};
        Volume total_quantity{};
        Slot head{kNone};
        Slot tail{kNone};  // also links free levels
        std::uint32_t order_count{};
        Side side{Side::Buy};
    };

    struct LadderEntry {
        Price price{};
        Slot level{kNone};
    };

    using Ladder = std::vector<LadderEntry>;  // bids ascending, asks descending: the best price is always at the back

    struct Rejection {
        RejectReason reason{RejectReason::None};
        std::string_view message;
    };

    Ladder& ladder(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    const Ladder& ladder(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    Rejection validate(const OrderRequest& order, bool replacing) const noexcept;
    void execute(const OrderRequest& order, SubmitResult& result);
    bool would_cross(const OrderRequest& order) const noexcept;
    Volume available_liquidity(const OrderRequest& order) const noexcept;
    void match(const OrderRequest& order, Quantity& remaining);
    void rest(const OrderRequest& order, Quantity remaining, SequenceNumber sequence);
    void remove_resting(Slot node_slot);
    Slot find_or_create_level(Side side, Price price);
    void remove_level(Slot level_slot);
    Ladder::const_iterator find_ladder_entry(Side side, Price price) const;
    void record_trade(const OrderRequest& aggressor, OrderId resting_order_id, Price price, Quantity quantity);

    Slot allocate_node();
    void release_node(Slot node_slot) noexcept;
    Slot allocate_level();
    void release_level(Slot level_slot) noexcept;

    std::vector<OrderNode> nodes_;
    std::vector<PriceLevel> levels_;
    Slot free_nodes_{kNone};
    Slot free_levels_{kNone};
    Ladder bids_;
    Ladder asks_;
    detail::OrderIndex index_;
    std::vector<Trade> trades_;
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch

#endif
