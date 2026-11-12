// Coverage-guided fuzz target for everything that reads persisted bytes: the journal reader, command decoding and
// replay, and snapshot decoding and restore. Malformed input must be rejected with JournalError, SnapshotError, or
// std::invalid_argument; it must never crash or corrupt an engine, and whatever is accepted must round-trip.
#include "tradematch/detail/codec.hpp"
#include "tradematch/journal.hpp"
#include "tradematch/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t kMaxSymbols = 8;
constexpr std::size_t kMaxCapacity = 64;  // keeps hostile capacities from exhausting memory

[[noreturn]] void fail(const char* problem) {
    std::fprintf(stderr, "%s\n", problem);
    std::abort();
}

tradematch::BookCapacity clamp(const tradematch::BookCapacity& capacity) {
    return {std::min(capacity.orders, kMaxCapacity),
            std::min(capacity.price_levels, kMaxCapacity),
            std::min(capacity.trades_per_call, kMaxCapacity),
            std::min(capacity.stop_orders, kMaxCapacity)};
}

// Wraps the input in a valid journal header and frames it into records with correct checksums, so the fuzzer explores
// command decoding and replay instead of stopping at checksum failures. Each record takes a length byte and up to 64
// payload bytes.
std::vector<std::uint8_t> frame_journal(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> journal;
    tradematch::detail::ByteWriter out(journal);
    out.bytes(std::array<std::uint8_t, 8>{'T', 'M', 'J', 'O', 'U', 'R', 'N', 'L'});
    out.fixed32(1);
    out.fixed64(1);
    out.fixed32(tradematch::detail::crc32c(journal));
    std::size_t position = 0;
    while (position < data.size()) {
        const std::size_t wanted = data[position++] % 64U + 1U;
        const auto payload = data.subspan(position, std::min(wanted, data.size() - position));
        if (payload.empty()) {
            break;
        }
        position += payload.size();
        out.fixed32(static_cast<std::uint32_t>(payload.size()));
        out.fixed32(tradematch::detail::crc32c(payload));
        out.bytes(payload);
    }
    return journal;
}

// The replayed engine must survive a snapshot round trip unchanged.
void check_snapshot_round_trip(const tradematch::MatchingEngine& engine, std::uint64_t sequence) {
    const auto snapshot = tradematch::capture(engine, sequence);
    if (!(tradematch::decode_snapshot(tradematch::encode_snapshot(snapshot)) == snapshot)) {
        fail("a snapshot did not survive encoding");
    }
    tradematch::MatchingEngine restored;
    tradematch::restore(restored, snapshot);
    if (tradematch::state_hash(restored) != tradematch::state_hash(engine)) {
        fail("a restored engine differs from the original");
    }
}

void replay(std::vector<std::uint8_t> bytes) {
    tradematch::MatchingEngine engine;
    try {
        tradematch::JournalReader reader(std::move(bytes));
        tradematch::Command command;
        while (reader.next(command)) {
            if (command.type == tradematch::CommandType::AddSymbol) {
                if (engine.symbol_count() >= kMaxSymbols && !engine.find_symbol(command.symbol_name)) {
                    continue;
                }
                command.capacity = clamp(command.capacity);
            }
            tradematch::apply(engine, command);
        }
        check_snapshot_round_trip(engine, reader.sequence());
    } catch (const tradematch::JournalError&) {
        // Rejected input; the engine must still be consistent.
        check_snapshot_round_trip(engine, 0);
    }
}

void decode_and_restore(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> bytes;
    tradematch::detail::ByteWriter out(bytes);
    out.bytes(std::array<std::uint8_t, 8>{'T', 'M', 'S', 'N', 'A', 'P', 'S', 'H'});
    out.fixed32(1);
    out.bytes(data);
    out.fixed32(tradematch::detail::crc32c(bytes));

    tradematch::EngineSnapshot snapshot;
    try {
        snapshot = tradematch::decode_snapshot(bytes);
    } catch (const tradematch::SnapshotError&) {
        return;
    }
    if (snapshot.symbols.size() > kMaxSymbols) {
        return;
    }
    for (auto& symbol : snapshot.symbols) {
        symbol.capacity = clamp(symbol.capacity);
    }
    tradematch::MatchingEngine engine;
    try {
        tradematch::restore(engine, snapshot);
    } catch (const std::invalid_argument&) {
        return;  // a state no book can reach
    }
    if (!(tradematch::capture(engine, snapshot.sequence) == snapshot)) {
        fail("an accepted snapshot did not restore exactly");
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    switch (data[0] % 3U) {
        case 0:
            replay({input.begin(), input.end()});  // raw bytes: headers, framing, torn tails, corruption
            break;
        case 1:
            replay(frame_journal(input));  // valid framing around fuzzed commands
            break;
        default:
            decode_and_restore(input);  // fuzzed snapshot contents
            break;
    }
    return 0;
}
