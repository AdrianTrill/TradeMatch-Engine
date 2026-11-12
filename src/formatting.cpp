#include "tradematch/types.hpp"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

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

std::optional<Price> parse_price(std::string_view text) {
    const bool negative = text.starts_with('-');
    const std::string_view digits = negative ? text.substr(1) : text;
    const auto point = digits.find('.');
    const std::string_view whole = digits.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view{} : digits.substr(point + 1);
    const auto all_digits = [](std::string_view part) {
        return std::ranges::all_of(part, [](char character) { return character >= '0' && character <= '9'; });
    };
    if (whole.empty() || whole.size() > 16 || !all_digits(whole) || !all_digits(fraction) || fraction.size() > 2 ||
        (point != std::string_view::npos && fraction.empty())) {
        return std::nullopt;
    }
    Price ticks = 0;
    for (const char character : whole) {
        ticks = ticks * 10 + (character - '0');
    }
    ticks *= 100;
    if (!fraction.empty()) {
        const Price cents =
            fraction.size() == 1 ? (fraction[0] - '0') * 10 : (fraction[0] - '0') * 10 + (fraction[1] - '0');
        ticks += cents;
    }
    return negative ? -ticks : ticks;
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
