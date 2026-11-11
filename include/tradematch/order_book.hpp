#ifndef TRADEMATCH_ORDER_BOOK_HPP
#define TRADEMATCH_ORDER_BOOK_HPP

#include "tradematch/book_listener.hpp"
#include "tradematch/detail/order_index.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tradematch {

// Initial capacities. The book grows past them automatically, but sizing them for the expected peak means that
// submit, cancel, and replace never touch the heap allocator.
struct BookCapacity {
    std::size_t orders{1'024};
    std::size_t price_levels{256};  // per side
    std::size_t trades_per_call{64};
    std::size_t stop_orders{64};
};

// Price-time-priority limit order book for a single instrument.
//
// Order handling: limit and market orders; stop and stop-limit orders triggered by the last trade price; iceberg
// orders that display one tranche at a time; GTC, DAY, IOC, and FOK time in force; post-only orders that are rejected
// rather than take liquidity; self-trade prevention; and replace, which keeps time priority when only the quantity
// goes down.
//
// All storage is index based and reused:
// - Resting orders live in a pooled array and form an intrusive FIFO list per price level.
// - Each side keeps its price levels in a vector sorted so the best price is at the back. Taking or removing the best
//   level is O(1), and new levels, which usually appear near the touch, shift only a few entries.
// - An open-addressing hash index maps order IDs to pool slots for O(1) cancel and replace.
// - Pending stop orders live in their own pool, with one vector per side sorted so the next stop to trigger is at
//   the back.
// - Trades and other per-request outputs are written to reusable buffers and returned as spans.
// Once capacity covers the peak book size, the request path performs no heap allocations.
class OrderBook {
public:
    explicit OrderBook(const BookCapacity& capacity = {});

    SubmitResult submit(const OrderRequest& order);
    CancelResult cancel(OrderId order_id);  // resting orders and pending stops
    ReplaceResult replace(const OrderRequest& replacement);

    // End-of-day processing: cancels every resting or pending DAY order and returns their IDs in ascending order.
    std::vector<OrderId> expire_day_orders();

    // Attaches a market data listener (or detaches it with nullptr). The book does no market data bookkeeping while
    // no listener is attached.
    void set_listener(BookListener* listener) noexcept {
        listener_ = listener;
    }

    bool has_order(OrderId order_id) const noexcept;
    bool has_stop_order(OrderId order_id) const noexcept;
    std::optional<RestingOrderView> get_order(OrderId order_id) const;
    std::vector<RestingOrderView> get_orders_at_level(Side side, Price price) const;
    std::optional<Price> best_bid() const noexcept;
    std::optional<Price> best_ask() const noexcept;
    std::optional<Price> last_trade_price() const noexcept;
    BookSnapshot snapshot() const;

    std::size_t order_count() const noexcept {
        return index_.size();
    }

    std::size_t stop_order_count() const noexcept {
        return stop_index_.size();
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
        Quantity remaining{};  // visible quantity; for icebergs, what is left of the displayed tranche
        Quantity hidden{};     // iceberg reserve that is not displayed yet
        Quantity display{};    // iceberg tranche size, 0 for regular orders
        OwnerId owner{};
        Slot level{kNone};
        Slot prev{kNone};
        Slot next{kNone};  // also links free nodes
        Side side{Side::Buy};
        TimeInForce time_in_force{TimeInForce::GoodTillCancel};
    };

    struct PriceLevel {
        Price price{};
        Volume total_quantity{};   // displayed quantity
        Volume hidden_quantity{};  // iceberg reserves, executable but not displayed
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

    struct StopOrder {
        OrderRequest request;
        SequenceNumber sequence{};  // arrival order, the tie-breaker between stops with the same trigger price
    };

    struct StopEntry {
        Price stop_price{};
        SequenceNumber sequence{};
        Slot stop{kNone};
    };

    // Buy stops descend and sell stops ascend by trigger price, with later arrivals first among equal prices, so the
    // next stop to trigger on each side is always at the back.
    using StopLadder = std::vector<StopEntry>;

    struct TouchedLevel {
        Side side{Side::Buy};
        Price price{};

        auto operator<=>(const TouchedLevel&) const = default;
    };

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

    bool has_pending_stops() const noexcept {
        return !buy_stops_.empty() || !sell_stops_.empty();
    }

    StopLadder& stop_ladder(Side side) noexcept {
        return side == Side::Buy ? buy_stops_ : sell_stops_;
    }

    void begin_request() noexcept;
    void finish_request(SubmitResult& result);
    void touch_level(Side side, Price price);
    void publish(std::span<const Trade> trades);
    Rejection validate(const OrderRequest& order, bool replacing) const noexcept;
    static Rejection validate_advanced(const OrderRequest& order) noexcept;
    void execute(const OrderRequest& order, SubmitResult& result);
    bool would_cross(const OrderRequest& order) const noexcept;
    Volume available_liquidity(const OrderRequest& order) const noexcept;
    void match(const OrderRequest& order, Quantity& remaining, bool& stopped_by_self_trade);
    void rest(const OrderRequest& order, Quantity remaining, SequenceNumber sequence);
    void refresh_iceberg(Slot node_slot, PriceLevel& level);
    bool prevent_self_trade(SelfTradePrevention mode, Slot node_slot, PriceLevel& level);
    void detach_node(Slot node_slot, PriceLevel& level);
    void remove_resting(Slot node_slot);
    Slot find_or_create_level(Side side, Price price);
    void remove_level(Slot level_slot);
    Ladder::const_iterator find_ladder_entry(Side side, Price price) const;
    void record_trade(Side aggressor_side,
                      OrderId aggressor_id,
                      OrderId resting_order_id,
                      Price price,
                      Quantity quantity);
    static RestingOrderView view_of(const OrderNode& node, Price price) noexcept;

    bool is_triggered(const OrderRequest& stop) const noexcept;
    void add_stop(const OrderRequest& order, SequenceNumber sequence);
    void remove_stop(Slot stop_slot);
    void activate_stop(const OrderRequest& stop, SubmitResult& result);
    void process_triggered_stops();

    Slot allocate_node();
    void release_node(Slot node_slot) noexcept;
    Slot allocate_level();
    void release_level(Slot level_slot) noexcept;
    Slot allocate_stop();

    std::vector<OrderNode> nodes_;
    std::vector<PriceLevel> levels_;
    Slot free_nodes_{kNone};
    Slot free_levels_{kNone};
    Ladder bids_;
    Ladder asks_;
    detail::OrderIndex index_;

    std::vector<StopOrder> stops_;
    std::vector<Slot> free_stops_;
    StopLadder buy_stops_;
    StopLadder sell_stops_;
    detail::OrderIndex stop_index_;

    std::vector<Trade> trades_;
    std::vector<OrderId> triggered_;
    std::vector<OrderId> self_trade_cancels_;
    std::optional<Price> last_trade_price_;
    BookListener* listener_{nullptr};
    std::vector<TouchedLevel> touched_levels_;
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch

#endif
