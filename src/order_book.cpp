#include "tradematch/order_book.hpp"

#include <algorithm>

namespace tradematch {

std::string OrderBook::validate(const OrderRequest& order) const {
    if (order.order_id == 0U) {
        return "Order ID must be greater than zero.";
    }

    if (order.quantity == 0U) {
        return "Order quantity must be greater than zero.";
    }

    if (order.price < 0) {
        return "Order price cannot be negative.";
    }

    if (order.type == OrderType::Limit && order.price <= 0) {
        return "Limit order price must be greater than zero.";
    }

    if (order_index_.find(order.order_id) != order_index_.end()) {
        return "Order ID is already active in the book.";
    }

    return {};
}

SubmitResult OrderBook::submit(const OrderRequest& order) {
    SubmitResult result;
    result.order_id = order.order_id;
    result.remaining_quantity = order.quantity;

    const auto validation_error = validate(order);
    if (!validation_error.empty()) {
        result.message = validation_error;
        return result;
    }

    WorkingOrder incoming{order, order.quantity, next_sequence_++};
    result.accepted = true;

    if (order.side == Side::Buy) {
        match_buy_order(incoming, result);
    } else {
        match_sell_order(incoming, result);
    }

    result.remaining_quantity = incoming.remaining_quantity;
    result.filled_quantity = static_cast<Quantity>(order.quantity - incoming.remaining_quantity);

    if (incoming.remaining_quantity == 0U) {
        result.message = "Order fully filled.";
        return result;
    }

    if (order.type == OrderType::Market) {
        result.expired = true;
        if (result.filled_quantity > 0U) {
            result.message = "Market order consumed available liquidity and expired with an unfilled remainder.";
        } else {
            result.message = "Market order expired because no opposing liquidity was available.";
        }
        return result;
    }

    rest_order(incoming);
    result.rested = true;
    if (result.filled_quantity > 0U) {
        result.message = "Order partially filled and remainder added to the book.";
    } else {
        result.message = "Order added to the book.";
    }

    return result;
}

CancelResult OrderBook::cancel(OrderId order_id) {
    CancelResult result;
    result.order_id = order_id;

    const auto locator_it = order_index_.find(order_id);
    if (locator_it == order_index_.end()) {
        result.message = "Order not found.";
        return result;
    }

    const OrderLocator locator = locator_it->second;
    OrderQueue* queue = nullptr;

    if (locator.side == Side::Buy) {
        auto level_it = bids_.find(locator.price);
        queue = &level_it->second;
    } else {
        auto level_it = asks_.find(locator.price);
        queue = &level_it->second;
    }

    result.cancelled = true;
    result.cancelled_quantity = locator.iterator->remaining_quantity;
    result.message = "Order cancelled.";
    queue->erase(locator.iterator);
    order_index_.erase(locator_it);

    if (locator.side == Side::Buy) {
        auto level_it = bids_.find(locator.price);
        if (level_it != bids_.end() && level_it->second.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(locator.price);
        if (level_it != asks_.end() && level_it->second.empty()) {
            asks_.erase(level_it);
        }
    }

    return result;
}

bool OrderBook::has_order(OrderId order_id) const {
    return order_index_.find(order_id) != order_index_.end();
}

std::optional<RestingOrderView> OrderBook::get_order(OrderId order_id) const {
    const auto locator_it = order_index_.find(order_id);
    if (locator_it == order_index_.end()) {
        return std::nullopt;
    }

    const auto& order = *locator_it->second.iterator;
    return RestingOrderView{
        order.order_id,
        order.side,
        order.price,
        order.remaining_quantity,
        order.sequence,
    };
}

std::vector<RestingOrderView> OrderBook::get_orders_at_level(Side side, Price price) const {
    std::vector<RestingOrderView> orders;

    if (side == Side::Buy) {
        const auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            return orders;
        }

        for (const auto& order : level_it->second) {
            orders.push_back(
                RestingOrderView{order.order_id, order.side, order.price, order.remaining_quantity, order.sequence});
        }
        return orders;
    }

    const auto level_it = asks_.find(price);
    if (level_it == asks_.end()) {
        return orders;
    }

    for (const auto& order : level_it->second) {
        orders.push_back(
            RestingOrderView{order.order_id, order.side, order.price, order.remaining_quantity, order.sequence});
    }

    return orders;
}

BookSnapshot OrderBook::snapshot() const {
    BookSnapshot book_snapshot;

    for (const auto& [price, queue] : bids_) {
        PriceLevelView level;
        level.price = price;
        level.order_count = queue.size();

        for (const auto& order : queue) {
            level.total_quantity += order.remaining_quantity;
            level.order_ids.push_back(order.order_id);
        }

        book_snapshot.bids.push_back(level);
    }

    for (const auto& [price, queue] : asks_) {
        PriceLevelView level;
        level.price = price;
        level.order_count = queue.size();

        for (const auto& order : queue) {
            level.total_quantity += order.remaining_quantity;
            level.order_ids.push_back(order.order_id);
        }

        book_snapshot.asks.push_back(level);
    }

    return book_snapshot;
}

void OrderBook::match_buy_order(WorkingOrder& incoming, SubmitResult& result) {
    while (incoming.remaining_quantity > 0U && !asks_.empty()) {
        auto best_ask_it = asks_.begin();
        if (incoming.request.type == OrderType::Limit && best_ask_it->first > incoming.request.price) {
            return;
        }

        auto& resting_queue = best_ask_it->second;
        while (incoming.remaining_quantity > 0U && !resting_queue.empty()) {
            auto resting_it = resting_queue.begin();
            auto& resting_order = *resting_it;
            const auto trade_quantity = static_cast<Quantity>(
                std::min(incoming.remaining_quantity, resting_order.remaining_quantity));

            append_trade(incoming, resting_order, trade_quantity, resting_order.price, result);

            incoming.remaining_quantity -= trade_quantity;
            resting_order.remaining_quantity -= trade_quantity;

            if (resting_order.remaining_quantity == 0U) {
                order_index_.erase(resting_order.order_id);
                resting_queue.erase(resting_it);
            }
        }

        if (resting_queue.empty()) {
            asks_.erase(best_ask_it);
        }
    }
}

void OrderBook::match_sell_order(WorkingOrder& incoming, SubmitResult& result) {
    while (incoming.remaining_quantity > 0U && !bids_.empty()) {
        auto best_bid_it = bids_.begin();
        if (incoming.request.type == OrderType::Limit && best_bid_it->first < incoming.request.price) {
            return;
        }

        auto& resting_queue = best_bid_it->second;
        while (incoming.remaining_quantity > 0U && !resting_queue.empty()) {
            auto resting_it = resting_queue.begin();
            auto& resting_order = *resting_it;
            const auto trade_quantity = static_cast<Quantity>(
                std::min(incoming.remaining_quantity, resting_order.remaining_quantity));

            append_trade(incoming, resting_order, trade_quantity, resting_order.price, result);

            incoming.remaining_quantity -= trade_quantity;
            resting_order.remaining_quantity -= trade_quantity;

            if (resting_order.remaining_quantity == 0U) {
                order_index_.erase(resting_order.order_id);
                resting_queue.erase(resting_it);
            }
        }

        if (resting_queue.empty()) {
            bids_.erase(best_bid_it);
        }
    }
}

void OrderBook::rest_order(const WorkingOrder& order) {
    RestingOrder resting_order{
        order.request.order_id,
        order.request.side,
        order.request.price,
        order.remaining_quantity,
        order.sequence,
    };

    if (resting_order.side == Side::Buy) {
        auto& queue = bids_[resting_order.price];
        queue.push_back(resting_order);
        order_index_.emplace(resting_order.order_id,
                             OrderLocator{resting_order.side, resting_order.price, std::prev(queue.end())});
        return;
    }

    auto& queue = asks_[resting_order.price];
    queue.push_back(resting_order);
    order_index_.emplace(resting_order.order_id,
                         OrderLocator{resting_order.side, resting_order.price, std::prev(queue.end())});
}

void OrderBook::append_trade(const WorkingOrder& incoming,
                             const RestingOrder& resting,
                             Quantity quantity,
                             Price price,
                             SubmitResult& result) {
    Trade trade;
    trade.trade_id = next_trade_id_++;
    trade.price = price;
    trade.quantity = quantity;
    trade.aggressor_side = incoming.request.side;

    if (incoming.request.side == Side::Buy) {
        trade.buy_order_id = incoming.request.order_id;
        trade.sell_order_id = resting.order_id;
    } else {
        trade.buy_order_id = resting.order_id;
        trade.sell_order_id = incoming.request.order_id;
    }

    result.trades.push_back(trade);
}

}  // namespace tradematch
