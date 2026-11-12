#include "tradematch/snapshot.hpp"

#include "tradematch/detail/codec.hpp"
#include "tradematch/detail/file_io.hpp"
#include "tradematch/journal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>

namespace tradematch {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'T', 'M', 'S', 'N', 'A', 'P', 'S', 'H'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kChecksumBytes = 4;

void encode_name(detail::ByteWriter& out, const std::string& name) {
    out.varint(name.size());
    out.bytes(std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
}

std::string decode_name(detail::ByteReader& in) {
    const auto name = in.bytes(in.count("symbol name length"));
    if (name.size() > kMaxSymbolNameBytes) {
        throw detail::DecodeError("symbol name is too long");
    }
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

}  // namespace

EngineSnapshot capture(const MatchingEngine& engine, std::uint64_t sequence) {
    EngineSnapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.symbols.reserve(engine.symbol_count());
    for (SymbolId symbol = 0; symbol < engine.symbol_count(); ++symbol) {
        snapshot.symbols.push_back(EngineSnapshot::Symbol{std::string{engine.symbol_name(symbol)},
                                                          engine.capacity(symbol),
                                                          engine.book(symbol).export_state()});
    }
    return snapshot;
}

void restore(MatchingEngine& engine, const EngineSnapshot& snapshot) {
    if (engine.symbol_count() != 0U) {
        throw std::invalid_argument("a snapshot can only be restored into an empty engine");
    }
    for (const EngineSnapshot::Symbol& symbol : snapshot.symbols) {
        if (engine.find_symbol(symbol.name)) {
            throw std::invalid_argument("the snapshot lists symbol " + symbol.name + " twice");
        }
        const SymbolId id = engine.add_symbol(symbol.name, symbol.capacity);
        engine.book(id).import_state(symbol.book);
    }
}

std::vector<std::uint8_t> encode_snapshot(const EngineSnapshot& snapshot) {
    std::vector<std::uint8_t> bytes;
    detail::ByteWriter out(bytes);
    out.bytes(kMagic);
    out.fixed32(kFormatVersion);
    out.varint(snapshot.sequence);
    out.varint(snapshot.symbols.size());
    for (const EngineSnapshot::Symbol& symbol : snapshot.symbols) {
        if (symbol.name.size() > kMaxSymbolNameBytes) {
            throw std::invalid_argument("symbol names are limited to " + std::to_string(kMaxSymbolNameBytes) +
                                        " bytes");
        }
        encode_name(out, symbol.name);
        out.varint(symbol.capacity.orders);
        out.varint(symbol.capacity.price_levels);
        out.varint(symbol.capacity.trades_per_call);
        out.varint(symbol.capacity.stop_orders);
        detail::encode_book_state(out, symbol.book);
    }
    out.fixed32(detail::crc32c(bytes));
    return bytes;
}

EngineSnapshot decode_snapshot(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() + 4 + kChecksumBytes || !std::ranges::equal(bytes.first(kMagic.size()), kMagic)) {
        throw SnapshotError("not a TradeMatch snapshot");
    }
    const auto body = bytes.first(bytes.size() - kChecksumBytes);
    if (detail::ByteReader(bytes.last(kChecksumBytes)).fixed32() != detail::crc32c(body)) {
        throw SnapshotError("the snapshot is corrupt: its checksum does not match");
    }

    try {
        detail::ByteReader in(body);
        in.bytes(kMagic.size());
        const std::uint32_t version = in.fixed32();
        if (version != kFormatVersion) {
            throw SnapshotError("unsupported snapshot format version " + std::to_string(version));
        }
        EngineSnapshot snapshot;
        snapshot.sequence = in.varint();
        snapshot.symbols.resize(in.count("symbol count"));
        for (EngineSnapshot::Symbol& symbol : snapshot.symbols) {
            symbol.name = decode_name(in);
            symbol.capacity.orders = in.varint_as<std::size_t>("order capacity");
            symbol.capacity.price_levels = in.varint_as<std::size_t>("price level capacity");
            symbol.capacity.trades_per_call = in.varint_as<std::size_t>("trade capacity");
            symbol.capacity.stop_orders = in.varint_as<std::size_t>("stop order capacity");
            symbol.book = detail::decode_book_state(in);
        }
        if (in.remaining() != 0U) {
            throw SnapshotError("the snapshot has unexpected trailing data");
        }
        return snapshot;
    } catch (const detail::DecodeError& error) {
        throw SnapshotError(std::string{"the snapshot is malformed: "} + error.what());
    }
}

void save_snapshot(const std::filesystem::path& path, const EngineSnapshot& snapshot) {
    const std::vector<std::uint8_t> bytes = encode_snapshot(snapshot);
    std::filesystem::path temporary = path;
    temporary += ".tmp";

    std::FILE* file = detail::open_file(temporary, "wb");
    if (file == nullptr) {
        throw SnapshotError("cannot create " + temporary.string() + ": " + detail::last_error());
    }
    std::string failure;
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() || std::fflush(file) != 0 ||
        !detail::sync_file(file)) {
        failure = detail::last_error();
    }
    if (std::fclose(file) != 0 && failure.empty()) {
        failure = detail::last_error();
    }
    std::error_code error;
    if (!failure.empty()) {
        std::filesystem::remove(temporary, error);
        throw SnapshotError("cannot write " + temporary.string() + ": " + failure);
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw SnapshotError("cannot replace " + path.string() + ": " + error.message());
    }
    detail::sync_directory(path.parent_path());
}

