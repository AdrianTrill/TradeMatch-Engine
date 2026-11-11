#ifndef TRADEMATCH_TESTS_REFERENCE_BOOK_HPP
#define TRADEMATCH_TESTS_REFERENCE_BOOK_HPP

#include "tradematch/types.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace tradematch::testing {

// Deliberately naive model of the matching rules. Every operation is a linear scan over flat vectors of resting and
// pending stop orders: far too slow for production, but small enough to be obviously correct. That makes it a
// trustworthy oracle for differential testing of the optimized OrderBook. It mirrors the engine's API, including
// views into reused buffers and reject reasons, but shares none of its algorithms: for example, a fill-or-kill check
// here simply runs the match on a throwaway copy of the whole book.
class ReferenceBook {
public:
    SubmitResult submit(const OrderRequest& order) {
        begin_request();
        SubmitResult result;
        result.order_id = order.order_id;
        result.remaining_quantity = order.quantity;

        auto reason = validation_reason(order);
        if (reason == RejectReason::None &&
            (find(order.order_id) != orders_.end() || find_stop(order.order_id) != stops_.end())) {
            reason = RejectReason::DuplicateOrderId;
        }
        if (reason == RejectReason::None && order.post_only && best_opposing_order(order) != orders_.end()) {
            reason = RejectReason::PostOnlyWouldCross;
        }
        if (reason != RejectReason::None) {
            result.reject_reason = reason;
            finish_request(result);
            return result;
        }

        if (is_stop(order.type)) {
            const SequenceNumber arrival = next_sequence_++;
            result.accepted = true;
            if (!is_triggered(order)) {
                stops_.push_back(PendingStop{order, arrival});
                result.pending_trigger = true;
                finish_request(result);
                return result;
            }
            activate(order, result);
        } else {
            execute(order, result);
        }
        process_stops();
        finish_request(result);
        return result;
    }

    CancelResult cancel(OrderId order_id) {
        CancelResult result;
        result.order_id = order_id;

        if (const auto it = find(order_id); it != orders_.end()) {
            result.cancelled = true;
            result.cancelled_quantity = it->visible + it->hidden;
            orders_.erase(it);
            return result;
        }
        if (const auto stop = find_stop(order_id); stop != stops_.end()) {
            result.cancelled = true;
            result.cancelled_quantity = stop->request.quantity;
            stops_.erase(stop);
            return result;
        }
        result.reject_reason = RejectReason::UnknownOrder;
        return result;
    }

    ReplaceResult replace(const OrderRequest& replacement) {
        begin_request();
        ReplaceResult result;
        result.order_id = replacement.order_id;

        const auto it = find(replacement.order_id);
        if (it == orders_.end()) {
            result.reject_reason = RejectReason::UnknownOrder;
        } else if (it->side != replacement.side) {
            result.reject_reason = RejectReason::SideChange;
        } else if (replacement.type != OrderType::Limit) {
            result.reject_reason = RejectReason::TypeChange;
        } else {
            result.reject_reason = validation_reason(replacement);
        }
        if (result.reject_reason == RejectReason::None && !can_rest(replacement.time_in_force)) {
            result.reject_reason = RejectReason::InvalidTimeInForce;
        }
        if (result.reject_reason != RejectReason::None) {
            finish_request(result.submit_result);
            return result;
        }

        const Quantity open_quantity = it->visible + it->hidden;
        result.previous_remaining_quantity = open_quantity;
        if (replacement.price == it->price && replacement.quantity <= open_quantity &&
            replacement.display_quantity == it->display) {
            const Quantity visible = std::min(it->visible, replacement.quantity);
            it->hidden = replacement.quantity - visible;
            it->visible = visible;
            it->time_in_force = replacement.time_in_force;
            result.replaced = true;
            result.priority_kept = true;
            result.submit_result.order_id = replacement.order_id;
            result.submit_result.accepted = true;
            result.submit_result.rested = true;
            result.submit_result.remaining_quantity = replacement.quantity;
            finish_request(result.submit_result);
            return result;
        }

        if (replacement.post_only && best_opposing_order(replacement) != orders_.end()) {
            result.reject_reason = RejectReason::PostOnlyWouldCross;
            finish_request(result.submit_result);
            return result;
        }
        orders_.erase(it);
        result.submit_result.order_id = replacement.order_id;
        result.submit_result.remaining_quantity = replacement.quantity;
        execute(replacement, result.submit_result);
        process_stops();
        finish_request(result.submit_result);
        result.replaced = true;
        return result;
    }

