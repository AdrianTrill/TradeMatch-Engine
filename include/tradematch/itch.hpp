#ifndef TRADEMATCH_ITCH_HPP
#define TRADEMATCH_ITCH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// NASDAQ TotalView-ITCH 5.0, the binary order-level market data feed NASDAQ publishes. Every message starts with a
// one-byte type, a stock locate code, a tracking number, and a 6-byte timestamp in nanoseconds since midnight.
// Integers are big-endian, and prices are in units of 1/10,000 of a dollar. NASDAQ's historical files store each
// message behind a 2-byte big-endian length.
namespace tradematch::itch {

using Timestamp = std::uint64_t;    // nanoseconds since midnight
using Stock = std::array<char, 8>;  // left-justified, padded with spaces

inline constexpr std::uint32_t kPriceScale = 10'000;  // ITCH prices are dollars times 10,000
inline constexpr std::size_t kHeaderBytes = 11;

// The fields every message starts with, after its type. The type itself follows from the kind of message.
struct Header {
    std::uint16_t stock_locate{};  // 1 and up for a stock; 0 for messages about the whole market
    std::uint16_t tracking_number{};
    Timestamp timestamp{};

    bool operator==(const Header&) const = default;
};

// 'S': the market's daily schedule. Event codes: 'O' start of messages, 'S' start of system hours, 'Q' start of
// market hours, 'M' end of market hours, 'E' end of system hours, 'C' end of messages.
struct SystemEvent {
    Header header{};
    char event_code{};

    bool operator==(const SystemEvent&) const = default;
};

// 'R': a stock's attributes, sent for every stock before it trades.
struct StockDirectory {
    Header header{};
    Stock stock{};
    char market_category{};
    char financial_status{};
    std::uint32_t round_lot_size{};
    char round_lots_only{};
    char issue_classification{};
    std::array<char, 2> issue_sub_type{};
    char authenticity{};
    char short_sale_threshold{};
    char ipo_flag{};
    char luld_reference_price_tier{};
    char etp_flag{};
    std::uint32_t etp_leverage_factor{};
    char inverse_indicator{};

    bool operator==(const StockDirectory&) const = default;
};

// 'H': a stock's trading state: 'H' halted, 'P' paused, 'Q' quotation only, 'T' trading.
struct StockTradingAction {
    Header header{};
    Stock stock{};
    char trading_state{};
    char reserved{};
    std::array<char, 4> reason{};

    bool operator==(const StockTradingAction&) const = default;
};

// 'A', or 'F' when it carries the market participant's attribution: a displayed order joins the book.
struct AddOrder {
    Header header{};
    std::uint64_t order_reference{};
    char side{};  // 'B' buy, 'S' sell
    std::uint32_t shares{};
    Stock stock{};
    std::uint32_t price{};
    // Spelled out, so that initializing only the first members doesn't trip -Wmissing-field-initializers.
    std::optional<std::array<char, 4>> attribution{};  // NOLINT(readability-redundant-member-init)

    bool operator==(const AddOrder&) const = default;
};

// 'E': shares of a resting order executed at its own price.
struct OrderExecuted {
    Header header{};
    std::uint64_t order_reference{};
    std::uint32_t executed_shares{};
    std::uint64_t match_number{};

    bool operator==(const OrderExecuted&) const = default;
};

// 'C': shares of a resting order executed at another price, as in a cross. A non-printable execution ('N') counts
// toward volume through a cross trade message instead.
struct OrderExecutedWithPrice {
    Header header{};
    std::uint64_t order_reference{};
    std::uint32_t executed_shares{};
    std::uint64_t match_number{};
    char printable{};
    std::uint32_t execution_price{};

    bool operator==(const OrderExecutedWithPrice&) const = default;
};

// 'X': part of a resting order cancelled.
struct OrderCancel {
    Header header{};
    std::uint64_t order_reference{};
    std::uint32_t cancelled_shares{};

    bool operator==(const OrderCancel&) const = default;
};

// 'D': a resting order removed.
struct OrderDelete {
    Header header{};
    std::uint64_t order_reference{};

