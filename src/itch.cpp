#include "tradematch/itch.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace tradematch::itch {

namespace {

constexpr std::uint64_t kMaxTimestamp = (std::uint64_t{1} << 48U) - 1;
constexpr std::size_t kLengthPrefixBytes = 2;

// Reads big-endian fields from a message whose length has been checked.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    std::uint64_t unsigned_bytes(std::size_t count) noexcept {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < count; ++index) {
            value = (value << 8U) | bytes_[position_++];
        }
        return value;
    }

    std::uint16_t u16() noexcept {
        return static_cast<std::uint16_t>(unsigned_bytes(2));
    }

    std::uint32_t u32() noexcept {
        return static_cast<std::uint32_t>(unsigned_bytes(4));
    }

    std::uint64_t u64() noexcept {
        return unsigned_bytes(8);
    }

    char character() noexcept {
        return static_cast<char>(bytes_[position_++]);
    }

    template <std::size_t Size>
    std::array<char, Size> characters() noexcept {
        std::array<char, Size> text{};
        for (char& character_slot : text) {
            character_slot = character();
        }
        return text;
    }

    // The header that follows the type byte.
    Header header() noexcept {
        position_ = 1;
        Header header;
        header.stock_locate = u16();
        header.tracking_number = u16();
        header.timestamp = unsigned_bytes(6);
        return header;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{0};
};

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    void unsigned_bytes(std::uint64_t value, std::size_t count) {
        for (std::size_t index = count; index > 0; --index) {
            out_.push_back(static_cast<std::uint8_t>(value >> (8U * (index - 1))));
        }
    }

    void u16(std::uint16_t value) {
        unsigned_bytes(value, 2);
    }

    void u32(std::uint32_t value) {
        unsigned_bytes(value, 4);
    }

    void u64(std::uint64_t value) {
        unsigned_bytes(value, 8);
    }

    void character(char value) {
        out_.push_back(static_cast<std::uint8_t>(value));
    }

    template <std::size_t Size>
    void characters(const std::array<char, Size>& text) {
        for (const char value : text) {
            character(value);
        }
    }

    void start(char type, const Header& header) {
        if (header.timestamp > kMaxTimestamp) {
            throw std::invalid_argument("an ITCH timestamp must fit in 48 bits");
        }
        character(type);
        u16(header.stock_locate);
        u16(header.tracking_number);
        unsigned_bytes(header.timestamp, 6);
    }

private:
    std::vector<std::uint8_t>& out_;
};

StockDirectory read_stock_directory(Reader& reader, const Header& header) {
    StockDirectory message{header};
    message.stock = reader.characters<8>();
    message.market_category = reader.character();
    message.financial_status = reader.character();
    message.round_lot_size = reader.u32();
    message.round_lots_only = reader.character();
    message.issue_classification = reader.character();
    message.issue_sub_type = reader.characters<2>();
    message.authenticity = reader.character();
    message.short_sale_threshold = reader.character();
    message.ipo_flag = reader.character();
    message.luld_reference_price_tier = reader.character();
    message.etp_flag = reader.character();
    message.etp_leverage_factor = reader.u32();
    message.inverse_indicator = reader.character();
    return message;
}

AddOrder read_add_order(Reader& reader, const Header& header, bool attributed) {
    AddOrder message{header};
    message.order_reference = reader.u64();
    message.side = reader.character();
    message.shares = reader.u32();
    message.stock = reader.characters<8>();
    message.price = reader.u32();
    if (attributed) {
        message.attribution = reader.characters<4>();
    }
    return message;
}

}  // namespace

bool operator==(const OtherMessage& left, const OtherMessage& right) noexcept {
    return left.header == right.header && std::ranges::equal(left.bytes, right.bytes);
}

std::size_t message_length(char type) noexcept {
    switch (type) {
        case 'S':
        case 'W':
            return 12;
        case 'R':
            return 39;
        case 'H':
            return 25;
        case 'Y':
        case 'N':
            return 20;
        case 'L':
            return 26;
        case 'V':
        case 'J':
        case 'U':
            return 35;
        case 'K':
            return 28;
        case 'h':
            return 21;
        case 'A':
        case 'C':
            return 36;
        case 'F':
        case 'Q':
            return 40;
        case 'E':
            return 31;
        case 'X':
            return 23;
        case 'D':
        case 'B':
            return 19;
        case 'P':
            return 44;
        case 'I':
            return 50;
        case 'O':
            return 48;
        default:
            return 0;
    }
}

