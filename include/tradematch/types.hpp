#ifndef TRADEMATCH_TYPES_HPP
#define TRADEMATCH_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tradematch {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using Price = std::int64_t;  // Integer price ticks, represented as cents in the demo and tests.
using Quantity = std::uint32_t;
using Volume = std::uint64_t;  // Aggregated quantity (level totals, traded volume) that can exceed one order's size.
using SequenceNumber = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell
};

enum class OrderType : std::uint8_t {
    Limit,
    Market
};

// Why a request was rejected. Every rejection leaves the book unchanged.
enum class RejectReason : std::uint8_t {
    None,
    InvalidOrderId,    // order ID 0 is reserved
    InvalidQuantity,   // quantity must be positive
    InvalidPrice,      // limit prices must be positive, market orders must use price 0
    DuplicateOrderId,  // the ID already belongs to a resting order
    UnknownOrder,      // cancel or replace of an ID that is not resting
    SideChange,        // replace may not change the side
    TypeChange         // replace must keep the limit order type
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

// Results are plain values that never own heap memory. `trades` is a view into a buffer owned by the order book and
// reused by its next submit or replace call, so copy trades out if they must outlive that call. `message` always
// refers to a static string.
struct SubmitResult {
    OrderId order_id{};
    bool accepted{false};
    bool rested{false};
    bool expired{false};
    RejectReason reject_reason{RejectReason::None};
    Quantity filled_quantity{};
    Quantity remaining_quantity{};
    std::span<const Trade> trades;
    std::string_view message;
};

struct CancelResult {
    OrderId order_id{};
    bool cancelled{false};
    RejectReason reject_reason{RejectReason::None};
    Quantity cancelled_quantity{};
    std::string_view message;
};

struct ReplaceResult {
    OrderId order_id{};
    bool replaced{false};
    RejectReason reject_reason{RejectReason::None};
    Quantity previous_remaining_quantity{};
    SubmitResult submit_result;
    std::string_view message;
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
    Volume total_quantity{};
    std::size_t order_count{};
    std::vector<OrderId> order_ids;
};

struct BookSnapshot {
    std::vector<PriceLevelView> bids;
    std::vector<PriceLevelView> asks;
};

const char* to_string(Side side) noexcept;
const char* to_string(OrderType type) noexcept;
const char* to_string(RejectReason reason) noexcept;
std::string format_price(Price price);

}  // namespace tradematch

#endif