    std::vector<OrderId> expire_day_orders() {
        std::vector<OrderId> expired;
        for (const auto& order : orders_) {
            if (order.time_in_force == TimeInForce::Day) {
                expired.push_back(order.order_id);
            }
        }
        for (const auto& stop : stops_) {
            if (stop.request.time_in_force == TimeInForce::Day) {
                expired.push_back(stop.request.order_id);
            }
        }
        std::erase_if(orders_, [](const auto& order) { return order.time_in_force == TimeInForce::Day; });
        std::erase_if(stops_, [](const auto& stop) { return stop.request.time_in_force == TimeInForce::Day; });
        std::ranges::sort(expired);
        return expired;
    }

    std::optional<RestingOrderView> get_order(OrderId order_id) const {
        const auto it = std::ranges::find_if(orders_, [&](const auto& order) { return order.order_id == order_id; });
        if (it == orders_.end()) {
            return std::nullopt;
        }
        return RestingOrderView{it->order_id,
                                it->side,
                                it->price,
                                it->visible + it->hidden,
                                it->sequence,
                                it->time_in_force,
                                it->visible,
                                it->display,
                                it->owner};
    }

    BookSnapshot snapshot() const {
        auto sorted = orders_;
        std::ranges::sort(sorted, [](const auto& left, const auto& right) {
            if (left.side != right.side) {
                return left.side == Side::Buy;
            }
            if (left.price != right.price) {
                return left.side == Side::Buy ? left.price > right.price : left.price < right.price;
            }
            return left.sequence < right.sequence;
        });

        BookSnapshot book_snapshot;
        for (const auto& order : sorted) {
            auto& levels = order.side == Side::Buy ? book_snapshot.bids : book_snapshot.asks;
            if (levels.empty() || levels.back().price != order.price) {
                levels.push_back(PriceLevelView{order.price, 0U, 0U, {}});
            }
            auto& level = levels.back();
            level.total_quantity += order.visible;
            ++level.order_count;
            level.order_ids.push_back(order.order_id);
        }
        return book_snapshot;
    }

    std::size_t order_count() const noexcept {
        return orders_.size();
    }

    std::size_t stop_order_count() const noexcept {
        return stops_.size();
    }

    std::optional<Price> best_bid() const {
        return best_price(Side::Buy);
    }

    std::optional<Price> best_ask() const {
        return best_price(Side::Sell);
    }

    std::optional<Price> last_trade_price() const {
        return last_trade_price_;
    }

private:
    struct Resting {
        OrderId order_id{};
        Side side{Side::Buy};
        Price price{};
        Quantity visible{};
        Quantity hidden{};
        Quantity display{};
        SequenceNumber sequence{};
        TimeInForce time_in_force{TimeInForce::GoodTillCancel};
        OwnerId owner{};
    };

    struct PendingStop {
        OrderRequest request;
        SequenceNumber sequence{};
    };

    using Orders = std::vector<Resting>;

    static bool can_rest(TimeInForce time_in_force) {
        return time_in_force == TimeInForce::GoodTillCancel || time_in_force == TimeInForce::Day;
    }

    static bool is_stop(OrderType type) {
        return type == OrderType::Stop || type == OrderType::StopLimit;
    }

    static bool has_limit_price(OrderType type) {
        return type == OrderType::Limit || type == OrderType::StopLimit;
    }

