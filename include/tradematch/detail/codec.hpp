#ifndef TRADEMATCH_DETAIL_CODEC_HPP
#define TRADEMATCH_DETAIL_CODEC_HPP

#include "tradematch/book_state.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Binary encoding shared by the journal and snapshots. Integers are LEB128 varints, with signed values zigzag-encoded
// first, so the small numbers that dominate order flow take one or two bytes. Fixed-width fields are little-endian.
// The output is byte-for-byte identical on every platform.
namespace tradematch::detail {

class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Zigzag maps 0, -1, 1, -2, ... to 0, 1, 2, 3, ..., so small negative numbers stay short too.
constexpr std::uint64_t zigzag(std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1U) ^ (value < 0 ? ~std::uint64_t{0} : std::uint64_t{0});
}

constexpr std::int64_t unzigzag(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>((value >> 1U) ^ (std::uint64_t{0} - (value & 1U)));
}

inline constexpr std::size_t kMaxVarintBytes = 10;
inline constexpr std::size_t kMaxVarint32Bytes = 5;

// Appends to a growable byte vector. Used where convenience matters more than speed: headers and snapshots.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    void u8(std::uint8_t value) {
        out_.push_back(value);
    }

    void fixed32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            out_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void fixed64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            out_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void varint(std::uint64_t value) {
        while (value >= 0x80U) {
            out_.push_back(static_cast<std::uint8_t>(value | 0x80U));
            value >>= 7U;
        }
        out_.push_back(static_cast<std::uint8_t>(value));
    }

    void svarint(std::int64_t value) {
        varint(zigzag(value));
    }

    void bytes(std::span<const std::uint8_t> data) {
        out_.insert(out_.end(), data.begin(), data.end());
    }

private:
    std::vector<std::uint8_t>& out_;
};

// Writes through a raw pointer into memory the caller has sized for the worst case, with no capacity checks: the
// journal's hot path.
class RawWriter {
public:
    explicit RawWriter(std::uint8_t* out) noexcept : out_(out) {}

    void u8(std::uint8_t value) noexcept {
        *out_++ = value;
    }

    void varint(std::uint64_t value) noexcept {
        while (value >= 0x80U) {
            *out_++ = static_cast<std::uint8_t>(value | 0x80U);
            value >>= 7U;
        }
        *out_++ = static_cast<std::uint8_t>(value);
    }

    void svarint(std::int64_t value) noexcept {
        varint(zigzag(value));
    }

    void bytes(std::span<const std::uint8_t> data) noexcept {
        if (!data.empty()) {
            std::memcpy(out_, data.data(), data.size());
            out_ += data.size();
        }
    }

    std::uint8_t* position() const noexcept {
        return out_;
    }

private:
    std::uint8_t* out_;
};

// Every read checks bounds and throws DecodeError on malformed input, so untrusted bytes can never cause an
// out-of-range access or an oversized allocation.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    std::uint8_t u8() {
        if (position_ >= data_.size()) {
            throw DecodeError("unexpected end of data");
        }
        return data_[position_++];
    }

    std::uint32_t fixed32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            value |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return value;
    }

    std::uint64_t fixed64() {
        const std::uint64_t low = fixed32();
        return low | (static_cast<std::uint64_t>(fixed32()) << 32U);
    }

    std::uint64_t varint() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64U; shift += 7U) {
            const std::uint8_t byte = u8();
            if (shift == 63U && byte > 1U) {
                break;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U) {
                return value;
            }
        }
        throw DecodeError("varint overflows 64 bits");
    }

    std::int64_t svarint() {
        return unzigzag(varint());
    }

    std::span<const std::uint8_t> bytes(std::size_t count) {
        if (count > remaining()) {
            throw DecodeError("unexpected end of data");
        }
        const auto field = data_.subspan(position_, count);
        position_ += count;
        return field;
    }

    // A varint that must fit in T.
    template <typename T>
    T varint_as(const char* field) {
        const std::uint64_t value = varint();
        if (value > std::numeric_limits<T>::max()) {
            throw DecodeError(std::string{field} + " is out of range");
        }
        return static_cast<T>(value);
    }

    // An element count, which can't exceed the bytes left since every element takes at least one byte.
    std::size_t count(const char* field) {
        const std::uint64_t value = varint();
        if (value > remaining()) {
            throw DecodeError(std::string{field} + " exceeds the remaining data");
        }
        return static_cast<std::size_t>(value);
    }

    std::size_t position() const noexcept {
        return position_;
    }

    std::size_t remaining() const noexcept {
        return data_.size() - position_;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_{0};
};

