#include "tradematch/order_book.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <stdexcept>

namespace tradematch {

namespace {

constexpr bool crosses(Side aggressor, Price limit, Price resting) noexcept {
    return aggressor == Side::Buy ? resting <= limit : resting >= limit;
}

// Bids are stored ascending and asks descending, so in both ladders the best price sits at the back.
template <typename Ladder>
auto ladder_lower_bound(Ladder& ladder, Side side, Price price) {
    if (side == Side::Buy) {
        return std::ranges::lower_bound(ladder, price, std::less{}, [](const auto& entry) { return entry.price; });
    }
    return std::ranges::lower_bound(ladder, price, std::greater{}, [](const auto& entry) { return entry.price; });
}

}  // namespace

OrderBook::OrderBook(const BookCapacity& capacity) : index_(capacity.orders) {
    nodes_.reserve(capacity.orders);
    levels_.reserve(capacity.price_levels * 2);
    bids_.reserve(capacity.price_levels);
    asks_.reserve(capacity.price_levels);
    trades_.reserve(capacity.trades_per_call);
}

OrderBook::Rejection OrderBook::validate(const OrderRequest& order, bool replacing) const noexcept {
    if (order.order_id == 0U) {
        return {RejectReason::InvalidOrderId, "Order ID must be greater than zero."};
    }
    if (order.quantity == 0U) {
        return {RejectReason::InvalidQuantity, "Order quantity must be greater than zero."};
    }
    if (order.price < 0) {
        return {RejectReason::InvalidPrice, "Order price cannot be negative."};
    }
    if (order.type == OrderType::Limit && order.price <= 0) {
        return {RejectReason::InvalidPrice, "Limit order price must be greater than zero."};
    }
    if (order.type == OrderType::Market && order.price != 0) {
        return {RejectReason::InvalidPrice, "Market order price must be zero."};
    }
    if (!replacing && index_.contains(order.order_id)) {
        return {RejectReason::DuplicateOrderId, "Order ID is already active in the book."};
    }
    return {};
}

SubmitResult OrderBook::submit(const OrderRequest& order) {
    trades_.clear();

    SubmitResult result;
    result.order_id = order.order_id;
    result.remaining_quantity = order.quantity;

    const auto rejection = validate(order, false);
    if (rejection.reason != RejectReason::None) {
        result.reject_reason = rejection.reason;
        result.message = rejection.message;
        return result;
    }

    execute(order, result);
    return result;
}

void OrderBook::execute(const OrderRequest& order, SubmitResult& result) {
    const SequenceNumber sequence = next_sequence_++;
    result.accepted = true;

    Quantity remaining = order.quantity;
    match(order, remaining);

    result.trades = trades_;
    result.remaining_quantity = remaining;
    result.filled_quantity = order.quantity - remaining;

    if (remaining == 0U) {
        result.message = "Order fully filled.";
        return;
    }

    if (order.type == OrderType::Market) {
        result.expired = true;
        result.message = result.filled_quantity > 0U
                             ? "Market order consumed available liquidity and expired with an unfilled remainder."
                             : "Market order expired because no opposing liquidity was available.";
        return;
    }

    rest(order, remaining, sequence);
    result.rested = true;
    result.message = result.filled_quantity > 0U ? "Order partially filled and remainder added to the book."
                                                 : "Order added to the book.";
}

CancelResult OrderBook::cancel(OrderId order_id) {
    CancelResult result;
    result.order_id = order_id;

    const Slot node_slot = index_.find(order_id);
    if (node_slot == detail::OrderIndex::kMissing) {
        result.reject_reason = RejectReason::UnknownOrder;
        result.message = "Order not found.";
        return result;
    }

    result.cancelled = true;
    result.cancelled_quantity = nodes_[node_slot].remaining;
    result.message = "Order cancelled.";
    remove_resting(node_slot);
    return result;
}

ReplaceResult OrderBook::replace(const OrderRequest& replacement) {
    trades_.clear();

    ReplaceResult result;
    result.order_id = replacement.order_id;

    const Slot node_slot = index_.find(replacement.order_id);
    if (node_slot == detail::OrderIndex::kMissing) {
        result.reject_reason = RejectReason::UnknownOrder;
        result.message = "Order not found.";
        return result;
    }

    const OrderNode& existing = nodes_[node_slot];
    if (replacement.side != existing.side) {
        result.reject_reason = RejectReason::SideChange;
        result.message = "Replace cannot change the order side.";
        return result;
    }
    if (replacement.type != OrderType::Limit) {
        result.reject_reason = RejectReason::TypeChange;
        result.message = "Replace must keep the limit order type of the resting order.";
        return result;
    }

    const auto rejection = validate(replacement, true);
    if (rejection.reason != RejectReason::None) {
        result.reject_reason = rejection.reason;
        result.message = rejection.message;
        return result;
    }

    // Replace is cancel plus resubmit under the same ID, so the order intentionally loses its time priority.
    result.previous_remaining_quantity = existing.remaining;
    remove_resting(node_slot);

    result.submit_result.order_id = replacement.order_id;
    result.submit_result.remaining_quantity = replacement.quantity;
    execute(replacement, result.submit_result);

    result.replaced = true;
    result.message = result.submit_result.rested
                         ? "Order replaced and re-entered the book with a new priority timestamp."
                         : "Order replaced and fully filled immediately.";
    return result;
}

bool OrderBook::has_order(OrderId order_id) const noexcept {
    return index_.contains(order_id);
}

std::optional<RestingOrderView> OrderBook::get_order(OrderId order_id) const {
    const Slot node_slot = index_.find(order_id);
    if (node_slot == detail::OrderIndex::kMissing) {
        return std::nullopt;
    }
    const OrderNode& node = nodes_[node_slot];
    return RestingOrderView{node.order_id, node.side, levels_[node.level].price, node.remaining, node.sequence};
}

std::vector<RestingOrderView> OrderBook::get_orders_at_level(Side side, Price price) const {
    std::vector<RestingOrderView> orders;
    const auto entry = find_ladder_entry(side, price);
    if (entry == ladder(side).end()) {
        return orders;
    }

    const PriceLevel& level = levels_[entry->level];
    orders.reserve(level.order_count);
    for (Slot node_slot = level.head; node_slot != kNone; node_slot = nodes_[node_slot].next) {
        const OrderNode& node = nodes_[node_slot];
        orders.push_back(RestingOrderView{node.order_id, node.side, level.price, node.remaining, node.sequence});
    }
    return orders;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.back().price;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.back().price;
}

BookSnapshot OrderBook::snapshot() const {
    const auto collect = [this](const Ladder& side_ladder, std::vector<PriceLevelView>& views) {
        views.reserve(side_ladder.size());
        for (const LadderEntry& entry : std::views::reverse(side_ladder)) {
            const PriceLevel& level = levels_[entry.level];
            PriceLevelView view{level.price, level.total_quantity, level.order_count, {}};
            view.order_ids.reserve(level.order_count);
            for (Slot node_slot = level.head; node_slot != kNone; node_slot = nodes_[node_slot].next) {
                view.order_ids.push_back(nodes_[node_slot].order_id);
            }
            views.push_back(std::move(view));
        }
    };

    BookSnapshot book_snapshot;
    collect(bids_, book_snapshot.bids);
    collect(asks_, book_snapshot.asks);
    return book_snapshot;
}

void OrderBook::match(const OrderRequest& order, Quantity& remaining) {
    Ladder& opposite = ladder(order.side == Side::Buy ? Side::Sell : Side::Buy);

    while (remaining > 0U && !opposite.empty()) {
        const LadderEntry best = opposite.back();
        if (order.type == OrderType::Limit && !crosses(order.side, order.price, best.price)) {
            return;
        }

        PriceLevel& level = levels_[best.level];
        while (remaining > 0U && level.head != kNone) {
            const Slot node_slot = level.head;
            OrderNode& resting = nodes_[node_slot];
            const Quantity fill = std::min(remaining, resting.remaining);

            record_trade(order, resting.order_id, level.price, fill);
            remaining -= fill;
            resting.remaining -= fill;
            level.total_quantity -= fill;

            if (resting.remaining == 0U) {
                level.head = resting.next;
                if (level.head != kNone) {
                    nodes_[level.head].prev = kNone;
                } else {
                    level.tail = kNone;
                }
                --level.order_count;
                index_.erase(resting.order_id);
                release_node(node_slot);
            }
        }

        if (level.head == kNone) {
            opposite.pop_back();
            release_level(best.level);
        }
    }
}

void OrderBook::rest(const OrderRequest& order, Quantity remaining, SequenceNumber sequence) {
    const Slot level_slot = find_or_create_level(order.side, order.price);
    const Slot node_slot = allocate_node();

    PriceLevel& level = levels_[level_slot];
    OrderNode& node = nodes_[node_slot];
    node = OrderNode{order.order_id, sequence, remaining, level_slot, level.tail, kNone, order.side};

    if (level.tail != kNone) {
        nodes_[level.tail].next = node_slot;
    } else {
        level.head = node_slot;
    }
    level.tail = node_slot;
    ++level.order_count;
    level.total_quantity += remaining;

    index_.insert(order.order_id, node_slot);
}

void OrderBook::remove_resting(Slot node_slot) {
    const OrderNode node = nodes_[node_slot];
    PriceLevel& level = levels_[node.level];

    if (node.prev != kNone) {
        nodes_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNone) {
        nodes_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    level.total_quantity -= node.remaining;
    --level.order_count;

    index_.erase(node.order_id);
    release_node(node_slot);
    if (level.order_count == 0U) {
        remove_level(node.level);
    }
}

OrderBook::Slot OrderBook::find_or_create_level(Side side, Price price) {
    Ladder& side_ladder = ladder(side);
    if (!side_ladder.empty() && side_ladder.back().price == price) {
        return side_ladder.back().level;  // joining the best price is the most common case
    }

    const auto position = ladder_lower_bound(side_ladder, side, price);
    if (position != side_ladder.end() && position->price == price) {
        return position->level;
    }

    const Slot level_slot = allocate_level();
    levels_[level_slot] = PriceLevel{price, 0U, kNone, kNone, 0U, side};
    side_ladder.insert(position, LadderEntry{price, level_slot});
    return level_slot;
}

void OrderBook::remove_level(Slot level_slot) {
    const PriceLevel& level = levels_[level_slot];
    Ladder& side_ladder = ladder(level.side);

    if (side_ladder.back().level == level_slot) {
        side_ladder.pop_back();
    } else {
        side_ladder.erase(ladder_lower_bound(side_ladder, level.side, level.price));
    }
    release_level(level_slot);
}

OrderBook::Ladder::const_iterator OrderBook::find_ladder_entry(Side side, Price price) const {
    const Ladder& side_ladder = ladder(side);
    const auto position = ladder_lower_bound(side_ladder, side, price);
    if (position != side_ladder.end() && position->price == price) {
        return position;
    }
    return side_ladder.end();
}

void OrderBook::record_trade(const OrderRequest& aggressor, OrderId resting_order_id, Price price, Quantity quantity) {
    const bool buyer_is_aggressor = aggressor.side == Side::Buy;
    trades_.push_back(Trade{
        next_trade_id_++,
        buyer_is_aggressor ? aggressor.order_id : resting_order_id,
        buyer_is_aggressor ? resting_order_id : aggressor.order_id,
        price,
        quantity,
        aggressor.side,
    });
}

OrderBook::Slot OrderBook::allocate_node() {
    if (free_nodes_ != kNone) {
        const Slot node_slot = free_nodes_;
        free_nodes_ = nodes_[node_slot].next;
        return node_slot;
    }
    if (nodes_.size() >= kNone) {
        throw std::length_error("order book node pool exhausted");
    }
    nodes_.emplace_back();
    return static_cast<Slot>(nodes_.size() - 1);
}

void OrderBook::release_node(Slot node_slot) noexcept {
    nodes_[node_slot].next = free_nodes_;
    free_nodes_ = node_slot;
}

OrderBook::Slot OrderBook::allocate_level() {
    if (free_levels_ != kNone) {
        const Slot level_slot = free_levels_;
        free_levels_ = levels_[level_slot].tail;
        return level_slot;
    }
    if (levels_.size() >= kNone) {
        throw std::length_error("order book level pool exhausted");
    }
    levels_.emplace_back();
    return static_cast<Slot>(levels_.size() - 1);
}

void OrderBook::release_level(Slot level_slot) noexcept {
    levels_[level_slot].tail = free_levels_;
    free_levels_ = level_slot;
}

}  // namespace tradematch
