#include "tradematch/detail/codec.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#if defined(__ARM_FEATURE_CRC32) && !defined(__ARM_BIG_ENDIAN)
#include <arm_acle.h>
#define TRADEMATCH_CRC32C_ARM 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define TRADEMATCH_CRC32C_SSE42 1
#endif

namespace tradematch::detail {

namespace {

constexpr std::uint32_t kCastagnoli = 0x82F63B78U;  // the CRC-32C polynomial, bit-reversed
constexpr std::size_t kSlices = 8;

using CrcTables = std::array<std::array<std::uint32_t, 256>, kSlices>;

// Table k advances the CRC over a byte followed by k zero bytes, which lets slice-by-8 fold eight input bytes with
// eight independent lookups.
constexpr CrcTables make_crc_tables() {
    CrcTables tables{};
    for (std::uint32_t byte = 0; byte < 256U; ++byte) {
        std::uint32_t crc = byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? kCastagnoli : 0U);
        }
        tables[0][byte] = crc;
    }
    for (std::size_t slice = 1; slice < kSlices; ++slice) {
        for (std::size_t byte = 0; byte < 256U; ++byte) {
            const std::uint32_t previous = tables[slice - 1][byte];
            tables[slice][byte] = (previous >> 8U) ^ tables[0][previous & 0xFFU];
        }
    }
    return tables;
}

constexpr CrcTables kCrcTables = make_crc_tables();

std::uint32_t load32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

TimeInForce decode_time_in_force(std::uint8_t value) {
    if (value > static_cast<std::uint8_t>(TimeInForce::FillOrKill)) {
        throw DecodeError("unknown time in force");
    }
    return static_cast<TimeInForce>(value);
}

}  // namespace

std::uint32_t crc32c_portable(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    const std::uint8_t* bytes = data.data();
    std::size_t left = data.size();
    for (; left >= kSlices; bytes += kSlices, left -= kSlices) {
        const std::uint32_t low = crc ^ load32(bytes);
        const std::uint32_t high = load32(bytes + 4);
        crc = kCrcTables[7][low & 0xFFU] ^ kCrcTables[6][(low >> 8U) & 0xFFU] ^ kCrcTables[5][(low >> 16U) & 0xFFU] ^
              kCrcTables[4][low >> 24U] ^ kCrcTables[3][high & 0xFFU] ^ kCrcTables[2][(high >> 8U) & 0xFFU] ^
              kCrcTables[1][(high >> 16U) & 0xFFU] ^ kCrcTables[0][high >> 24U];
    }
    for (; left > 0; ++bytes, --left) {
        crc = (crc >> 8U) ^ kCrcTables[0][(crc ^ *bytes) & 0xFFU];
    }
    return ~crc;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
#if defined(TRADEMATCH_CRC32C_ARM) || defined(TRADEMATCH_CRC32C_SSE42)
    std::uint32_t crc = 0xFFFFFFFFU;
    const std::uint8_t* bytes = data.data();
    std::size_t left = data.size();
    for (; left >= 8U; bytes += 8, left -= 8U) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes, sizeof(word));  // little-endian, as the CRC instructions expect
#ifdef TRADEMATCH_CRC32C_ARM
        crc = __crc32cd(crc, word);
#else
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
#endif
    }
    for (; left > 0; ++bytes, --left) {
#ifdef TRADEMATCH_CRC32C_ARM
        crc = __crc32cb(crc, *bytes);
#else
        crc = _mm_crc32_u8(crc, *bytes);
#endif
    }
    return ~crc;
#else
    return crc32c_portable(data);
#endif
}

std::uint64_t fnv1a(std::span<const std::uint8_t> data) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325U;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 0x100000001B3U;
    }
    return hash;
}

void encode_book_state(ByteWriter& out, const BookState& state) {
    out.u8(static_cast<std::uint8_t>(state.phase));
    out.u8(state.last_trade_price ? 1U : 0U);
    if (state.last_trade_price) {
        out.svarint(*state.last_trade_price);
    }
    out.varint(state.next_sequence);
    out.varint(state.next_trade_id);
    for (const auto* levels : {&state.bids, &state.asks}) {
        out.varint(levels->size());
        for (const PriceLevelState& level : *levels) {
            out.svarint(level.price);
            out.varint(level.orders.size());
            for (const RestingOrderState& order : level.orders) {
                out.varint(order.order_id);
                out.varint(order.sequence);
                out.varint(order.visible_quantity);
                out.varint(order.hidden_quantity);
                out.varint(order.display_quantity);
                out.varint(order.owner);
                out.u8(static_cast<std::uint8_t>(order.time_in_force));
            }
        }
    }
    out.varint(state.stops.size());
    for (const StopOrderState& stop : state.stops) {
        encode_order(out, stop.request);
        out.varint(stop.sequence);
    }
    for (const auto* queue : {&state.auction_buys, &state.auction_sells}) {
        out.varint(queue->size());
        for (const AuctionMarketOrderState& queued : *queue) {
            out.varint(queued.order_id);
            out.varint(queued.quantity);
            out.u8(static_cast<std::uint8_t>(queued.time_in_force));
        }
    }
}

BookState decode_book_state(ByteReader& in) {
    BookState state;
    const std::uint8_t phase = in.u8();
    if (phase > static_cast<std::uint8_t>(TradingPhase::Auction)) {
        throw DecodeError("unknown trading phase");
    }
    state.phase = static_cast<TradingPhase>(phase);
    const std::uint8_t has_last_trade = in.u8();
    if (has_last_trade > 1U) {
        throw DecodeError("invalid last trade price flag");
    }
    if (has_last_trade == 1U) {
        state.last_trade_price = in.svarint();
    }
    state.next_sequence = in.varint();
    state.next_trade_id = in.varint();
    for (auto* levels : {&state.bids, &state.asks}) {
        levels->resize(in.count("level count"));
        for (PriceLevelState& level : *levels) {
            level.price = in.svarint();
            level.orders.resize(in.count("order count"));
            for (RestingOrderState& order : level.orders) {
                order.order_id = in.varint();
                order.sequence = in.varint();
                order.visible_quantity = in.varint_as<Quantity>("visible quantity");
                order.hidden_quantity = in.varint_as<Quantity>("hidden quantity");
                order.display_quantity = in.varint_as<Quantity>("display quantity");
                order.owner = in.varint_as<OwnerId>("owner");
                order.time_in_force = decode_time_in_force(in.u8());
            }
        }
    }
    state.stops.resize(in.count("stop count"));
    for (StopOrderState& stop : state.stops) {
        stop.request = decode_order(in);
        stop.sequence = in.varint();
    }
    for (auto* queue : {&state.auction_buys, &state.auction_sells}) {
        queue->resize(in.count("auction queue length"));
        for (AuctionMarketOrderState& queued : *queue) {
            queued.order_id = in.varint();
            queued.quantity = in.varint_as<Quantity>("auction order quantity");
            queued.time_in_force = decode_time_in_force(in.u8());
        }
    }
    return state;
}

}  // namespace tradematch::detail
