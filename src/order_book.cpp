#include "tradematch/order_book.hpp"

#include "tradematch/detail/config.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <stdexcept>

namespace tradematch {

namespace {

constexpr bool can_rest(TimeInForce time_in_force) noexcept {
    return time_in_force == TimeInForce::GoodTillCancel || time_in_force == TimeInForce::Day;
}

constexpr bool is_stop(OrderType type) noexcept {
    return type == OrderType::Stop || type == OrderType::StopLimit;
}

constexpr bool has_limit_price(OrderType type) noexcept {
    return type == OrderType::Limit || type == OrderType::StopLimit;
}

constexpr bool uses_advanced_features(const OrderRequest& order) noexcept {
    return is_stop(order.type) || order.stop_price != 0 || order.display_quantity != 0U || order.post_only;
}

constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr bool crosses(Side aggressor, Price limit, Price resting) noexcept {
    return aggressor == Side::Buy ? resting <= limit : resting >= limit;
}

constexpr bool self_trade_checks(const OrderRequest& order) noexcept {
    return order.owner != 0U && order.self_trade_prevention != SelfTradePrevention::None;
}

// Bids are stored ascending and asks descending, so in both ladders the best price sits at the back.
template <typename Ladder>
auto ladder_lower_bound(Ladder& ladder, Side side, Price price) {
    if (side == Side::Buy) {
        return std::ranges::lower_bound(ladder, price, std::less{}, [](const auto& entry) { return entry.price; });
    }
    return std::ranges::lower_bound(ladder, price, std::greater{}, [](const auto& entry) { return entry.price; });
}

// Stop ladders keep the next stop to trigger at the back: buy stops descend and sell stops ascend by trigger price,
// and among equal trigger prices later arrivals sort first, so the earliest arrival triggers first.
template <typename Entry>
bool stop_sorts_before(Side side, const Entry& left, const Entry& right) noexcept {
    if (left.stop_price != right.stop_price) {
        return side == Side::Buy ? left.stop_price > right.stop_price : left.stop_price < right.stop_price;
    }
    return left.sequence > right.sequence;
}

}  // namespace

OrderBook::OrderBook(const BookCapacity& capacity) : index_(capacity.orders), stop_index_(capacity.stop_orders) {
    nodes_.reserve(capacity.orders);
    levels_.reserve(capacity.price_levels * 2);
    bids_.reserve(capacity.price_levels);
    asks_.reserve(capacity.price_levels);
    stops_.reserve(capacity.stop_orders);
    free_stops_.reserve(capacity.stop_orders);
    buy_stops_.reserve(capacity.stop_orders);
    sell_stops_.reserve(capacity.stop_orders);
    trades_.reserve(capacity.trades_per_call);
    triggered_.reserve(capacity.stop_orders);
    self_trade_cancels_.reserve(capacity.trades_per_call);
    touched_levels_.reserve(capacity.price_levels);
}

TRADEMATCH_HOT_INLINE OrderBook::Rejection OrderBook::validate(const OrderRequest& order,
                                                               bool replacing) const noexcept {
    if (order.order_id == 0U) {
        return {RejectReason::InvalidOrderId, "Order ID must be greater than zero."};
    }
    if (order.quantity == 0U) {
        return {RejectReason::InvalidQuantity, "Order quantity must be greater than zero."};
    }
    if (order.price < 0) {
        return {RejectReason::InvalidPrice, "Order price cannot be negative."};
    }
    if (has_limit_price(order.type) && order.price <= 0) {
        return {RejectReason::InvalidPrice, "Limit order price must be greater than zero."};
    }
    if (!has_limit_price(order.type) && order.price != 0) {
        return {RejectReason::InvalidPrice, "Market order price must be zero."};
    }
    if (uses_advanced_features(order)) {
        const auto rejection = validate_advanced(order);
        if (rejection.reason != RejectReason::None) {
            return rejection;
        }
    }
    if (!replacing &&
        (index_.contains(order.order_id) || (stop_index_.size() != 0U && stop_index_.contains(order.order_id)))) {
        return {RejectReason::DuplicateOrderId, "Order ID is already active in the book."};
    }
    return {};
}

// Stop, iceberg, and post-only checks, kept out of line so plain limit and market orders validate quickly.
TRADEMATCH_COLD OrderBook::Rejection OrderBook::validate_advanced(const OrderRequest& order) noexcept {
    if (is_stop(order.type) ? order.stop_price <= 0 : order.stop_price != 0) {
        return {RejectReason::InvalidStopPrice, "Stop orders need a positive stop price; other orders must use 0."};
    }
    if (order.display_quantity > 0U &&
        (!has_limit_price(order.type) || !can_rest(order.time_in_force) || order.display_quantity > order.quantity)) {
        return {RejectReason::InvalidDisplayQuantity,
                "Icebergs need a limit price, a resting time in force, and a display quantity within the order."};
    }
    if (order.post_only && (order.type != OrderType::Limit || !can_rest(order.time_in_force))) {
        return {RejectReason::InvalidPostOnly, "Post-only requires a limit order that can rest (GTC or DAY)."};
    }
    return {};
}

TRADEMATCH_HOT_INLINE void OrderBook::begin_request() noexcept {
    trades_.clear();
    triggered_.clear();
    self_trade_cancels_.clear();
}

TRADEMATCH_HOT_INLINE void OrderBook::finish_request(SubmitResult& result) {
    result.trades = trades_;
    result.triggered_stops = triggered_;
    result.self_trade_cancels = self_trade_cancels_;
    if (listener_ != nullptr) {
        publish(trades_);
    }
}

TRADEMATCH_HOT_INLINE void OrderBook::touch_level(Side side, Price price) {
    if (listener_ != nullptr) {
        touched_levels_.push_back(TouchedLevel{side, price});
    }
}

// Emits the request's trades, then the final state of every level it changed, bids before asks and in ascending price
// order, so the output is deterministic.
TRADEMATCH_COLD void OrderBook::publish(std::span<const Trade> trades) {
    for (const Trade& trade : trades) {
        listener_->on_trade(trade);
    }

    std::ranges::sort(touched_levels_);
    const auto duplicates = std::ranges::unique(touched_levels_);
    touched_levels_.erase(duplicates.begin(), duplicates.end());
    for (const TouchedLevel& touched : touched_levels_) {
        LevelUpdate update{touched.side, touched.price, 0U, 0U};
        const auto entry = find_ladder_entry(touched.side, touched.price);
        if (entry != ladder(touched.side).end()) {
            const PriceLevel& level = levels_[entry->level];
            update.quantity = level.total_quantity;
            update.order_count = level.order_count;
        }
        listener_->on_level_update(update);
    }
    touched_levels_.clear();
}

SubmitResult OrderBook::submit(const OrderRequest& order) {
    begin_request();

    SubmitResult result;
    result.order_id = order.order_id;
    result.remaining_quantity = order.quantity;

    const auto rejection = validate(order, false);
    if (rejection.reason != RejectReason::None) {
        result.reject_reason = rejection.reason;
        result.message = rejection.message;
        finish_request(result);
        return result;
    }
    if (order.post_only && would_cross(order)) {
        result.reject_reason = RejectReason::PostOnlyWouldCross;
        result.message = "Post-only order would take liquidity and was rejected.";
        finish_request(result);
        return result;
    }

    if (is_stop(order.type)) {
        const SequenceNumber arrival = next_sequence_++;
        result.accepted = true;
        if (!is_triggered(order)) {
            add_stop(order, arrival);
            result.pending_trigger = true;
            result.message = "Stop order accepted; waiting for its trigger price.";
            finish_request(result);
            return result;
        }
        activate_stop(order, result);
    } else {
        execute(order, result);
    }

    if (has_pending_stops()) {
        process_triggered_stops();
    }
    finish_request(result);
    return result;
}

void OrderBook::execute(const OrderRequest& order, SubmitResult& result) {
    const SequenceNumber sequence = next_sequence_++;
    result.accepted = true;

    if (order.time_in_force == TimeInForce::FillOrKill && available_liquidity(order) < order.quantity) {
        result.expired = true;
        result.message = "Fill-or-kill order could not be filled in full and was cancelled.";
        return;
    }

    Quantity remaining = order.quantity;
    bool stopped_by_self_trade = false;
    match(order, remaining, stopped_by_self_trade);

    result.remaining_quantity = remaining;
    result.filled_quantity = order.quantity - remaining;

    if (remaining == 0U) {
        result.message = "Order fully filled.";
        return;
    }

    if (stopped_by_self_trade) {
        result.expired = true;
        result.self_trade_prevented = true;
        result.message = "Self-trade prevention cancelled the remaining quantity.";
        return;
    }

    if (order.type == OrderType::Market) {
        result.expired = true;
        result.message = result.filled_quantity > 0U
                             ? "Market order consumed available liquidity and expired with an unfilled remainder."
                             : "Market order expired because no opposing liquidity was available.";
        return;
    }

    if (!can_rest(order.time_in_force)) {
        result.expired = true;
        result.message = result.filled_quantity > 0U
                             ? "Immediate-or-cancel order filled what it could; the remainder was cancelled."
                             : "Immediate-or-cancel order found no liquidity and was cancelled.";
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
    if (node_slot != detail::OrderIndex::kMissing) {
        const OrderNode& node = nodes_[node_slot];
        result.cancelled = true;
        result.cancelled_quantity = node.remaining + node.hidden;
        result.message = "Order cancelled.";
        remove_resting(node_slot);
        if (listener_ != nullptr) {
            publish({});
        }
        return result;
    }

    const Slot stop_slot = stop_index_.find(order_id);
    if (stop_slot != detail::OrderIndex::kMissing) {
        result.cancelled = true;
        result.cancelled_quantity = stops_[stop_slot].request.quantity;
        result.message = "Stop order cancelled before it triggered.";
        remove_stop(stop_slot);
        return result;
    }

    result.reject_reason = RejectReason::UnknownOrder;
    result.message = "Order not found.";
    return result;
}

ReplaceResult OrderBook::replace(const OrderRequest& replacement) {
    begin_request();

    ReplaceResult result;
    result.order_id = replacement.order_id;
    const auto reject = [&](RejectReason reason, std::string_view message) {
        result.reject_reason = reason;
        result.message = message;
        finish_request(result.submit_result);
        return result;
    };

    const Slot node_slot = index_.find(replacement.order_id);
    if (node_slot == detail::OrderIndex::kMissing) {
        return reject(RejectReason::UnknownOrder, "Order not found.");
    }

    OrderNode& existing = nodes_[node_slot];
    if (replacement.side != existing.side) {
        return reject(RejectReason::SideChange, "Replace cannot change the order side.");
    }
    if (replacement.type != OrderType::Limit) {
        return reject(RejectReason::TypeChange, "Replace must keep the limit order type of the resting order.");
    }

    const auto rejection = validate(replacement, true);
    if (rejection.reason != RejectReason::None) {
        return reject(rejection.reason, rejection.message);
    }
    if (!can_rest(replacement.time_in_force)) {
        return reject(RejectReason::InvalidTimeInForce,
                      "A replacement must keep a time in force that can rest (GTC or DAY).");
    }

    const Quantity open_quantity = existing.remaining + existing.hidden;
    result.previous_remaining_quantity = open_quantity;
    PriceLevel& level = levels_[existing.level];

    // Same price, same display, and no larger quantity: amend in place, as exchanges do, so the order keeps its queue
    // position. Any reduction comes out of the hidden reserve first.
    if (replacement.price == level.price && replacement.quantity <= open_quantity &&
        replacement.display_quantity == existing.display) {
        const Quantity visible = std::min(existing.remaining, replacement.quantity);
        const Quantity hidden = replacement.quantity - visible;
        level.total_quantity -= existing.remaining - visible;
        level.hidden_quantity -= existing.hidden - hidden;
        existing.remaining = visible;
        existing.hidden = hidden;
        existing.time_in_force = replacement.time_in_force;
        touch_level(existing.side, level.price);

        result.replaced = true;
        result.priority_kept = true;
        result.submit_result.order_id = replacement.order_id;
        result.submit_result.accepted = true;
        result.submit_result.rested = true;
        result.submit_result.remaining_quantity = replacement.quantity;
        result.submit_result.message = "Order amended in place.";
        result.message = "Order quantity reduced in place; time priority kept.";
        finish_request(result.submit_result);
        return result;
    }

    if (replacement.post_only && would_cross(replacement)) {
        return reject(RejectReason::PostOnlyWouldCross, "Post-only replacement would take liquidity and was rejected.");
    }

    // A new price, a new display size, or a larger quantity is cancel plus resubmit under the same ID, so time
    // priority resets.
    remove_resting(node_slot);
    result.submit_result.order_id = replacement.order_id;
    result.submit_result.remaining_quantity = replacement.quantity;
    execute(replacement, result.submit_result);
    if (has_pending_stops()) {
        process_triggered_stops();
    }
    finish_request(result.submit_result);

    result.replaced = true;
    result.message = result.submit_result.rested
                         ? "Order replaced and re-entered the book with a new priority timestamp."
                         : "Order replaced and fully filled immediately.";
    return result;
}

std::vector<OrderId> OrderBook::expire_day_orders() {
    std::vector<Slot> expiring_nodes;
    for (const Ladder* side_ladder : {&bids_, &asks_}) {
        for (const LadderEntry& entry : *side_ladder) {
            for (Slot node_slot = levels_[entry.level].head; node_slot != kNone; node_slot = nodes_[node_slot].next) {
                if (nodes_[node_slot].time_in_force == TimeInForce::Day) {
                    expiring_nodes.push_back(node_slot);
                }
            }
        }
    }

    std::vector<Slot> expiring_stops;
    for (const StopLadder* side_stops : {&buy_stops_, &sell_stops_}) {
        for (const StopEntry& entry : *side_stops) {
            if (stops_[entry.stop].request.time_in_force == TimeInForce::Day) {
                expiring_stops.push_back(entry.stop);
            }
        }
    }

    std::vector<OrderId> expired;
    expired.reserve(expiring_nodes.size() + expiring_stops.size());
    for (const Slot node_slot : expiring_nodes) {
        expired.push_back(nodes_[node_slot].order_id);
        remove_resting(node_slot);
    }
    for (const Slot stop_slot : expiring_stops) {
        expired.push_back(stops_[stop_slot].request.order_id);
        remove_stop(stop_slot);
    }
    std::ranges::sort(expired);
    if (listener_ != nullptr) {
        publish({});
    }
    return expired;
}

bool OrderBook::has_order(OrderId order_id) const noexcept {
    return index_.contains(order_id);
}

bool OrderBook::has_stop_order(OrderId order_id) const noexcept {
    return stop_index_.contains(order_id);
}

RestingOrderView OrderBook::view_of(const OrderNode& node, Price price) noexcept {
    return RestingOrderView{node.order_id,
                            node.side,
                            price,
                            node.remaining + node.hidden,
                            node.sequence,
                            node.time_in_force,
                            node.remaining,
                            node.display,
                            node.owner};
}

std::optional<RestingOrderView> OrderBook::get_order(OrderId order_id) const {
    const Slot node_slot = index_.find(order_id);
    if (node_slot == detail::OrderIndex::kMissing) {
        return std::nullopt;
    }
    const OrderNode& node = nodes_[node_slot];
    return view_of(node, levels_[node.level].price);
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
        orders.push_back(view_of(nodes_[node_slot], level.price));
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

std::optional<Price> OrderBook::last_trade_price() const noexcept {
    return last_trade_price_;
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

bool OrderBook::would_cross(const OrderRequest& order) const noexcept {
    const Ladder& opposing = ladder(opposite(order.side));
    return !opposing.empty() && crosses(order.side, order.price, opposing.back().price);
}

// Quantity the order could execute right now, walking liquidity in exactly the order matching would. Iceberg reserves
// count, since matching refreshes them; with self-trade prevention, the order's own resting orders are either skipped
// (cancel oldest) or end the walk (cancel newest or both), just as they would end matching. Stops early once the
// order's quantity is covered, so a FOK check stays cheap even on deep books.
TRADEMATCH_COLD Volume OrderBook::available_liquidity(const OrderRequest& order) const noexcept {
    const bool check_self_trades = self_trade_checks(order);
    Volume available = 0;
    for (const LadderEntry& entry : std::views::reverse(ladder(opposite(order.side)))) {
        if (order.type == OrderType::Limit && !crosses(order.side, order.price, entry.price)) {
            break;
        }
        const PriceLevel& level = levels_[entry.level];
        if (!check_self_trades) {
            available += level.total_quantity + level.hidden_quantity;
        } else {
            Volume reserves = 0;
            for (Slot node_slot = level.head; node_slot != kNone; node_slot = nodes_[node_slot].next) {
                const OrderNode& node = nodes_[node_slot];
                if (node.owner == order.owner) {
                    if (order.self_trade_prevention == SelfTradePrevention::CancelOldest) {
                        continue;
                    }
                    return available;
                }
                available += node.remaining;
                if (available >= order.quantity) {
                    return available;
                }
                reserves += node.hidden;
            }
            available += reserves;
        }
        if (available >= order.quantity) {
            break;
        }
    }
    return available;
}

void OrderBook::match(const OrderRequest& order, Quantity& remaining, bool& stopped_by_self_trade) {
    const bool check_self_trades = self_trade_checks(order);
    Ladder& opposing = ladder(opposite(order.side));

    while (remaining > 0U && !opposing.empty()) {
        const LadderEntry best = opposing.back();
        if (order.type == OrderType::Limit && !crosses(order.side, order.price, best.price)) {
            return;
        }

        PriceLevel& level = levels_[best.level];
        touch_level(level.side, best.price);
        while (remaining > 0U && level.head != kNone) {
            const Slot node_slot = level.head;
            OrderNode& resting = nodes_[node_slot];

            if (check_self_trades && resting.owner == order.owner) {
                if (prevent_self_trade(order.self_trade_prevention, node_slot, level)) {
                    stopped_by_self_trade = true;
                    break;
                }
                continue;
            }

            const Quantity fill = std::min(remaining, resting.remaining);
            record_trade(order.side, order.order_id, resting.order_id, level.price, fill);
            remaining -= fill;
            resting.remaining -= fill;
            level.total_quantity -= fill;

            if (resting.remaining == 0U) {
                if (resting.hidden > 0U) {
                    refresh_iceberg(node_slot, level);
                    continue;
                }
                // A filled order is always the head of its queue, so unlink it without the general-case checks.
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
            opposing.pop_back();
            release_level(best.level);
        }
        if (stopped_by_self_trade) {
            return;
        }
    }
}

// Applies the incoming order's self-trade prevention to a resting order with the same owner. Returns whether matching
// must stop for the incoming order.
TRADEMATCH_COLD bool OrderBook::prevent_self_trade(SelfTradePrevention mode, Slot node_slot, PriceLevel& level) {
    if (mode != SelfTradePrevention::CancelNewest) {
        self_trade_cancels_.push_back(nodes_[node_slot].order_id);
        detach_node(node_slot, level);
    }
    return mode != SelfTradePrevention::CancelOldest;
}

// Shows the next tranche of an iceberg whose displayed quantity was just consumed. The refreshed tranche joins the
// back of the queue with a new sequence number, so hidden quantity never jumps ahead of displayed orders.
TRADEMATCH_COLD void OrderBook::refresh_iceberg(Slot node_slot, PriceLevel& level) {
    OrderNode& node = nodes_[node_slot];
    const Quantity tranche = std::min(node.display, node.hidden);
    node.hidden -= tranche;
    node.remaining = tranche;
    node.sequence = next_sequence_++;
    level.hidden_quantity -= tranche;
    level.total_quantity += tranche;

    if (level.tail == node_slot) {
        return;
    }
    if (node.prev != kNone) {
        nodes_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    nodes_[node.next].prev = node.prev;  // not the tail, so a successor exists

    node.prev = level.tail;
    node.next = kNone;
    nodes_[level.tail].next = node_slot;
    level.tail = node_slot;
}

void OrderBook::rest(const OrderRequest& order, Quantity remaining, SequenceNumber sequence) {
    const Slot level_slot = find_or_create_level(order.side, order.price);
    const Slot node_slot = allocate_node();

    const bool iceberg = order.display_quantity > 0U && order.display_quantity < remaining;
    const Quantity visible = iceberg ? order.display_quantity : remaining;

    PriceLevel& level = levels_[level_slot];
    OrderNode& node = nodes_[node_slot];
    node = OrderNode{order.order_id,
                     sequence,
                     visible,
                     remaining - visible,
                     iceberg ? order.display_quantity : 0U,
                     order.owner,
                     level_slot,
                     level.tail,
                     kNone,
                     order.side,
                     order.time_in_force};

    if (level.tail != kNone) {
        nodes_[level.tail].next = node_slot;
    } else {
        level.head = node_slot;
    }
    level.tail = node_slot;
    ++level.order_count;
    level.total_quantity += visible;
    level.hidden_quantity += remaining - visible;
    touch_level(order.side, order.price);

    index_.insert(order.order_id, node_slot);
}

// Unlinks a node from its level and frees it. The level itself stays, even when it becomes empty.
TRADEMATCH_HOT_INLINE void OrderBook::detach_node(Slot node_slot, PriceLevel& level) {
    const OrderNode& node = nodes_[node_slot];
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
    level.hidden_quantity -= node.hidden;
    --level.order_count;

    index_.erase(node.order_id);
    release_node(node_slot);
}

void OrderBook::remove_resting(Slot node_slot) {
    const Slot level_slot = nodes_[node_slot].level;
    PriceLevel& level = levels_[level_slot];
    touch_level(level.side, level.price);
    detach_node(node_slot, level);
    if (level.order_count == 0U) {
        remove_level(level_slot);
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
    levels_[level_slot] = PriceLevel{price, 0U, 0U, kNone, kNone, 0U, side};
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

TRADEMATCH_HOT_INLINE void OrderBook::record_trade(Side aggressor_side,
                                                   OrderId aggressor_id,
                                                   OrderId resting_order_id,
                                                   Price price,
                                                   Quantity quantity) {
    const bool buyer_is_aggressor = aggressor_side == Side::Buy;
    trades_.push_back(Trade{
        next_trade_id_++,
        buyer_is_aggressor ? aggressor_id : resting_order_id,
        buyer_is_aggressor ? resting_order_id : aggressor_id,
        price,
        quantity,
        aggressor_side,
    });
    last_trade_price_ = price;
}

bool OrderBook::is_triggered(const OrderRequest& stop) const noexcept {
    if (!last_trade_price_) {
        return false;
    }
    return stop.side == Side::Buy ? *last_trade_price_ >= stop.stop_price : *last_trade_price_ <= stop.stop_price;
}

TRADEMATCH_COLD void OrderBook::add_stop(const OrderRequest& order, SequenceNumber sequence) {
    const Slot stop_slot = allocate_stop();
    stops_[stop_slot] = StopOrder{order, sequence};

    StopLadder& side_stops = stop_ladder(order.side);
    const StopEntry entry{order.stop_price, sequence, stop_slot};
    const auto position =
        std::ranges::lower_bound(side_stops, entry, [side = order.side](const auto& left, const auto& right) {
            return stop_sorts_before(side, left, right);
        });
    side_stops.insert(position, entry);
    stop_index_.insert(order.order_id, stop_slot);
}

TRADEMATCH_COLD void OrderBook::remove_stop(Slot stop_slot) {
    const StopOrder& stop = stops_[stop_slot];
    const Side side = stop.request.side;
    StopLadder& side_stops = stop_ladder(side);

    if (side_stops.back().stop == stop_slot) {
        side_stops.pop_back();
    } else {
        const StopEntry key{stop.request.stop_price, stop.sequence, stop_slot};
        side_stops.erase(std::ranges::lower_bound(side_stops, key, [side](const auto& left, const auto& right) {
            return stop_sorts_before(side, left, right);
        }));
    }
    stop_index_.erase(stop.request.order_id);
    free_stops_.push_back(stop_slot);
}

TRADEMATCH_COLD void OrderBook::activate_stop(const OrderRequest& stop, SubmitResult& result) {
    triggered_.push_back(stop.order_id);

    OrderRequest activated = stop;
    activated.type = stop.type == OrderType::Stop ? OrderType::Market : OrderType::Limit;
    activated.stop_price = 0;
    result.order_id = stop.order_id;
    result.remaining_quantity = stop.quantity;
    execute(activated, result);
}

// Activates triggered stops one at a time until none remain. Each activation can trade, move the last trade price,
// and trigger further stops. Within a side, stops trigger by price and then arrival; when both sides have a
// triggered stop, the earlier arrival goes first.
TRADEMATCH_COLD void OrderBook::process_triggered_stops() {
    const auto triggered_back = [this](Side side) -> const StopEntry* {
        const StopLadder& side_stops = side == Side::Buy ? buy_stops_ : sell_stops_;
        if (side_stops.empty() || !last_trade_price_) {
            return nullptr;
        }
        const StopEntry& back = side_stops.back();
        const bool hit =
            side == Side::Buy ? *last_trade_price_ >= back.stop_price : *last_trade_price_ <= back.stop_price;
        return hit ? &back : nullptr;
    };

    while (true) {
        const StopEntry* buy = triggered_back(Side::Buy);
        const StopEntry* sell = triggered_back(Side::Sell);
        if (buy == nullptr && sell == nullptr) {
            return;
        }
        const StopEntry* next = buy;
        if (next == nullptr || (sell != nullptr && sell->sequence < buy->sequence)) {
            next = sell;
        }

        const Slot stop_slot = next->stop;
        const OrderRequest request = stops_[stop_slot].request;
        remove_stop(stop_slot);
        SubmitResult activation;
        activate_stop(request, activation);
    }
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

OrderBook::Slot OrderBook::allocate_stop() {
    if (!free_stops_.empty()) {
        const Slot stop_slot = free_stops_.back();
        free_stops_.pop_back();
        return stop_slot;
    }
    if (stops_.size() >= kNone) {
        throw std::length_error("order book stop pool exhausted");
    }
    stops_.emplace_back();
    return static_cast<Slot>(stops_.size() - 1);
}

}  // namespace tradematch
