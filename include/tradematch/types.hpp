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
using OwnerId = std::uint32_t;   // trading account or firm, used for self-trade prevention; 0 means none
using Timestamp = std::int64_t;  // nanoseconds, from the caller's clock, real or simulated

enum class Side : std::uint8_t {
    Buy,
    Sell
};

enum class OrderType : std::uint8_t {
    Limit,
    Market,
    Stop,      // becomes a market order once the last trade price reaches stop_price
    StopLimit  // becomes a limit order at `price` once the last trade price reaches stop_price
};

enum class TimeInForce : std::uint8_t {
    GoodTillCancel,     // rests until filled or cancelled
    Day,                // rests until filled, cancelled, or the end-of-day expiry
    ImmediateOrCancel,  // trades what it can immediately; any remainder is cancelled
    FillOrKill          // trades its full quantity immediately or does nothing
};

// What happens when an incoming order would trade against a resting order with the same non-zero owner. The incoming
// order's setting decides, as on most venues.
enum class SelfTradePrevention : std::uint8_t {
    None,          // allow the self-trade
    CancelNewest,  // cancel the incoming order's remaining quantity
    CancelOldest,  // cancel the resting order and keep matching
    CancelBoth     // cancel the resting order and the incoming order's remaining quantity
};

// Why a request was rejected. Every rejection leaves the book unchanged.
enum class RejectReason : std::uint8_t {
    None,
    InvalidOrderId,          // order ID 0 is reserved
    InvalidQuantity,         // quantity must be positive
    InvalidPrice,            // limit prices must be positive, market orders must use price 0
    DuplicateOrderId,        // the ID already belongs to a resting order
    UnknownOrder,            // cancel or replace of an ID that is not resting
    SideChange,              // replace may not change the side
    TypeChange,              // replace must keep the limit order type
    InvalidTimeInForce,      // a replacement must be able to rest (GTC or DAY)
    InvalidPostOnly,         // post-only requires a limit order that can rest (GTC or DAY)
    PostOnlyWouldCross,      // a post-only order may not take liquidity
    InvalidStopPrice,        // stop orders need a positive stop price; other orders must leave it at 0
    InvalidDisplayQuantity,  // icebergs need a limit price, a resting time in force, and display <= quantity
    UnknownSymbol,           // a matching engine request named a symbol that does not exist
    NotAllowedInAuction      // IOC, FOK, and post-only orders cannot join a call auction
};

enum class TradingPhase : std::uint8_t {
    Continuous,  // orders match on arrival
    Auction      // orders accumulate without matching until uncross()
};

struct OrderRequest {
    OrderId order_id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Quantity quantity{};
    Price price{};
    TimeInForce time_in_force{TimeInForce::GoodTillCancel};
    bool post_only{false};        // rejected instead of trading if it would take liquidity
    Price stop_price{};           // trigger for Stop and StopLimit orders
    Quantity display_quantity{};  // iceberg tranche size; 0 displays the full quantity
    OwnerId owner{};              // self-trade prevention group; 0 opts out
    SelfTradePrevention self_trade_prevention{SelfTradePrevention::None};

    bool operator==(const OrderRequest&) const = default;
};

struct Trade {
    TradeId trade_id{};
    OrderId buy_order_id{};
    OrderId sell_order_id{};
    Price price{};
    Quantity quantity{};
    Side aggressor_side{Side::Buy};  // meaningless for auction trades, which have no aggressor
    bool auction{false};             // executed by an auction uncross at the single auction price

    bool operator==(const Trade&) const = default;
};

// Results are plain values that never own heap memory. `trades` is a view into a buffer owned by the order book and
// reused by its next submit or replace call, so copy trades out if they must outlive that call. `message` always
// refers to a static string.
struct SubmitResult {
    OrderId order_id{};
    bool accepted{false};
    bool rested{false};
    bool expired{false};
    bool pending_trigger{false};       // a stop order waiting in the stop book
    bool queued_for_auction{false};    // a market order waiting for the auction uncross
    bool self_trade_prevented{false};  // self-trade prevention cancelled the incoming order's remainder
    RejectReason reject_reason{RejectReason::None};
    Quantity filled_quantity{};
    Quantity remaining_quantity{};
    std::span<const Trade> trades;                // every trade the request caused, including triggered stops
    std::span<const OrderId> triggered_stops;     // stop orders the request activated, in activation order
    std::span<const OrderId> self_trade_cancels;  // resting orders cancelled by self-trade prevention
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
    bool priority_kept{false};  // amended in place: same price and no larger quantity
    RejectReason reject_reason{RejectReason::None};
    Quantity previous_remaining_quantity{};
    SubmitResult submit_result;
    std::string_view message;
};

// What an auction uncross would do at this moment. Published during the call phase so participants can see the
// indicative price and the imbalance they could still fill.
struct AuctionIndicative {
    Price price{};
    Volume matched_quantity{};
    Volume buy_surplus{};   // buy quantity at or above the price that would stay unfilled
    Volume sell_surplus{};  // sell quantity at or below the price that would stay unfilled

    bool operator==(const AuctionIndicative&) const = default;
};

struct AuctionResult {
    bool executed{false};  // false when nothing crossed; the book still returns to continuous trading
    Price price{};
    Volume matched_quantity{};
    std::span<const Trade> trades;                   // the auction trades, then any trades from triggered stops
    std::span<const OrderId> expired_market_orders;  // market orders the uncross could not fill
    std::span<const OrderId> triggered_stops;
};

struct RestingOrderView {
    OrderId order_id{};
    Side side{Side::Buy};
    Price price{};
    Quantity remaining_quantity{};  // total open quantity, including any hidden iceberg reserve
    SequenceNumber sequence{};
    TimeInForce time_in_force{TimeInForce::GoodTillCancel};
    Quantity visible_quantity{};  // the displayed part; equals remaining_quantity unless the order is an iceberg
    Quantity display_quantity{};
    OwnerId owner{};
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
const char* to_string(TimeInForce time_in_force) noexcept;
const char* to_string(SelfTradePrevention mode) noexcept;
const char* to_string(RejectReason reason) noexcept;
std::string format_price(Price price);

}  // namespace tradematch

#endif
