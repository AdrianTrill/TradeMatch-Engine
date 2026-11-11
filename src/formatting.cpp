#include "tradematch/types.hpp"

#include <iomanip>
#include <sstream>

namespace tradematch {

const char* to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }

    return "UNKNOWN";
}

const char* to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Limit:
            return "LIMIT";
        case OrderType::Market:
            return "MARKET";
        case OrderType::Stop:
            return "STOP";
        case OrderType::StopLimit:
            return "STOP_LIMIT";
    }

    return "UNKNOWN";
}

const char* to_string(TimeInForce time_in_force) noexcept {
    switch (time_in_force) {
        case TimeInForce::GoodTillCancel:
            return "GTC";
        case TimeInForce::Day:
            return "DAY";
        case TimeInForce::ImmediateOrCancel:
            return "IOC";
        case TimeInForce::FillOrKill:
            return "FOK";
    }

    return "UNKNOWN";
}

const char* to_string(SelfTradePrevention mode) noexcept {
    switch (mode) {
        case SelfTradePrevention::None:
            return "NONE";
        case SelfTradePrevention::CancelNewest:
            return "CANCEL_NEWEST";
        case SelfTradePrevention::CancelOldest:
            return "CANCEL_OLDEST";
        case SelfTradePrevention::CancelBoth:
            return "CANCEL_BOTH";
    }

    return "UNKNOWN";
}

const char* to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "NONE";
        case RejectReason::InvalidOrderId:
            return "INVALID_ORDER_ID";
        case RejectReason::InvalidQuantity:
            return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice:
            return "INVALID_PRICE";
        case RejectReason::DuplicateOrderId:
            return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrder:
            return "UNKNOWN_ORDER";
        case RejectReason::SideChange:
            return "SIDE_CHANGE";
        case RejectReason::TypeChange:
            return "TYPE_CHANGE";
        case RejectReason::InvalidTimeInForce:
            return "INVALID_TIME_IN_FORCE";
        case RejectReason::InvalidPostOnly:
            return "INVALID_POST_ONLY";
        case RejectReason::PostOnlyWouldCross:
            return "POST_ONLY_WOULD_CROSS";
        case RejectReason::InvalidStopPrice:
            return "INVALID_STOP_PRICE";
        case RejectReason::InvalidDisplayQuantity:
            return "INVALID_DISPLAY_QUANTITY";
        case RejectReason::UnknownSymbol:
            return "UNKNOWN_SYMBOL";
        case RejectReason::NotAllowedInAuction:
            return "NOT_ALLOWED_IN_AUCTION";
    }

    return "UNKNOWN";
}

std::string format_price(Price price) {
    const bool negative = price < 0;
    // Negate in unsigned arithmetic so the most negative price does not overflow.
    const auto magnitude = negative ? 0U - static_cast<std::uint64_t>(price) : static_cast<std::uint64_t>(price);
    const auto whole = magnitude / 100U;
    const auto fractional = magnitude % 100U;

    std::ostringstream stream;
    if (negative) {
        stream << '-';
    }

    stream << whole << '.' << std::setw(2) << std::setfill('0') << fractional;
    return stream.str();
}

}  // namespace tradematch