namespace {

struct TypeOf {
    char operator()(const SystemEvent& /*message*/) const noexcept {
        return 'S';
    }
    char operator()(const StockDirectory& /*message*/) const noexcept {
        return 'R';
    }
    char operator()(const StockTradingAction& /*message*/) const noexcept {
        return 'H';
    }
    char operator()(const AddOrder& order) const noexcept {
        return order.attribution ? 'F' : 'A';
    }
    char operator()(const OrderExecuted& /*message*/) const noexcept {
        return 'E';
    }
    char operator()(const OrderExecutedWithPrice& /*message*/) const noexcept {
        return 'C';
    }
    char operator()(const OrderCancel& /*message*/) const noexcept {
        return 'X';
    }
    char operator()(const OrderDelete& /*message*/) const noexcept {
        return 'D';
    }
    char operator()(const OrderReplace& /*message*/) const noexcept {
        return 'U';
    }
    char operator()(const NonCrossTrade& /*message*/) const noexcept {
        return 'P';
    }
    char operator()(const CrossTrade& /*message*/) const noexcept {
        return 'Q';
    }
    char operator()(const BrokenTrade& /*message*/) const noexcept {
        return 'B';
    }
    char operator()(const OtherMessage& other) const noexcept {
        return other.bytes.empty() ? '\0' : static_cast<char>(other.bytes.front());
    }
};

// Writes each kind of message field by field.
struct Encoder {
    Writer& writer;
    std::vector<std::uint8_t>& out;

    void operator()(const SystemEvent& event) const {
        writer.start('S', event.header);
        writer.character(event.event_code);
    }
    void operator()(const StockDirectory& directory) const {
        writer.start('R', directory.header);
        writer.characters(directory.stock);
        writer.character(directory.market_category);
        writer.character(directory.financial_status);
        writer.u32(directory.round_lot_size);
        writer.character(directory.round_lots_only);
        writer.character(directory.issue_classification);
        writer.characters(directory.issue_sub_type);
        writer.character(directory.authenticity);
        writer.character(directory.short_sale_threshold);
        writer.character(directory.ipo_flag);
        writer.character(directory.luld_reference_price_tier);
        writer.character(directory.etp_flag);
        writer.u32(directory.etp_leverage_factor);
        writer.character(directory.inverse_indicator);
    }
    void operator()(const StockTradingAction& action) const {
        writer.start('H', action.header);
        writer.characters(action.stock);
        writer.character(action.trading_state);
        writer.character(action.reserved);
        writer.characters(action.reason);
    }
    void operator()(const AddOrder& order) const {
        writer.start(order.attribution ? 'F' : 'A', order.header);
        writer.u64(order.order_reference);
        writer.character(order.side);
        writer.u32(order.shares);
        writer.characters(order.stock);
        writer.u32(order.price);
        if (order.attribution) {
            writer.characters(*order.attribution);
        }
    }
    void operator()(const OrderExecuted& executed) const {
        writer.start('E', executed.header);
        writer.u64(executed.order_reference);
        writer.u32(executed.executed_shares);
        writer.u64(executed.match_number);
    }
    void operator()(const OrderExecutedWithPrice& executed) const {
        writer.start('C', executed.header);
        writer.u64(executed.order_reference);
        writer.u32(executed.executed_shares);
        writer.u64(executed.match_number);
        writer.character(executed.printable);
        writer.u32(executed.execution_price);
    }
    void operator()(const OrderCancel& cancel) const {
        writer.start('X', cancel.header);
        writer.u64(cancel.order_reference);
        writer.u32(cancel.cancelled_shares);
    }
    void operator()(const OrderDelete& deleted) const {
        writer.start('D', deleted.header);
        writer.u64(deleted.order_reference);
    }
    void operator()(const OrderReplace& replace) const {
        writer.start('U', replace.header);
        writer.u64(replace.original_order_reference);
        writer.u64(replace.new_order_reference);
        writer.u32(replace.shares);
        writer.u32(replace.price);
    }
    void operator()(const NonCrossTrade& trade) const {
        writer.start('P', trade.header);
        writer.u64(trade.order_reference);
        writer.character(trade.side);
        writer.u32(trade.shares);
        writer.characters(trade.stock);
        writer.u32(trade.price);
        writer.u64(trade.match_number);
    }
    void operator()(const CrossTrade& cross) const {
        writer.start('Q', cross.header);
        writer.u64(cross.shares);
        writer.characters(cross.stock);
        writer.u32(cross.cross_price);
        writer.u64(cross.match_number);
        writer.character(cross.cross_type);
    }
    void operator()(const BrokenTrade& broken) const {
        writer.start('B', broken.header);
        writer.u64(broken.match_number);
    }
    void operator()(const OtherMessage& other) const {
        out.insert(out.end(), other.bytes.begin(), other.bytes.end());
    }
};

}  // namespace

char type_of(const Message& message) {
    return std::visit(TypeOf{}, message);
}