EngineSnapshot load_snapshot(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes;
    try {
        bytes = detail::read_file(path);
    } catch (const std::runtime_error& error) {
        throw SnapshotError(error.what());
    }
    return decode_snapshot(bytes);
}

std::uint64_t state_hash(const OrderBook& book) {
    std::vector<std::uint8_t> bytes;
    detail::ByteWriter out(bytes);
    detail::encode_book_state(out, book.export_state());
    return detail::fnv1a(bytes);
}

std::uint64_t state_hash(const MatchingEngine& engine) {
    std::vector<std::uint8_t> bytes;
    detail::ByteWriter out(bytes);
    out.varint(engine.symbol_count());
    for (SymbolId symbol = 0; symbol < engine.symbol_count(); ++symbol) {
        encode_name(out, std::string{engine.symbol_name(symbol)});
        detail::encode_book_state(out, engine.book(symbol).export_state());
    }
    return detail::fnv1a(bytes);
}

RecoveryResult recover(MatchingEngine& engine,
                       const std::filesystem::path& journal,
                       const std::optional<std::filesystem::path>& snapshot) {
    if (engine.symbol_count() != 0U) {
        throw std::invalid_argument("recovery needs an empty engine");
    }
    RecoveryResult result;
    if (snapshot) {
        const EngineSnapshot loaded = load_snapshot(*snapshot);
        restore(engine, loaded);
        result.snapshot_sequence = loaded.sequence;
    }

    JournalReader reader(journal);
    if (reader.first_sequence() > result.snapshot_sequence + 1) {
        throw JournalError("the journal starts at record " + std::to_string(reader.first_sequence()) +
                           ", but the state only covers records up to " + std::to_string(result.snapshot_sequence));
    }
    Command command;
    while (reader.next(command)) {
        if (reader.sequence() > result.snapshot_sequence) {
            apply(engine, command);
            ++result.replayed;
        }
    }
    if (reader.sequence() < result.snapshot_sequence) {
        throw JournalError("the journal ends at record " + std::to_string(reader.sequence()) +
                           ", before the snapshot at record " + std::to_string(result.snapshot_sequence));
    }
    result.last_sequence = reader.sequence();
    result.torn_bytes = reader.torn_bytes();
    return result;
}

}  // namespace tradematch
