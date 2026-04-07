#ifndef TRADEMATCH_ORDER_BOOK_HPP
#define TRADEMATCH_ORDER_BOOK_HPP

#include "tradematch/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tradematch {

class OrderBook {
public:
    SubmitResult submit(const OrderRequest& order);
    CancelResult cancel(OrderId order_id);

    bool has_order(OrderId order_id) const;
    std::optional<RestingOrderView> get_order(OrderId order_id) const;
    std::vector<RestingOrderView> get_orders_at_level(Side side, Price price) const;
    BookSnapshot snapshot() const;

    std::size_t order_count() const noexcept {
        return order_index_.size();
    }

private:
    struct RestingOrder {
        OrderId order_id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};
        SequenceNumber sequence{};
    };

    using OrderQueue = std::list<RestingOrder>;
    using OrderIterator = OrderQueue::iterator;
    using BidBook = std::map<Price, OrderQueue, std::greater<Price>>;
    using AskBook = std::map<Price, OrderQueue, std::less<Price>>;

    struct OrderLocator {
        Side side{Side::Buy};
        Price price{};
        OrderIterator iterator;
    };

    struct WorkingOrder {
        OrderRequest request;
        Quantity remaining_quantity{};
        SequenceNumber sequence{};
    };

    std::string validate(const OrderRequest& order) const;
    void match_buy_order(WorkingOrder& incoming, SubmitResult& result);
    void match_sell_order(WorkingOrder& incoming, SubmitResult& result);
    void rest_order(const WorkingOrder& order);
    void append_trade(const WorkingOrder& incoming,
                      const RestingOrder& resting,
                      Quantity quantity,
                      Price price,
                      SubmitResult& result);

    BidBook bids_;
    AskBook asks_;
    std::unordered_map<OrderId, OrderLocator> order_index_;
    SequenceNumber next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace tradematch

#endif