    // Same checks, in the same priority order, as the engine's validation.
    static RejectReason validation_reason(const OrderRequest& order) {
        if (order.order_id == 0U) {
            return RejectReason::InvalidOrderId;
        }
        if (order.quantity == 0U) {
            return RejectReason::InvalidQuantity;
        }
        const bool price_ok = has_limit_price(order.type) ? order.price > 0 : order.price == 0;
        if (!price_ok) {
            return RejectReason::InvalidPrice;
        }
        const bool stop_price_ok = is_stop(order.type) ? order.stop_price > 0 : order.stop_price == 0;
        if (!stop_price_ok) {
            return RejectReason::InvalidStopPrice;
        }
        if (order.display_quantity > 0U && (!has_limit_price(order.type) || !can_rest(order.time_in_force) ||
                                            order.display_quantity > order.quantity)) {
            return RejectReason::InvalidDisplayQuantity;
        }
        if (order.post_only && (order.type != OrderType::Limit || !can_rest(order.time_in_force))) {
            return RejectReason::InvalidPostOnly;
        }
        return RejectReason::None;
    }

    void begin_request() {
        trades_.clear();
        triggered_.clear();
        self_trade_cancels_.clear();
    }

    void finish_request(SubmitResult& result) const {
        result.trades = trades_;
        result.triggered_stops = triggered_;
        result.self_trade_cancels = self_trade_cancels_;
    }

    bool is_triggered(const OrderRequest& stop) const {
        if (!last_trade_price_) {
            return false;
        }
        return stop.side == Side::Buy ? *last_trade_price_ >= stop.stop_price : *last_trade_price_ <= stop.stop_price;
    }

    // Matches the order against the book and returns how much remains. Implements price-time priority, iceberg
    // refresh, and self-trade prevention directly on the flat vector.
    Quantity match(const OrderRequest& order, Quantity remaining, bool& stopped_by_self_trade) {
        const bool self_trade_checks = order.owner != 0U && order.self_trade_prevention != SelfTradePrevention::None;
        while (remaining > 0U) {
            const auto best = best_opposing_order(order);
            if (best == orders_.end()) {
                break;
            }
            if (self_trade_checks && best->owner == order.owner) {
                const auto mode = order.self_trade_prevention;
                if (mode != SelfTradePrevention::CancelNewest) {
                    self_trade_cancels_.push_back(best->order_id);
                    orders_.erase(best);
                }
                if (mode != SelfTradePrevention::CancelOldest) {
                    stopped_by_self_trade = true;
                    break;
                }
                continue;
            }

            const Quantity traded = std::min(remaining, best->visible);
            Trade trade;
            trade.trade_id = next_trade_id_++;
            trade.buy_order_id = order.side == Side::Buy ? order.order_id : best->order_id;
            trade.sell_order_id = order.side == Side::Sell ? order.order_id : best->order_id;
            trade.price = best->price;
            trade.quantity = traded;
            trade.aggressor_side = order.side;
            trades_.push_back(trade);
            last_trade_price_ = best->price;

            remaining -= traded;
            best->visible -= traded;
            if (best->visible == 0U) {
                if (best->hidden > 0U) {
                    best->visible = std::min(best->display, best->hidden);
                    best->hidden -= best->visible;
                    best->sequence = next_sequence_++;  // a refreshed tranche joins the back of the queue
                } else {
                    orders_.erase(best);
                }
            }
        }
        return remaining;
    }

    // How much of a fill-or-kill order would trade: run the real match on a scratch copy of the book.
    Quantity fillable_quantity(const OrderRequest& order) const {
        ReferenceBook scratch = *this;
        bool stopped = false;
        return order.quantity - scratch.match(order, order.quantity, stopped);
    }