    bool operator==(const OrderDelete&) const = default;
};

// 'U': a resting order replaced by a new one, which takes a new reference and the back of the queue. The side and
// stock carry over from the original.
struct OrderReplace {
    Header header{};
    std::uint64_t original_order_reference{};
    std::uint64_t new_order_reference{};
    std::uint32_t shares{};
    std::uint32_t price{};

    bool operator==(const OrderReplace&) const = default;
};

// 'P': an execution against a non-displayed order, which therefore never appeared in the book.
struct NonCrossTrade {
    Header header{};
    std::uint64_t order_reference{};
    char side{};
    std::uint32_t shares{};
    Stock stock{};
    std::uint32_t price{};
    std::uint64_t match_number{};

    bool operator==(const NonCrossTrade&) const = default;
};

// 'Q': the volume of a cross. Cross types: 'O' opening, 'C' closing, 'H' IPO or halt, 'I' intraday.
struct CrossTrade {
    Header header{};
    std::uint64_t shares{};
    Stock stock{};
    std::uint32_t cross_price{};
    std::uint64_t match_number{};
    char cross_type{};

    bool operator==(const CrossTrade&) const = default;
};

// 'B': an execution was broken after the fact.
struct BrokenTrade {
    Header header{};
    std::uint64_t match_number{};

    bool operator==(const BrokenTrade&) const = default;
};

// A valid message of a type modeled only by its header: 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'I', 'N', or 'O'. The
// bytes are the whole message and point into the buffer it was decoded from.
struct OtherMessage {
    Header header{};
    std::span<const std::uint8_t> bytes;

    friend bool operator==(const OtherMessage& left, const OtherMessage& right) noexcept;
};

using Message = std::variant<SystemEvent,
                             StockDirectory,
                             StockTradingAction,
                             AddOrder,
                             OrderExecuted,
                             OrderExecutedWithPrice,
                             OrderCancel,
                             OrderDelete,
                             OrderReplace,
                             NonCrossTrade,
                             CrossTrade,
                             BrokenTrade,
                             OtherMessage>;

// The length of a message of this type, type byte included, or 0 if ITCH 5.0 has no such type.
std::size_t message_length(char type) noexcept;

// The message's type byte, such as 'A' for an AddOrder without attribution.
char type_of(const Message& message);

// The timestamp of an encoded message, read without decoding the rest, or nothing if it is too short to have one.
std::optional<Timestamp> timestamp_of(std::span<const std::uint8_t> bytes) noexcept;

// Decodes one message, without its length prefix. Returns nothing if the type is unknown or the length is wrong.
std::optional<Message> decode(std::span<const std::uint8_t> bytes);

// Appends the message's bytes, with the 2-byte length prefix of NASDAQ's files when framed. An OtherMessage is written
// as its original bytes. Throws std::invalid_argument for a timestamp that doesn't fit the field's 48 bits.
void encode(const Message& message, std::vector<std::uint8_t>& out);
void encode_framed(const Message& message, std::vector<std::uint8_t>& out);

const Header& header_of(const Message& message);

// The symbol without its padding, and the padded field for a symbol of at most 8 characters (longer ones throw
// std::invalid_argument).
std::string_view symbol_of(const Stock& stock) noexcept;
Stock make_stock(std::string_view symbol);

// A price in dollars with its four decimals, such as "123.4500".
std::string format_price(std::uint32_t price);

// Streams the messages of a file in NASDAQ's format, reading it in large blocks. The file isn't owned.
class FileReader {
public:
    explicit FileReader(std::FILE* file, std::size_t block_bytes = std::size_t{1} << 22U);

    // The next message without its length prefix, valid until the next call, or nothing at the end of the file.
    // Throws std::runtime_error if the file ends partway through a message or can't be read.
    std::optional<std::span<const std::uint8_t>> next();

    std::uint64_t bytes_read() const noexcept {
        return bytes_read_;
    }

private:
    bool refill(std::size_t needed);

    std::FILE* file_;
    std::vector<std::uint8_t> buffer_;
    std::size_t begin_{0};
    std::size_t end_{0};
    std::uint64_t bytes_read_{0};
};

}  // namespace tradematch::itch

#endif
