#ifndef TRADEMATCH_TESTS_REFERENCE_BOOK_HPP
#define TRADEMATCH_TESTS_REFERENCE_BOOK_HPP

#include "tradematch/types.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace tradematch::testing {

// Deliberately naive model of the matching rules. Every operation is a linear scan over one flat vector of
// resting orders: far too slow for production, but small enough to be obviously correct. That makes it a
// trustworthy oracle for differential testing of the optimized OrderBook. It mirrors the engine's API, including
// trade views into a reused buffer and reject reasons.
class ReferenceBook {
public:
    SubmitResult submit(const OrderRequest& order) {
        trades_.clear();
        SubmitResult result;
        result.order_id = order.order_id;
        result.remaining_quantity = order.quantity;

        auto reason = validation_reason(order);
        if (reason == RejectReason::None && find(order.order_id) != orders_.end()) {
            reason = RejectReason::DuplicateOrderId;
        }
        if (reason == RejectReason::None && order.post_only && best_opposing_order(order) != orders_.end()) {
            reason = RejectReason::PostOnlyWouldCross;
        }
        if (reason != RejectReason::None) {
            result.reject_reason = reason;
            return result;
        }

        execute(order, result);
        return result;
    }

    CancelResult cancel(OrderId order_id) {
        CancelResult result;
        result.order_id = order_id;

        const auto it = find(order_id);
        if (it == orders_.end()) {
            result.reject_reason = RejectReason::UnknownOrder;
            return result;
        }

        result.cancelled = true;
        result.cancelled_quantity = it->remaining_quantity;
        orders_.erase(it);
        return result;
    }

    ReplaceResult replace(const OrderRequest& replacement) {
        trades_.clear();
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
            return result;
        }

        result.previous_remaining_quantity = it->remaining_quantity;
        if (replacement.price == it->price && replacement.quantity <= it->remaining_quantity) {
            it->remaining_quantity = replacement.quantity;
            it->time_in_force = replacement.time_in_force;
            result.replaced = true;
            result.priority_kept = true;
            result.submit_result.order_id = replacement.order_id;
            result.submit_result.accepted = true;
            result.submit_result.rested = true;
            result.submit_result.remaining_quantity = replacement.quantity;
            result.submit_result.trades = trades_;
            return result;
        }

        if (replacement.post_only && best_opposing_order(replacement) != orders_.end()) {
            result.reject_reason = RejectReason::PostOnlyWouldCross;
            return result;
        }
        orders_.erase(it);
        result.submit_result.order_id = replacement.order_id;
        result.submit_result.remaining_quantity = replacement.quantity;
        execute(replacement, result.submit_result);
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
        std::erase_if(orders_, [](const auto& order) { return order.time_in_force == TimeInForce::Day; });
        std::ranges::sort(expired);
        return expired;
    }

    std::optional<RestingOrderView> get_order(OrderId order_id) const {
        const auto it = std::ranges::find_if(orders_, [&](const auto& order) { return order.order_id == order_id; });
        if (it == orders_.end()) {
            return std::nullopt;
        }
        return *it;
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
            level.total_quantity += order.remaining_quantity;
            ++level.order_count;
            level.order_ids.push_back(order.order_id);
        }
        return book_snapshot;
    }

    std::size_t order_count() const noexcept {
        return orders_.size();
    }

    std::optional<Price> best_bid() const {
        return best_price(Side::Buy);
    }

    std::optional<Price> best_ask() const {
        return best_price(Side::Sell);
    }

private:
    using Orders = std::vector<RestingOrderView>;

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

    static bool can_rest(TimeInForce time_in_force) {
        return time_in_force == TimeInForce::GoodTillCancel || time_in_force == TimeInForce::Day;
    }

    // Same checks, in the same priority order, as the engine's validation.
    static RejectReason validation_reason(const OrderRequest& order) {
        if (order.order_id == 0U) {
            return RejectReason::InvalidOrderId;
        }
        if (order.quantity == 0U) {
            return RejectReason::InvalidQuantity;
        }
        const bool price_ok = order.type == OrderType::Limit ? order.price > 0 : order.price == 0;
        if (!price_ok) {
            return RejectReason::InvalidPrice;
        }
        if (order.post_only && (order.type != OrderType::Limit || !can_rest(order.time_in_force))) {
            return RejectReason::InvalidPostOnly;
        }
        return RejectReason::None;
    }

    // Total opposing quantity whose price the order's limit reaches.
    Volume crossing_quantity(const OrderRequest& order) const {
        Volume total = 0;
        for (const auto& resting : orders_) {
            if (resting.side == order.side) {
                continue;
            }
            const bool reachable =
                order.type == OrderType::Market ||
                (order.side == Side::Buy ? resting.price <= order.price : resting.price >= order.price);
            if (reachable) {
                total += resting.remaining_quantity;
            }
        }
        return total;
    }

    void execute(const OrderRequest& order, SubmitResult& result) {
        result.accepted = true;
        result.trades = trades_;
        const SequenceNumber sequence = next_sequence_++;
        Quantity remaining = order.quantity;

        if (order.time_in_force == TimeInForce::FillOrKill && crossing_quantity(order) < order.quantity) {
            result.expired = true;
            return;
        }

        while (remaining > 0U) {
            const auto best = best_opposing_order(order);
            if (best == orders_.end()) {
                break;
            }

            const Quantity traded = std::min(remaining, best->remaining_quantity);
            Trade trade;
            trade.trade_id = next_trade_id_++;
            trade.buy_order_id = order.side == Side::Buy ? order.order_id : best->order_id;
            trade.sell_order_id = order.side == Side::Sell ? order.order_id : best->order_id;
            trade.price = best->price;
            trade.quantity = traded;
            trade.aggressor_side = order.side;
            trades_.push_back(trade);

            remaining -= traded;
            best->remaining_quantity -= traded;
            if (best->remaining_quantity == 0U) {
                orders_.erase(best);
            }
        }

        result.trades = trades_;
        result.remaining_quantity = remaining;
        result.filled_quantity = order.quantity - remaining;
        if (remaining == 0U) {
            return;
        }
        if (order.type == OrderType::Market || !can_rest(order.time_in_force)) {
            result.expired = true;
            return;
        }
        orders_.push_back(
            RestingOrderView{order.order_id, order.side, order.price, remaining, sequence, order.time_in_force});
        result.rested = true;
    }

    Orders::iterator find(OrderId order_id) {
        return std::ranges::find_if(orders_, [&](const auto& order) { return order.order_id == order_id; });
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
    std::vector<Trade> trades_;
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch::testing

#endif
