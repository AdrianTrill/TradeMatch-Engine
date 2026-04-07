#ifndef TRADEMATCH_TYPES_HPP
#define TRADEMATCH_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tradematch {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using Price = std::int64_t;  // Integer price ticks, represented as cents in the demo and tests.
using Quantity = std::uint32_t;
using SequenceNumber = std::uint64_t;

enum class Side {
    Buy,
    Sell
};

enum class OrderType {
    Limit,
    Market
};

struct OrderRequest {
    OrderId order_id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Quantity quantity{};
    Price price{};
};

struct Trade {
    TradeId trade_id{};
    OrderId buy_order_id{};
    OrderId sell_order_id{};
    Price price{};
    Quantity quantity{};
    Side aggressor_side{Side::Buy};
};

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

struct RestingOrderView {
    OrderId order_id{};
    Side side{Side::Buy};
    Price price{};
    Quantity remaining_quantity{};
    SequenceNumber sequence{};
};

struct PriceLevelView {
    Price price{};
    Quantity total_quantity{};
    std::size_t order_count{};
    std::vector<OrderId> order_ids;
};

struct BookSnapshot {
    std::vector<PriceLevelView> bids;
    std::vector<PriceLevelView> asks;
};

const char* to_string(Side side) noexcept;
const char* to_string(OrderType type) noexcept;
std::string format_price(Price price);

}  // namespace tradematch

#endif