// CRC-32C (Castagnoli), the checksum used by iSCSI, ext4, and many storage engines. crc32c() uses the CPU's CRC
// instructions where the build targets them (ARMv8, or x86 with SSE4.2) and the portable slice-by-8 version
// otherwise; both give the same result.
std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c_portable(std::span<const std::uint8_t> data) noexcept;

// 64-bit FNV-1a, used to fingerprint canonical state encodings.
std::uint64_t fnv1a(std::span<const std::uint8_t> data) noexcept;

// Orders take one flag byte (bit 0 side, bits 1-2 type, bits 3-4 time in force, bit 5 post-only, bits 6-7 self-trade
// prevention) and six varints. Every flag byte decodes to valid enum values.
inline constexpr std::size_t kMaxOrderBytes = 1 + (3 * kMaxVarintBytes) + (3 * kMaxVarint32Bytes);

namespace order_flags {
inline constexpr unsigned kTypeShift = 1;
inline constexpr unsigned kTimeInForceShift = 3;
inline constexpr unsigned kPostOnlyShift = 5;
inline constexpr unsigned kSelfTradeShift = 6;
inline constexpr unsigned kTwoBits = 3;
}  // namespace order_flags

// Throws std::invalid_argument for enum values outside their declared range, which could not be decoded faithfully.
inline std::uint8_t pack_order_flags(const OrderRequest& order) {
    using namespace order_flags;
    const auto side = static_cast<unsigned>(order.side);
    const auto type = static_cast<unsigned>(order.type);
    const auto time_in_force = static_cast<unsigned>(order.time_in_force);
    const auto self_trade = static_cast<unsigned>(order.self_trade_prevention);
    if (side > 1U || type > kTwoBits || time_in_force > kTwoBits || self_trade > kTwoBits) {
        throw std::invalid_argument("order has an enum value outside its range");
    }
    return static_cast<std::uint8_t>(side | (type << kTypeShift) | (time_in_force << kTimeInForceShift) |
                                     ((order.post_only ? 1U : 0U) << kPostOnlyShift) | (self_trade << kSelfTradeShift));
}

template <typename Writer>
void encode_order(Writer& out, const OrderRequest& order) {
    out.u8(pack_order_flags(order));
    out.varint(order.order_id);
    out.varint(order.quantity);
    out.svarint(order.price);
    out.svarint(order.stop_price);
    out.varint(order.display_quantity);
    out.varint(order.owner);
}

inline OrderRequest decode_order(ByteReader& in) {
    using namespace order_flags;
    const unsigned flags = in.u8();
    OrderRequest order;
    order.side = static_cast<Side>(flags & 1U);
    order.type = static_cast<OrderType>((flags >> kTypeShift) & kTwoBits);
    order.time_in_force = static_cast<TimeInForce>((flags >> kTimeInForceShift) & kTwoBits);
    order.post_only = ((flags >> kPostOnlyShift) & 1U) != 0U;
    order.self_trade_prevention = static_cast<SelfTradePrevention>((flags >> kSelfTradeShift) & kTwoBits);
    order.order_id = in.varint();
    order.quantity = in.varint_as<Quantity>("quantity");
    order.price = in.svarint();
    order.stop_price = in.svarint();
    order.display_quantity = in.varint_as<Quantity>("display quantity");
    order.owner = in.varint_as<OwnerId>("owner");
    return order;
}

// The canonical encoding of a book state: equal states always encode to the same bytes.
void encode_book_state(ByteWriter& out, const BookState& state);
BookState decode_book_state(ByteReader& in);

}  // namespace tradematch::detail

#endif
