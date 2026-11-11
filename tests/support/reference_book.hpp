#ifndef TRADEMATCH_TESTS_REFERENCE_BOOK_HPP
#define TRADEMATCH_TESTS_REFERENCE_BOOK_HPP

#include "tradematch/types.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace tradematch::testing {

// Deliberately naive model of the matching rules. Every operation is a linear scan over one flat vector of
// resting orders: far too slow for production, but small enough to be obviously correct. That makes it a
// trustworthy oracle for differential testing of the optimized OrderBook.
class ReferenceBook {
public:
    SubmitResult submit(const OrderRequest& order) {
        SubmitResult result;
        result.order_id = order.order_id;
        result.remaining_quantity = order.quantity;

        if (!is_valid(order) || find(order.order_id) != orders_.end()) {
            return result;
        }

        result.accepted = true;
        const SequenceNumber sequence = next_sequence_++;
        Quantity remaining = order.quantity;

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
            result.trades.push_back(trade);

            remaining -= traded;
            best->remaining_quantity -= traded;
            if (best->remaining_quantity == 0U) {
                orders_.erase(best);
            }
        }

        result.remaining_quantity = remaining;
        result.filled_quantity = order.quantity - remaining;

        if (remaining == 0U) {
            return result;
        }

        if (order.type == OrderType::Market) {
            result.expired = true;
            return result;
        }

        orders_.push_back(RestingOrderView{order.order_id, order.side, order.price, remaining, sequence});
        result.rested = true;
        return result;
    }

    CancelResult cancel(OrderId order_id) {
        CancelResult result;
        result.order_id = order_id;

        const auto it = find(order_id);
        if (it == orders_.end()) {
            return result;
        }

        result.cancelled = true;
        result.cancelled_quantity = it->remaining_quantity;
        orders_.erase(it);
        return result;
    }

    ReplaceResult replace(const OrderRequest& replacement) {
        ReplaceResult result;
        result.order_id = replacement.order_id;

        const auto it = find(replacement.order_id);
        if (it == orders_.end() || it->side != replacement.side || replacement.type != OrderType::Limit ||
            !is_valid(replacement)) {
            return result;
        }

        result.previous_remaining_quantity = it->remaining_quantity;
        orders_.erase(it);
        result.submit_result = submit(replacement);
        result.replaced = result.submit_result.accepted;
        return result;
    }

    std::optional<RestingOrderView> get_order(OrderId order_id) const {
        const auto it = std::find_if(orders_.begin(), orders_.end(), [&](const auto& order) {
            return order.order_id == order_id;
        });
        if (it == orders_.end()) {
            return std::nullopt;
        }
        return *it;
    }

    BookSnapshot snapshot() const {
        auto sorted = orders_;
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
            if (left.side != right.side) {
                return left.side == Side::Buy;
            }
            if (left.price != right.price) {
                return left.side == Side::Buy ? left.price > right.price : left.price < right.price;
            }
            return left.sequence < right.sequence;
        });

        BookSnapshot snapshot;
        for (const auto& order : sorted) {
            auto& levels = order.side == Side::Buy ? snapshot.bids : snapshot.asks;
            if (levels.empty() || levels.back().price != order.price) {
                levels.push_back(PriceLevelView{order.price, 0U, 0U, {}});
            }
            auto& level = levels.back();
            level.total_quantity += order.remaining_quantity;
            ++level.order_count;
            level.order_ids.push_back(order.order_id);
        }
        return snapshot;
    }

    std::size_t order_count() const noexcept {
        return orders_.size();
    }

private:
    using Orders = std::vector<RestingOrderView>;

    static bool is_valid(const OrderRequest& order) {
        if (order.order_id == 0U || order.quantity == 0U || order.price < 0) {
            return false;
        }
        if (order.type == OrderType::Limit) {
            return order.price > 0;
        }
        return order.price == 0;
    }

    Orders::iterator find(OrderId order_id) {
        return std::find_if(orders_.begin(), orders_.end(), [&](const auto& order) {
            return order.order_id == order_id;
        });
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
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch::testing

#endif
