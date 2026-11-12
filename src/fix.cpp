#include "tradematch/fix.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>

namespace tradematch::fix {

namespace {

// The whole text as a decimal integer, by from_chars rules: no whitespace, no plus sign, and a minus sign only for
// signed types.
template <typename Integer>
std::optional<Integer> parse_integer(std::string_view text) {
    Integer value{};
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [stop, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || stop != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> parse_digits(std::string_view text) {
    return parse_integer<std::uint64_t>(text);
}

unsigned checksum(std::string_view bytes) noexcept {
    unsigned sum = 0;
    for (const char byte : bytes) {
        sum += static_cast<unsigned char>(byte);
    }
    return sum % 256U;
}

ParseResult invalid(std::size_t consumed, std::string error) {
    return {ParseStatus::Invalid, consumed, std::move(error)};
}

}  // namespace

Message::Message(std::string_view type) {
    set(tag::MsgType, type);
}

std::string_view Message::type() const {
    return get(tag::MsgType).value_or(std::string_view{});
}

Message& Message::set(int field_tag, std::string_view value) {
    for (auto& [existing, text] : fields_) {
        if (existing == field_tag) {
            text = value;
            return *this;
        }
    }
    fields_.emplace_back(field_tag, std::string{value});
    return *this;
}

Message& Message::set_int(int field_tag, std::int64_t value) {
    return set(field_tag, std::to_string(value));
}

std::optional<std::string_view> Message::get(int field_tag) const {
    for (const auto& [existing, text] : fields_) {
        if (existing == field_tag) {
            return std::string_view{text};
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> Message::get_int(int field_tag) const {
    const auto text = get(field_tag);
    return text ? parse_integer<std::int64_t>(*text) : std::nullopt;
}

std::string encode(const Message& message) {
    if (message.fields().empty() || message.fields().front().first != tag::MsgType) {
        throw std::invalid_argument("a FIX message starts with its MsgType");
    }
    std::string body;
    for (const auto& [field_tag, value] : message.fields()) {
        if (field_tag <= 0 || value.empty() || value.find(kSoh) != std::string::npos) {
            throw std::invalid_argument("FIX fields need a positive tag and a value without SOH");
        }
        body += std::to_string(field_tag);
        body += '=';
        body += value;
        body += kSoh;
    }
    std::string encoded = "8=" + std::string{kBeginString} + kSoh + "9=" + std::to_string(body.size()) + kSoh;
    encoded += body;
    std::array<char, 8> trailer{};
    std::snprintf(trailer.data(), trailer.size(), "10=%03u", checksum(encoded));
    encoded += trailer.data();
    encoded += kSoh;
    return encoded;
}

ParseResult parse(std::string_view buffer, Message& message) {
    message.clear();
    const std::string begin = "8=" + std::string{kBeginString} + kSoh + "9=";
    if (buffer.size() < begin.size()) {
        return begin.starts_with(buffer) ? ParseResult{}
                                         : invalid(buffer.size(),
                                                   "the message doesn't start with "
                                                   "8=FIX.4.2");
    }
    if (!buffer.starts_with(begin)) {
        return invalid(buffer.size(), "the message doesn't start with 8=FIX.4.2");
    }

    const auto length_end = buffer.find(kSoh, begin.size());
    if (length_end == std::string_view::npos) {
        return buffer.size() - begin.size() > 6 ? invalid(buffer.size(), "BodyLength is too long") : ParseResult{};
    }
    const auto body_length = parse_digits(buffer.substr(begin.size(), length_end - begin.size()));
    if (!body_length || *body_length == 0U || *body_length > kMaxBodyLength) {
        return invalid(buffer.size(), "BodyLength is missing or out of range");
    }
    const std::size_t body_start = length_end + 1;
    const std::size_t trailer_start = body_start + *body_length;
    constexpr std::size_t kTrailerBytes = 7;  // 10=nnn<SOH>
    if (buffer.size() < trailer_start + kTrailerBytes) {
        return {};
    }
    const std::string_view trailer = buffer.substr(trailer_start, kTrailerBytes);
    if (!trailer.starts_with("10=") || trailer.back() != kSoh) {
        return invalid(buffer.size(), "the CheckSum isn't where BodyLength says the body ends");
    }
    const auto expected = parse_digits(trailer.substr(3, 3));
    if (!expected || *expected != checksum(buffer.substr(0, trailer_start))) {
        return invalid(buffer.size(), "the CheckSum doesn't match");
    }

    const std::string_view body = buffer.substr(body_start, *body_length);
    if (body.back() != kSoh) {
        return invalid(buffer.size(), "the body doesn't end with a field separator");
    }
    std::size_t position = 0;
    while (position < body.size()) {
        const auto end = body.find(kSoh, position);
        const std::string_view field = body.substr(position, end - position);
        const auto equals = field.find('=');
        const auto field_tag = equals == std::string_view::npos ? std::nullopt : parse_digits(field.substr(0, equals));
        if (!field_tag || *field_tag == 0U || *field_tag > 99'999U || equals + 1 == field.size()) {
            message.clear();
            return invalid(buffer.size(), "a field isn't tag=value");
        }
        message.set(static_cast<int>(*field_tag), field.substr(equals + 1));
        position = end + 1;
    }
    if (message.fields().front().first != tag::MsgType) {
        message.clear();
        return invalid(buffer.size(), "MsgType isn't the first field of the body");
    }
    return {ParseStatus::Complete, trailer_start + kTrailerBytes, {}};
}

std::string format_price(Price ticks) {
    return tradematch::format_price(ticks);
}

std::string utc_timestamp(std::chrono::system_clock::time_point now) {
    const auto millis = std::chrono::floor<std::chrono::milliseconds>(now);
    const auto days = std::chrono::floor<std::chrono::days>(millis);
    const std::chrono::year_month_day date{days};
    const std::chrono::hh_mm_ss time{millis - days};
    std::array<char, 32> text{};
    std::snprintf(text.data(),
                  text.size(),
                  "%04d%02u%02u-%02d:%02d:%02d.%03d",
                  static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()),
                  static_cast<int>(time.hours().count()),
                  static_cast<int>(time.minutes().count()),
                  static_cast<int>(time.seconds().count()),
                  static_cast<int>(time.subseconds().count()));
    return text.data();
}

std::string to_text(std::string_view encoded) {
    std::string text{encoded};
    for (char& character : text) {
        if (character == kSoh) {
            character = '|';
        }
    }
    return text;
}

}  // namespace tradematch::fix
