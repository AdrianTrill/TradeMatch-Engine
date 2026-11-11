#ifndef TRADEMATCH_BENCH_BASELINE_MAP_ORDER_BOOK_HPP
#define TRADEMATCH_BENCH_BASELINE_MAP_ORDER_BOOK_HPP

#include "tradematch/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tradematch::baseline {

// Result types of the original engine: every call returns freshly allocated trade vectors and message strings.
struct SubmitResult {
    OrderId order_id{};
    bool accepted{false};
    bool rested{false};
    bool expired{false};
    Quantity filled_quantity{};
    Quantity remaining_quantity{};
    std::vector<Trade> trades;
    std::string message;
};

struct CancelResult {
    OrderId order_id{};
    bool cancelled{false};
    Quantity cancelled_quantity{};
    std::string message;
};

struct ReplaceResult {
    OrderId order_id{};
    bool replaced{false};
    Quantity previous_remaining_quantity{};
    SubmitResult submit_result;
    std::string message;
};

// The original engine (v1), kept unchanged as the performance baseline: std::map price levels, std::list FIFO
// queues, and std::unordered_map order lookup. Its semantics match tradematch::OrderBook for limit and market orders.
class MapOrderBook {
public:
    SubmitResult submit(const OrderRequest& order);
    CancelResult cancel(OrderId order_id);
    ReplaceResult replace(const OrderRequest& replacement);

    bool has_order(OrderId order_id) const;
    std::optional<RestingOrderView> get_order(OrderId order_id) const;
    std::vector<RestingOrderView> get_orders_at_level(Side side, Price price) const;
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
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
    using BidBook = std::map<Price, OrderQueue, std::greater<>>;
    using AskBook = std::map<Price, OrderQueue, std::less<>>;

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

    std::string validate_request(const OrderRequest& order, OrderId ignored_order_id = 0U) const;
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

}  // namespace tradematch::baseline

#endif
