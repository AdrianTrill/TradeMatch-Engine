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
