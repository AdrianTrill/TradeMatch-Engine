#ifndef TRADEMATCH_SNAPSHOT_HPP
#define TRADEMATCH_SNAPSHOT_HPP

#include "tradematch/book_state.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace tradematch {

// The state of every book in a matching engine, as of a journal sequence number.
struct EngineSnapshot {
    struct Symbol {
        std::string name;
        BookCapacity capacity;
        BookState book;

        bool operator==(const Symbol&) const = default;
    };

    std::uint64_t sequence{0};    // the last journal record the state includes, or 0 for none
    std::vector<Symbol> symbols;  // in SymbolId order

    bool operator==(const EngineSnapshot&) const = default;
};

class SnapshotError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

EngineSnapshot capture(const MatchingEngine& engine, std::uint64_t sequence = 0);

// Rebuilds an empty engine from a snapshot. Throws std::invalid_argument if the engine already has symbols, a name
// repeats, or a book state is invalid; the engine should then be discarded.
void restore(MatchingEngine& engine, const EngineSnapshot& snapshot);

// File format: "TMSNAPSH", u32 format version, the varint-encoded snapshot, and a CRC-32C of everything before it.
std::vector<std::uint8_t> encode_snapshot(const EngineSnapshot& snapshot);
EngineSnapshot decode_snapshot(std::span<const std::uint8_t> bytes);

// Writes the snapshot to a temporary file, forces it to disk, and renames it over `path`, so a crash leaves either
// the previous snapshot or the new one, never a mix.
void save_snapshot(const std::filesystem::path& path, const EngineSnapshot& snapshot);
EngineSnapshot load_snapshot(const std::filesystem::path& path);

// A 64-bit fingerprint (FNV-1a) of the canonical encoding of the complete logical state. Equal fingerprints mean,
// short of a hash collision, that the books will behave identically from here on. Pool capacities don't count.
std::uint64_t state_hash(const OrderBook& book);
std::uint64_t state_hash(const MatchingEngine& engine);

struct RecoveryResult {
    std::uint64_t snapshot_sequence{0};  // 0 without a snapshot
    std::uint64_t replayed{0};           // journal records applied after the snapshot
    std::uint64_t last_sequence{0};      // the state now includes every record up to this one
    std::size_t torn_bytes{0};           // an incomplete final journal record that was ignored
};

// Rebuilds an empty engine after a restart: loads the snapshot, if any, then replays the journal records after it.
// Throws JournalError if the journal doesn't cover the gap after the snapshot or ends before it.
RecoveryResult recover(MatchingEngine& engine,
                       const std::filesystem::path& journal,
                       const std::optional<std::filesystem::path>& snapshot = std::nullopt);

}  // namespace tradematch

#endif