std::optional<Timestamp> timestamp_of(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < kHeaderBytes) {
        return std::nullopt;
    }
    Reader reader(bytes);
    return reader.header().timestamp;
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    const auto type = static_cast<char>(bytes.front());
    const std::size_t length = message_length(type);
    if (length == 0 || bytes.size() != length) {
        return std::nullopt;
    }

    Reader reader(bytes);
    const Header header = reader.header();
    switch (type) {
        case 'S':
            return SystemEvent{header, reader.character()};
        case 'R':
            return read_stock_directory(reader, header);
        case 'H': {
            StockTradingAction message{header};
            message.stock = reader.characters<8>();
            message.trading_state = reader.character();
            message.reserved = reader.character();
            message.reason = reader.characters<4>();
            return message;
        }
        case 'A':
        case 'F':
            return read_add_order(reader, header, type == 'F');
        case 'E': {
            OrderExecuted message{header};
            message.order_reference = reader.u64();
            message.executed_shares = reader.u32();
            message.match_number = reader.u64();
            return message;
        }
        case 'C': {
            OrderExecutedWithPrice message{header};
            message.order_reference = reader.u64();
            message.executed_shares = reader.u32();
            message.match_number = reader.u64();
            message.printable = reader.character();
            message.execution_price = reader.u32();
            return message;
        }
        case 'X': {
            OrderCancel message{header};
            message.order_reference = reader.u64();
            message.cancelled_shares = reader.u32();
            return message;
        }
        case 'D':
            return OrderDelete{header, reader.u64()};
        case 'U': {
            OrderReplace message{header};
            message.original_order_reference = reader.u64();
            message.new_order_reference = reader.u64();
            message.shares = reader.u32();
            message.price = reader.u32();
            return message;
        }
        case 'P': {
            NonCrossTrade message{header};
            message.order_reference = reader.u64();
            message.side = reader.character();
            message.shares = reader.u32();
            message.stock = reader.characters<8>();
            message.price = reader.u32();
            message.match_number = reader.u64();
            return message;
        }
        case 'Q': {
            CrossTrade message{header};
            message.shares = reader.u64();
            message.stock = reader.characters<8>();
            message.cross_price = reader.u32();
            message.match_number = reader.u64();
            message.cross_type = reader.character();
            return message;
        }
        case 'B':
            return BrokenTrade{header, reader.u64()};
        default:
            return OtherMessage{header, bytes};
    }
}

void encode(const Message& message, std::vector<std::uint8_t>& out) {
    Writer writer(out);
    std::visit(Encoder{writer, out}, message);
}

void encode_framed(const Message& message, std::vector<std::uint8_t>& out) {
    const std::size_t start = out.size();
    out.resize(start + kLengthPrefixBytes);
    encode(message, out);
    const std::size_t length = out.size() - start - kLengthPrefixBytes;
    out[start] = static_cast<std::uint8_t>(length >> 8U);
    out[start + 1] = static_cast<std::uint8_t>(length);
}

const Header& header_of(const Message& message) {
    return std::visit([](const auto& alternative) -> const Header& { return alternative.header; }, message);
}

std::string_view symbol_of(const Stock& stock) noexcept {
    const std::string_view text{stock.data(), stock.size()};
    const auto last = text.find_last_not_of(' ');
    return last == std::string_view::npos ? std::string_view{} : std::string_view{stock.data(), last + 1};
}

Stock make_stock(std::string_view symbol) {
    Stock stock{};
    if (symbol.size() > stock.size()) {
        throw std::invalid_argument("an ITCH stock symbol has at most 8 characters: " + std::string{symbol});
    }
    stock.fill(' ');
    std::ranges::copy(symbol, stock.begin());
    return stock;
}

std::string format_price(std::uint32_t price) {
    std::string fraction = std::to_string(price % kPriceScale);
    fraction.insert(0, 4 - fraction.size(), '0');
    return std::to_string(price / kPriceScale) + '.' + fraction;
}

FileReader::FileReader(std::FILE* file, std::size_t block_bytes)
    : file_(file), buffer_(std::max(block_bytes, kLengthPrefixBytes + 0xFFFFU)) {}

std::optional<std::span<const std::uint8_t>> FileReader::next() {
    if (!refill(kLengthPrefixBytes)) {
        return std::nullopt;
    }
    const std::size_t length = (std::size_t{buffer_[begin_]} << 8U) | buffer_[begin_ + 1];
    refill(kLengthPrefixBytes + length);  // with the length prefix already buffered, a short file throws here
    const std::span<const std::uint8_t> message{buffer_.data() + begin_ + kLengthPrefixBytes, length};
    begin_ += kLengthPrefixBytes + length;
    return message;
}

// Makes at least `needed` unread bytes available, moving what is left to the front of the buffer and reading more.
// Returns false at a clean end of the file, and throws if the file ends partway through.
bool FileReader::refill(std::size_t needed) {
    if (end_ - begin_ >= needed) {
        return true;
    }
    std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(begin_),
              buffer_.begin() + static_cast<std::ptrdiff_t>(end_),
              buffer_.begin());
    end_ -= begin_;
    begin_ = 0;
    while (end_ < needed) {
        const std::size_t read = std::fread(buffer_.data() + end_, 1, buffer_.size() - end_, file_);
        if (read == 0) {
            if (std::ferror(file_) != 0) {
                throw std::runtime_error("the ITCH file can't be read");
            }
            if (end_ == 0) {
                return false;
            }
            throw std::runtime_error("the ITCH file ends partway through a message");
        }
        end_ += read;
        bytes_read_ += read;
    }
    return true;
}

}  // namespace tradematch::itch