    void execute(const OrderRequest& order, SubmitResult& result) {
        result.accepted = true;
        const SequenceNumber sequence = next_sequence_++;

        if (order.time_in_force == TimeInForce::FillOrKill && fillable_quantity(order) < order.quantity) {
            result.expired = true;
            return;
        }

        bool stopped = false;
        const Quantity remaining = match(order, order.quantity, stopped);
        result.remaining_quantity = remaining;
        result.filled_quantity = order.quantity - remaining;
        if (remaining == 0U) {
            return;
        }
        if (stopped) {
            result.expired = true;
            result.self_trade_prevented = true;
            return;
        }
        if (order.type == OrderType::Market || !can_rest(order.time_in_force)) {
            result.expired = true;
            return;
        }

        const bool iceberg = order.display_quantity > 0U && order.display_quantity < remaining;
        const Quantity visible = iceberg ? order.display_quantity : remaining;
        orders_.push_back(Resting{order.order_id,
                                  order.side,
                                  order.price,
                                  visible,
                                  remaining - visible,
                                  iceberg ? order.display_quantity : 0U,
                                  sequence,
                                  order.time_in_force,
                                  order.owner});
        result.rested = true;
    }

    void activate(const OrderRequest& stop, SubmitResult& result) {
        triggered_.push_back(stop.order_id);
        OrderRequest activated = stop;
        activated.type = stop.type == OrderType::Stop ? OrderType::Market : OrderType::Limit;
        activated.stop_price = 0;
        result.order_id = stop.order_id;
        result.remaining_quantity = stop.quantity;
        execute(activated, result);
    }

    // The stop on one side that triggers first: lowest buy or highest sell trigger price, then earliest arrival.
    std::vector<PendingStop>::iterator best_triggered_stop(Side side) {
        auto best = stops_.end();
        for (auto it = stops_.begin(); it != stops_.end(); ++it) {
            if (it->request.side != side || !is_triggered(it->request)) {
                continue;
            }
            if (best == stops_.end()) {
                best = it;
                continue;
            }
            const bool better_price = side == Side::Buy ? it->request.stop_price < best->request.stop_price
                                                        : it->request.stop_price > best->request.stop_price;
            const bool same_price = it->request.stop_price == best->request.stop_price;
            if (better_price || (same_price && it->sequence < best->sequence)) {
                best = it;
            }
        }
        return best;
    }

    // Triggered stops activate one at a time, and when both sides have one, the earlier arrival goes first.
    void process_stops() {
        while (true) {
            const auto buy = best_triggered_stop(Side::Buy);
            const auto sell = best_triggered_stop(Side::Sell);
            auto next = buy;
            if (next == stops_.end() || (sell != stops_.end() && sell->sequence < buy->sequence)) {
                next = sell;
            }
            if (next == stops_.end()) {
                return;
            }
            const OrderRequest request = next->request;
            stops_.erase(next);
            SubmitResult activation;
            activate(request, activation);
        }
    }

    std::optional<Price> best_price(Side side) const {
        std::optional<Price> best;
        for (const auto& order : orders_) {
            if (order.side != side) {
                continue;
            }
            if (!best || (side == Side::Buy ? order.price > *best : order.price < *best)) {
                best = order.price;
            }
        }
        return best;
    }

    Orders::iterator find(OrderId order_id) {
        return std::ranges::find_if(orders_, [&](const auto& order) { return order.order_id == order_id; });
    }

    std::vector<PendingStop>::iterator find_stop(OrderId order_id) {
        return std::ranges::find_if(stops_, [&](const auto& stop) { return stop.request.order_id == order_id; });
    }

    // Best price first, then oldest sequence: a direct transcription of price-time priority.
    Orders::iterator best_opposing_order(const OrderRequest& incoming) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == incoming.side) {
                continue;
            }

            const bool buying = incoming.side == Side::Buy;
            if (incoming.type == OrderType::Limit &&
                (buying ? it->price > incoming.price : it->price < incoming.price)) {
                continue;
            }

            if (best == orders_.end()) {
                best = it;
                continue;
            }

            const bool better_price = buying ? it->price < best->price : it->price > best->price;
            if (better_price || (it->price == best->price && it->sequence < best->sequence)) {
                best = it;
            }
        }
        return best;
    }

    Orders orders_;
    std::vector<PendingStop> stops_;
    std::vector<Trade> trades_;
    std::vector<OrderId> triggered_;
    std::vector<OrderId> self_trade_cancels_;
    std::optional<Price> last_trade_price_;
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch::testing

#endif
