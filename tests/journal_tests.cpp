#include "support/differential.hpp"
#include "tradematch/detail/codec.hpp"
#include "tradematch/detail/file_io.hpp"
#include "tradematch/journal.hpp"
#include "tradematch/simulator.hpp"
#include "tradematch/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using tradematch::BookState;
using tradematch::Command;
using tradematch::CommandType;
using tradematch::Durability;
using tradematch::JournalError;
using tradematch::JournalOptions;
using tradematch::JournalReader;
using tradematch::JournalWriter;
using tradematch::OrderRequest;
using tradematch::OrderType;
using tradematch::Side;
using tradematch::TimeInForce;
using Bytes = std::vector<std::uint8_t>;

constexpr tradematch::Timestamp kSecond = 1'000'000'000;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename Exception>
void expect_throws(const std::function<void()>& action, const std::string& message) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw TestFailure(message);
}

// A scratch directory, removed again when the test ends.
class TempDir {
public:
    TempDir() {
        std::random_device device;
        path_ = std::filesystem::temp_directory_path() /
                ("tradematch-journal-tests-" + std::to_string(device()) + '-' + std::to_string(device()));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path operator/(std::string_view name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

Bytes bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

Bytes prefix(const Bytes& bytes, std::size_t size) {
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)};
}

std::vector<Command> read_all(JournalReader& reader) {
    std::vector<Command> commands;
    Command command;
    while (reader.next(command)) {
        commands.push_back(command);
    }
    return commands;
}

// One command of every type, with extreme values and timestamps that jump backwards and forwards.
std::vector<Command> sample_commands() {
    std::vector<Command> commands;

    Command add;
    add.type = CommandType::AddSymbol;
    add.timestamp = 1'000;
    add.symbol_name = "ACME";
    add.capacity = tradematch::BookCapacity{10, 20, 30, 40};
    commands.push_back(add);

    Command submit;
    submit.type = CommandType::Submit;
    submit.timestamp = 999;
    submit.order = OrderRequest{std::numeric_limits<tradematch::OrderId>::max(),
                                Side::Sell,
                                OrderType::StopLimit,
                                std::numeric_limits<tradematch::Quantity>::max(),
                                std::numeric_limits<tradematch::Price>::max(),
                                TimeInForce::FillOrKill,
                                true,
                                std::numeric_limits<tradematch::Price>::min(),
                                7,
                                std::numeric_limits<tradematch::OwnerId>::max(),
                                tradematch::SelfTradePrevention::CancelBoth};
    commands.push_back(submit);

    Command cancel;
    cancel.type = CommandType::Cancel;
    cancel.timestamp = std::numeric_limits<tradematch::Timestamp>::min();
    cancel.symbol = std::numeric_limits<tradematch::SymbolId>::max();
    cancel.order_id = 42;
    commands.push_back(cancel);

    Command replace;
    replace.type = CommandType::Replace;
    replace.timestamp = std::numeric_limits<tradematch::Timestamp>::max();
    replace.symbol = 3;
    replace.order = OrderRequest{17, Side::Buy, OrderType::Limit, 250, 10'050, TimeInForce::Day};
    commands.push_back(replace);

    Command begin_auction;
    begin_auction.type = CommandType::BeginAuction;
    begin_auction.timestamp = -5;
    begin_auction.symbol = 3;
    commands.push_back(begin_auction);

    Command uncross = begin_auction;
    uncross.type = CommandType::Uncross;
    uncross.timestamp = 0;
    commands.push_back(uncross);

    Command expire;
    expire.type = CommandType::ExpireDayOrders;
    expire.timestamp = 86'400 * kSecond;
    commands.push_back(expire);
    return commands;
}

// Journal bytes with valid checksums around any header fields and payloads, to reach the checks behind the checksums.
Bytes forge_journal(std::uint32_t version, std::uint64_t first_sequence, const std::vector<Bytes>& payloads) {
    Bytes bytes;
    tradematch::detail::ByteWriter out(bytes);
    out.bytes(bytes_of("TMJOURNL"));
    out.fixed32(version);
    out.fixed64(first_sequence);
    out.fixed32(tradematch::detail::crc32c(bytes));
    for (const Bytes& payload : payloads) {
        out.fixed32(static_cast<std::uint32_t>(payload.size()));
        out.fixed32(tradematch::detail::crc32c(payload));
        out.bytes(payload);
    }
    return bytes;
}

Bytes forge_snapshot(std::uint32_t version, const Bytes& body) {
    Bytes bytes;
    tradematch::detail::ByteWriter out(bytes);
    out.bytes(bytes_of("TMSNAPSH"));
    out.fixed32(version);
    out.bytes(body);
    out.fixed32(tradematch::detail::crc32c(bytes));
    return bytes;
}

Bytes write_sample_journal(const std::filesystem::path& path) {
    JournalWriter writer(path, JournalOptions{Durability::Buffered, true, 1});
    for (const Command& command : sample_commands()) {
        writer.append(command);
    }
    writer.close();
    return tradematch::detail::read_file(path);
}

void test_crc32c_matches_reference_vectors() {
    Bytes ascending(32);
    Bytes descending(32);
    for (std::size_t index = 0; index < 32; ++index) {
        ascending[index] = static_cast<std::uint8_t>(index);
        descending[index] = static_cast<std::uint8_t>(31 - index);
    }
    // From RFC 3720 (iSCSI), appendix B.4, plus the standard check value.
    const std::vector<std::pair<Bytes, std::uint32_t>> vectors{
        {Bytes{}, 0x00000000U},
        {bytes_of("123456789"), 0xE3069283U},
        {Bytes(32, 0x00), 0x8A9136AAU},
        {Bytes(32, 0xFF), 0x62A8AB43U},
        {ascending, 0x46DD794EU},
        {descending, 0x113FDB5CU},
    };
    for (const auto& [data, crc] : vectors) {
        expect(tradematch::detail::crc32c(data) == crc, "crc32c should match the reference vector.");
        expect(tradematch::detail::crc32c_portable(data) == crc, "The portable crc32c should match too.");
    }

    // Every length and alignment around the 8-byte fast paths agrees between the two implementations.
    std::mt19937_64 rng(11);
    Bytes data;
    for (std::size_t length = 0; length < 300; ++length) {
        data.push_back(static_cast<std::uint8_t>(rng()));
        const std::span<const std::uint8_t> view(data);
        for (std::size_t offset = 0; offset < std::min<std::size_t>(8, data.size()); ++offset) {
            expect(tradematch::detail::crc32c(view.subspan(offset)) ==
                       tradematch::detail::crc32c_portable(view.subspan(offset)),
                   "Hardware and portable crc32c should agree on every length and alignment.");
        }
    }
}

void test_varints_round_trip() {
    using tradematch::detail::ByteReader;
    using tradematch::detail::ByteWriter;
    using tradematch::detail::DecodeError;
    constexpr auto kMaxUnsigned = std::numeric_limits<std::uint64_t>::max();
    const std::vector<std::uint64_t>
        unsigned_values{0, 1, 127, 128, 300, 16'383, 16'384, 0xFFFFFFFFU, std::uint64_t{1} << 63U, kMaxUnsigned};
    const std::vector<std::int64_t> signed_values{0,
                                                  -1,
                                                  1,
                                                  -64,
                                                  63,
                                                  64,
                                                  -65,
                                                  std::numeric_limits<std::int64_t>::min(),
                                                  std::numeric_limits<std::int64_t>::max()};

    Bytes bytes;
    ByteWriter out(bytes);
    for (const auto value : unsigned_values) {
        out.varint(value);
    }
    for (const auto value : signed_values) {
        out.svarint(value);
    }
    ByteReader in(bytes);
    for (const auto value : unsigned_values) {
        expect(in.varint() == value, "Unsigned varints should round-trip.");
    }
    for (const auto value : signed_values) {
        expect(in.svarint() == value, "Signed varints should round-trip.");
    }
    expect(in.remaining() == 0U, "Decoding should consume exactly what was encoded.");

    const auto encoded_size = [](const std::function<void(ByteWriter&)>& encode) {
        Bytes encoded;
        ByteWriter writer(encoded);
        encode(writer);
        return encoded.size();
    };
    expect(encoded_size([](ByteWriter& writer) { writer.varint(127); }) == 1U &&
               encoded_size([](ByteWriter& writer) { writer.varint(128); }) == 2U &&
               encoded_size([](ByteWriter& writer) { writer.varint(kMaxUnsigned); }) == 10U &&
               encoded_size([](ByteWriter& writer) { writer.svarint(-64); }) == 1U &&
               encoded_size([](ByteWriter& writer) { writer.svarint(64); }) == 2U,
           "Small magnitudes should encode in one byte, and no value in more than ten.");

    const std::vector<Bytes> malformed{
        Bytes{0x80},                                                              // truncated
        Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02},        // overflows 64 bits
        Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x00},  // eleven bytes
    };
    for (const Bytes& data : malformed) {
        expect_throws<DecodeError>(
            [&] {
                ByteReader reader(data);
                reader.varint();
            },
            "A malformed varint should be rejected.");
    }
    expect_throws<DecodeError>(
        [] {
            const Bytes data{0x05, 0x01};
            ByteReader reader(data);
            reader.count("elements");
        },
        "A count larger than the remaining data should be rejected before anything is allocated.");
}

void test_commands_round_trip_through_a_journal() {
    const TempDir dir;
    const auto commands = sample_commands();
    const Bytes bytes = write_sample_journal(dir / "sample.tmj");
    JournalReader reader(bytes);
    expect(reader.first_sequence() == 1U, "A new journal starts at sequence 1.");
    expect(read_all(reader) == commands, "Every command should read back exactly as written.");
    expect(reader.sequence() == commands.size() && reader.torn_bytes() == 0U, "Records are numbered from 1.");

    const auto path = dir / "offset.tmj";
    {
        JournalWriter writer(path, JournalOptions{Durability::Buffered, true, 1'000});
        expect(writer.append(commands[0]) == 1'000U && writer.append(commands[1]) == 1'001U,
               "A journal can start at any sequence number, for example right after a snapshot.");
    }
    JournalReader offset(path);
    read_all(offset);
    expect(offset.first_sequence() == 1'000U && offset.sequence() == 1'001U, "Sequence numbers continue from there.");
}

void test_reopening_a_journal_appends() {
    const TempDir dir;
    const auto path = dir / "append.tmj";
    const auto commands = sample_commands();
    {
        JournalWriter writer(path);
        writer.append(commands[0]);
        writer.append(commands[1]);
    }
    {
        JournalWriter writer(path);
        expect(writer.last_sequence() == 2U && writer.recovered_torn_bytes() == 0U, "Reopening finds two records.");
        for (std::size_t index = 2; index < commands.size(); ++index) {
            expect(writer.append(commands[index]) == index + 1, "Appends continue the sequence.");
        }
    }
    JournalReader reader(path);
    expect(read_all(reader) == commands, "Appended records, including their timestamps, should read back intact.");
}

void test_a_torn_final_record_is_cut_off() {
    const TempDir dir;
    const auto path = dir / "torn.tmj";
    const auto commands = sample_commands();
    const Bytes full = write_sample_journal(path);

    JournalReader probe(full);
    Command command;
    for (std::size_t index = 0; index + 1 < commands.size(); ++index) {
        probe.next(command);
    }
    const std::size_t last_record = probe.valid_bytes();

    // A crash can stop an append anywhere inside the final record.
    for (std::size_t cut = last_record + 1; cut < full.size(); ++cut) {
        JournalReader reader(prefix(full, cut));
        expect(read_all(reader).size() == commands.size() - 1 && reader.torn_bytes() == cut - last_record &&
                   reader.valid_bytes() == last_record,
               "Every partial final record should be ignored as torn, cut at byte " + std::to_string(cut) + ".");
    }

    // The record can also be complete in size but only partly persisted, or followed by zero-filled space.
    Bytes damaged = full;
    damaged.back() ^= 0x40U;
    JournalReader damaged_reader(damaged);
    expect(read_all(damaged_reader).size() == commands.size() - 1 &&
               damaged_reader.torn_bytes() == full.size() - last_record,
           "A final record that fails its checksum is torn.");
    Bytes zero_filled = full;
    zero_filled.resize(full.size() + 64, 0);
    JournalReader zero_reader(zero_filled);
    expect(read_all(zero_reader).size() == commands.size() && zero_reader.torn_bytes() == 64U,
           "Zero-filled space after the last record is torn, not corrupt.");

    // Reopening cuts off the torn record, and appending resumes with its sequence number.
    {
        const Bytes torn = prefix(full, full.size() - 3);
        std::FILE* file = tradematch::detail::open_file(path, "wb");
        expect(file != nullptr && std::fwrite(torn.data(), 1, torn.size(), file) == torn.size(),
               "Writing the torn journal should work.");
        std::fclose(file);
    }
    {
        JournalWriter writer(path);
        expect(writer.recovered_torn_bytes() == full.size() - 3 - last_record &&
                   writer.last_sequence() == commands.size() - 1,
               "Opening should report and remove the torn record.");
        expect(writer.append(commands.back()) == commands.size(), "The re-sent command reuses its sequence number.");
    }
    JournalReader reader(path);
    expect(read_all(reader) == commands && reader.torn_bytes() == 0U, "The repaired journal reads back in full.");
}

void test_corruption_is_an_error() {
    const TempDir dir;
    const Bytes full = write_sample_journal(dir / "corrupt.tmj");
    const auto read_everything = [](Bytes bytes) {
        return [bytes = std::move(bytes)] {
            JournalReader reader(bytes);
            read_all(reader);
        };
    };

    Bytes flipped = full;
    flipped[tradematch::kJournalHeaderBytes + tradematch::kJournalRecordHeaderBytes] ^= 0x01U;
    try {
        read_everything(flipped)();
        throw TestFailure("A damaged record before the end should be reported.");
    } catch (const JournalError& error) {
        const std::string message = error.what();
        expect(message.find("record 1 ") != std::string::npos && message.find("checksum") != std::string::npos,
               "The error should name the record and the problem: " + message);
    }

    Bytes bad_length = full;
    bad_length[tradematch::kJournalHeaderBytes + 1] = 0xFFU;  // payload length 65,280+, above the maximum
    expect_throws<JournalError>(read_everything(bad_length), "An impossible record length should be reported.");

    Bytes bad_header = full;
    bad_header[12] ^= 0x01U;
    expect_throws<JournalError>(read_everything(bad_header), "A damaged header should be reported.");
    Bytes bad_magic = full;
    bad_magic[0] = 'X';
    expect_throws<JournalError>(read_everything(bad_magic), "A file that isn't a journal should be rejected.");
    expect_throws<JournalError>(read_everything(prefix(full, 10)), "A truncated header should be rejected.");

    // A record with a valid checksum but contents no writer produces.
    expect_throws<JournalError>(read_everything(forge_journal(1, 1, {Bytes{0x09, 0x00}})),
                                "A record with an unknown command type should be reported.");

    // A writer refuses to append to a corrupt journal instead of silently dropping its records.
    const auto path = dir / "corrupt-append.tmj";
    {
        std::FILE* file = tradematch::detail::open_file(path, "wb");
        expect(file != nullptr && std::fwrite(flipped.data(), 1, flipped.size(), file) == flipped.size(),
               "Writing the corrupt journal should work.");
        std::fclose(file);
    }
    expect_throws<JournalError>([&] { JournalWriter writer(path); }, "Appending to a corrupt journal should fail.");
    expect(tradematch::detail::read_file(path) == flipped, "The corrupt journal should be left untouched.");
}

void test_durability_decides_when_records_reach_the_file() {
    const TempDir dir;
    const auto path = dir / "durability.tmj";
    const auto commands = sample_commands();
    const auto records_in_file = [&] {
        JournalReader reader(path);
        return read_all(reader).size();
    };
    for (const Durability durability :
         {Durability::Buffered, Durability::Background, Durability::Flush, Durability::Sync}) {
        JournalWriter writer(path, JournalOptions{durability, true, 1});
        writer.append(commands[0]);
        const bool buffered = durability == Durability::Buffered || durability == Durability::Background;
        expect(records_in_file() == (buffered ? 0U : 1U),
               "Only buffered journals should hold records back until a flush.");
        writer.flush();
        expect(records_in_file() == 1U, "A flush should write every buffered record.");
    }
}

void test_writer_rejects_bad_input_without_damaging_the_journal() {
    const TempDir dir;
    const auto path = dir / "input.tmj";
    tradematch::MatchingEngine engine;
    JournalWriter writer(path, JournalOptions{Durability::Buffered, true, 1});
    tradematch::JournaledEngine journaled(engine, &writer);
    journaled.add_symbol("ACME");

    expect_throws<std::invalid_argument>([&] { journaled.add_symbol(std::string(300, 'X')); },
                                         "Overlong symbol names should be rejected.");
    OrderRequest invalid{1, static_cast<Side>(9), OrderType::Limit, 10, 100};
    expect_throws<std::invalid_argument>([&] { journaled.submit(0, invalid); },
                                         "Orders with out-of-range enums can't be journaled faithfully.");
    expect_throws<std::out_of_range>([&] { journaled.begin_auction(7); },
                                     "Auction commands for unknown symbols should fail before being journaled.");
    journaled.cancel(0, 1);
    writer.close();
    expect_throws<JournalError>([&] { writer.append_cancel(0, 0, 1); }, "A closed journal should refuse appends.");

    JournalReader reader(path);
    const auto commands = read_all(reader);
    expect(
        commands.size() == 2U && commands[0].type == CommandType::AddSymbol && commands[1].type == CommandType::Cancel,
        "Rejected input should leave no trace in the journal.");
}

void test_error_paths_are_reported() {
    using tradematch::SnapshotError;
    const TempDir dir;
    const auto unreachable = dir / "missing-directory" / "file";
    expect_throws<JournalError>([&] { JournalReader reader(dir / "absent.tmj"); }, "A missing journal can't be read.");
    expect_throws<SnapshotError>([&] { tradematch::load_snapshot(dir / "absent.snapshot"); },
                                 "A missing snapshot can't be loaded.");
    expect_throws<JournalError>([&] { JournalWriter writer(unreachable); }, "A journal needs a writable directory.");
    expect_throws<SnapshotError>([&] { tradematch::save_snapshot(unreachable, tradematch::EngineSnapshot{}); },
                                 "A snapshot needs a writable directory.");
    expect_throws<std::invalid_argument>(
        [&] { JournalWriter writer(dir / "zero.tmj", JournalOptions{Durability::Buffered, true, 0}); },
        "Sequence numbers start at 1.");

    const auto read_forged = [](Bytes bytes) {
        return [bytes = std::move(bytes)] {
            JournalReader reader(bytes);
            read_all(reader);
        };
    };
    expect_throws<JournalError>(read_forged(forge_journal(2, 1, {})), "Unknown journal versions should be rejected.");
    expect_throws<JournalError>(read_forged(forge_journal(1, 0, {})), "A journal can't start at sequence 0.");

    Bytes long_name{static_cast<std::uint8_t>(CommandType::AddSymbol), 0x00};
    tradematch::detail::ByteWriter name_out(long_name);
    name_out.varint(300);
    long_name.resize(long_name.size() + 300, 'X');
    for (int field = 0; field < 4; ++field) {
        name_out.varint(1);
    }
    expect_throws<JournalError>(read_forged(forge_journal(1, 1, {long_name})),
                                "A symbol name longer than any writer allows should be rejected.");
    Bytes huge_quantity{static_cast<std::uint8_t>(CommandType::Submit), 0x00, 0x00, 0x00, 0x01};
    tradematch::detail::ByteWriter(huge_quantity).varint(std::uint64_t{1} << 40U);
    expect_throws<JournalError>(read_forged(forge_journal(1, 1, {huge_quantity})),
                                "A quantity that doesn't fit in 32 bits should be rejected.");

    expect(tradematch::decode_snapshot(forge_snapshot(1, Bytes{0, 0})) == tradematch::EngineSnapshot{},
           "An empty snapshot should decode.");
    expect_throws<SnapshotError>([] { tradematch::decode_snapshot(forge_snapshot(2, Bytes{0, 0})); },
                                 "Unknown snapshot versions should be rejected.");
    expect_throws<SnapshotError>([] { tradematch::decode_snapshot(forge_snapshot(1, Bytes{0, 0, 0})); },
                                 "Trailing data after the last symbol should be rejected.");
    // One symbol whose only resting order has time in force 7.
    const Bytes bad_time_in_force{0, 1, 1, 'A', 1, 1, 1, 1, 0, 0, 2, 1, 1, 200, 1, 1, 1, 1, 1, 0, 0, 0, 7, 0, 0, 0, 0};
    expect_throws<SnapshotError>([&] { tradematch::decode_snapshot(forge_snapshot(1, bad_time_in_force)); },
                                 "An unknown time in force should be rejected.");
    tradematch::EngineSnapshot long_named;
    long_named.symbols.push_back({std::string(300, 'X'), {}, {}});
    expect_throws<std::invalid_argument>([&] { tradematch::encode_snapshot(long_named); },
                                         "Overlong symbol names can't be saved.");

    tradematch::MatchingEngine engine;
    Command uncross;
    uncross.type = CommandType::Uncross;
    uncross.symbol = 4;
    expect_throws<JournalError>([&] { tradematch::apply(engine, uncross); },
                                "Replaying an auction for an unknown symbol should fail.");

    // Deliberately out of range, as a corrupted command might be.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto unknown_type = static_cast<CommandType>(0);
    expect(std::string{tradematch::to_string(CommandType::AddSymbol)} == "ADD_SYMBOL" &&
               std::string{tradematch::to_string(CommandType::BeginAuction)} == "BEGIN_AUCTION" &&
               std::string{tradematch::to_string(CommandType::ExpireDayOrders)} == "EXPIRE_DAY_ORDERS" &&
               std::string{tradematch::to_string(unknown_type)} == "UNKNOWN",
           "Command types should have names.");
    Command unknown;
    unknown.type = unknown_type;
    JournalWriter writer(dir / "unknown.tmj", JournalOptions{Durability::Buffered, true, 1});
    expect_throws<std::invalid_argument>([&] { writer.append(unknown); }, "Unknown command types can't be written.");
}

tradematch::SimulationConfig short_day() {
    tradematch::SimulationConfig config;
    config.seed = 21;
    config.symbols = {"ACME", "GLOBEX"};
    config.opening_auction = 2 * kSecond;
    config.continuous = 20 * kSecond;
    config.closing_auction = 2 * kSecond;
    config.closed = kSecond;
    config.measure_latency = false;
    return config;
}

void test_replaying_a_simulated_session_rebuilds_identical_books() {
    const TempDir dir;
    const auto path = dir / "session.tmj";
    std::uint64_t live_hash = 0;
    tradematch::EngineSnapshot live;
    {
        // Background writes: the session hands dozens of 64 KiB batches to the I/O thread.
        JournalWriter journal(path, JournalOptions{Durability::Background, true, 1});
        tradematch::MarketSimulator sim(short_day(), &journal);
        sim.run_for(60 * kSecond);  // two full days with four auctions, then part of a third
        expect(sim.stats().auctions >= 4U && sim.stats().requests > 10'000U, "The session should be busy.");
        live_hash = tradematch::state_hash(sim.engine());
        live = tradematch::capture(sim.engine(), journal.last_sequence());
        journal.close();
    }

    tradematch::MatchingEngine replayed;
    const auto result = tradematch::recover(replayed, path);
    expect(result.replayed == live.sequence && result.last_sequence == live.sequence,
           "Recovery should replay every journaled command.");
    expect(tradematch::state_hash(replayed) == live_hash, "The replayed engine should hash the same.");
    expect(tradematch::capture(replayed, result.last_sequence) == live,
           "Every book should match exactly: levels, queues, icebergs, stops, and counters.");
}

// Outside orders, as the browser playground sends them, are journaled with the agents' and replay to the same books.
void test_outside_orders_replay_with_the_session() {
    const TempDir dir;
    const auto path = dir / "session.tmj";
    std::uint64_t live_hash = 0;
    {
        JournalWriter journal(path, JournalOptions{Durability::Buffered, true, 1});
        tradematch::MarketSimulator sim(short_day(), &journal);
        std::size_t traded = 0;
        for (int second = 0; second < 40; ++second) {
            sim.run_for(kSecond);
            const tradematch::OrderBook& book = sim.engine().book(sim.symbol(1));
            const auto ask = book.best_ask();
            const auto bid = book.best_bid();
            if (sim.session() != tradematch::SessionPhase::Continuous || !ask || !bid) {
                continue;
            }
            tradematch::OrderRequest take{0,
                                          tradematch::Side::Buy,
                                          tradematch::OrderType::Limit,
                                          300,
                                          *ask,
                                          tradematch::TimeInForce::ImmediateOrCancel};
            take.owner = sim.participant_owner();
            traded += sim.submit(1, take).trades.size();
            tradematch::OrderRequest rest{0,
                                          tradematch::Side::Sell,
                                          tradematch::OrderType::Limit,
                                          200,
                                          *ask + 3,
                                          tradematch::TimeInForce::Day};
            rest.owner = sim.participant_owner();
            const auto rested = sim.submit(1, rest);
            if (second % 3 == 0) {
                sim.cancel(1, rested.order_id);
            }
        }
        expect(traded > 0U, "The outside orders should trade.");
        live_hash = tradematch::state_hash(sim.engine());
        journal.close();
    }
    tradematch::MatchingEngine replayed;
    tradematch::recover(replayed, path);
    expect(tradematch::state_hash(replayed) == live_hash, "The replayed engine should hash the same.");
}

// Applies a generated operation through the journaled engine and mirrors it into the generator's view.
void apply_generated(tradematch::JournaledEngine& journaled,
                     tradematch::testing::ReferenceBook& view,
                     const tradematch::testing::Operation& op) {
    using tradematch::testing::OpKind;
    switch (op.kind) {
        case OpKind::Submit:
            journaled.submit(0, op.order);
            view.submit(op.order);
            break;
        case OpKind::Cancel:
            journaled.cancel(0, op.order.order_id);
            view.cancel(op.order.order_id);
            break;
        case OpKind::Replace:
            journaled.replace(0, op.order);
            view.replace(op.order);
            break;
        case OpKind::ExpireDay:
            journaled.expire_day_orders();
            view.expire_day_orders();
            break;
        case OpKind::BeginAuction:
            journaled.begin_auction(0);
            view.begin_auction();
            break;
        case OpKind::Uncross:
            journaled.uncross(0);
            view.uncross();
            break;
        case OpKind::Snapshot:
            break;
    }
}

void test_recovery_from_a_snapshot_and_the_journal_tail() {
    const TempDir dir;
    const auto journal_path = dir / "flow.tmj";
    const auto snapshot_path = dir / "flow.snapshot";
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t live_hash = 0;
    {
        tradematch::MatchingEngine engine;
        JournalWriter journal(journal_path, JournalOptions{Durability::Buffered, true, 1});
        tradematch::JournaledEngine journaled(engine, &journal);
        journaled.add_symbol("ACME", tradematch::BookCapacity{1, 1, 1});
        tradematch::testing::OperationGenerator generator(7);
        tradematch::testing::ReferenceBook view;
        for (int step = 0; step < 4'000; ++step) {
            journaled.set_time(tradematch::Timestamp{step} * 1'000);
            apply_generated(journaled, view, generator.next(view));
            if (step == 2'500) {
                snapshot_sequence = journaled.save_snapshot(snapshot_path);
            }
        }
        live_hash = tradematch::state_hash(engine);
        last_sequence = journal.last_sequence();
    }
    expect(!std::filesystem::exists(snapshot_path.string() + ".tmp"), "No temporary snapshot should be left.");

    tradematch::MatchingEngine from_snapshot;
    const auto result = tradematch::recover(from_snapshot, journal_path, snapshot_path);
    expect(result.snapshot_sequence == snapshot_sequence && result.replayed == last_sequence - snapshot_sequence &&
               result.last_sequence == last_sequence,
           "Recovery from the snapshot should replay only the journal tail.");
    expect(tradematch::state_hash(from_snapshot) == live_hash, "Snapshot plus tail should rebuild the same state.");
    expect(from_snapshot.capacity(0) == tradematch::BookCapacity{1, 1, 1}, "Snapshots keep book capacities.");

    tradematch::MatchingEngine from_journal;
    expect(tradematch::recover(from_journal, journal_path).replayed == last_sequence &&
               tradematch::state_hash(from_journal) == live_hash,
           "The journal alone should rebuild the same state.");
    expect_throws<std::invalid_argument>([&] { tradematch::recover(from_journal, journal_path); },
                                         "Recovery should refuse an engine that already has state.");
}

void test_recovery_refuses_gaps_between_snapshot_and_journal() {
    const TempDir dir;
    const auto journal_path = dir / "late.tmj";
    const auto snapshot_path = dir / "state.snapshot";
    {
        JournalWriter journal(journal_path, JournalOptions{Durability::Buffered, true, 10});
        journal.append(sample_commands().front());  // record 10 adds a symbol
    }
    const auto recover_with = [&](std::optional<std::uint64_t> snapshot_sequence) {
        tradematch::MatchingEngine engine;
        if (!snapshot_sequence) {
            return tradematch::recover(engine, journal_path);
        }
        tradematch::save_snapshot(snapshot_path, tradematch::EngineSnapshot{*snapshot_sequence, {}});
        return tradematch::recover(engine, journal_path, snapshot_path);
    };
    expect_throws<JournalError>([&] { recover_with(std::nullopt); },
                                "A journal starting at record 10 needs a snapshot covering records 1 to 9.");
    expect_throws<JournalError>([&] { recover_with(5); }, "Records 6 to 9 are missing between snapshot and journal.");
    expect_throws<JournalError>([&] { recover_with(20); }, "A journal that ends before its snapshot lost records.");
    const auto result = recover_with(9);
    expect(result.replayed == 1U && result.last_sequence == 10U, "A snapshot at record 9 lines up with the journal.");
}

tradematch::EngineSnapshot busy_snapshot() {
    tradematch::MatchingEngine engine;
    tradematch::JournaledEngine journaled(engine);
    journaled.add_symbol("ACME");
    tradematch::testing::OperationGenerator generator(3);
    tradematch::testing::ReferenceBook view;
    for (int step = 0; step < 600; ++step) {
        apply_generated(journaled, view, generator.next(view));
    }
    return tradematch::capture(engine, 600);
}

void test_snapshot_files_detect_damage() {
    const TempDir dir;
    const auto snapshot = busy_snapshot();
    const Bytes bytes = tradematch::encode_snapshot(snapshot);
    expect(tradematch::decode_snapshot(bytes) == snapshot, "A snapshot should decode to what was encoded.");

    const auto path = dir / "busy.snapshot";
    tradematch::save_snapshot(path, snapshot);
    expect(tradematch::load_snapshot(path) == snapshot, "A saved snapshot should load back intact.");

    // A CRC-32C detects every single-byte error and every truncation.
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        Bytes damaged = bytes;
        damaged[index] ^= 0x10U;
        expect_throws<tradematch::SnapshotError>([&] { tradematch::decode_snapshot(damaged); },
                                                 "A damaged byte at " + std::to_string(index) + " should be caught.");
    }
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        expect_throws<tradematch::SnapshotError>(
            [&] { tradematch::decode_snapshot(prefix(bytes, size)); },
            "A snapshot truncated to " + std::to_string(size) + " bytes should be caught.");
    }

    tradematch::EngineSnapshot twice = snapshot;
    twice.symbols.push_back(snapshot.symbols.front());
    tradematch::MatchingEngine engine;
    expect_throws<std::invalid_argument>([&] { tradematch::restore(engine, twice); },
                                         "A snapshot naming a symbol twice should be rejected.");
    tradematch::MatchingEngine used;
    used.add_symbol("OTHER");
    expect_throws<std::invalid_argument>([&] { tradematch::restore(used, snapshot); },
                                         "Snapshots restore only into empty engines.");
}

void test_import_rejects_states_a_book_cannot_reach() {
    tradematch::OrderBook book;
    book.submit({1, Side::Buy, OrderType::Limit, 100, 10'000});
    book.submit({2, Side::Buy, OrderType::Limit, 50, 9'990});
    OrderRequest iceberg{3, Side::Sell, OrderType::Limit, 300, 10'010};
    iceberg.display_quantity = 100;
    book.submit(iceberg);
    book.submit({4, Side::Sell, OrderType::Limit, 70, 10'020, TimeInForce::Day});
    OrderRequest stop{5, Side::Buy, OrderType::Stop, 10, 0};
    stop.stop_price = 10'050;
    book.submit(stop);
    book.submit({6, Side::Sell, OrderType::Limit, 10, 10'000});  // trades, so the book has a last price
    const BookState valid = book.export_state();
    expect(valid.stops.size() == 1U && valid.last_trade_price == 10'000 && valid.asks[0].orders[0].hidden_quantity > 0U,
           "The sample state should cover stops, icebergs, and a last trade price.");

    tradematch::OrderBook copy;
    copy.import_state(valid);
    expect(copy.export_state() == valid, "A reachable state should import and export unchanged.");

    BookState auction = valid;
    auction.phase = tradematch::TradingPhase::Auction;
    auction.bids[0].price = 10'030;  // crosses the asks, which only a call auction allows
    auction.auction_buys.push_back({7, 25, TimeInForce::Day});
    copy.import_state(auction);
    expect(copy.export_state() == auction, "A crossed book with queued market orders is valid during an auction.");

    using Mutation = std::function<void(BookState&)>;
    const std::vector<std::pair<const char*, Mutation>> cases{
        {"unknown phase",
         [](BookState& state) {
             // Deliberately out of range, as a corrupted or hostile state might be.
             // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
             state.phase = static_cast<tradematch::TradingPhase>(7);
         }},
        {"zero next sequence", [](BookState& state) { state.next_sequence = 0; }},
        {"zero next trade ID", [](BookState& state) { state.next_trade_id = 0; }},
        {"non-positive last price", [](BookState& state) { state.last_trade_price = 0; }},
        {"empty level", [](BookState& state) { state.bids[0].orders.clear(); }},
        {"bids out of order", [](BookState& state) { std::swap(state.bids[0], state.bids[1]); }},
        {"duplicate level", [](BookState& state) { state.asks[1].price = state.asks[0].price; }},
        {"crossed continuous book", [](BookState& state) { state.bids[0].price = state.asks[0].price; }},
        {"order ID 0", [](BookState& state) { state.bids[0].orders[0].order_id = 0; }},
        {"duplicate order ID",
         [](BookState& state) { state.asks[1].orders[0].order_id = state.bids[0].orders[0].order_id; }},
        {"stop reusing a resting ID", [](BookState& state) { state.stops[0].request.order_id = 2; }},
        {"sequence from the future", [](BookState& state) { state.bids[0].orders[0].sequence = state.next_sequence; }},
        {"duplicate sequence",
         [](BookState& state) { state.bids[1].orders[0].sequence = state.bids[0].orders[0].sequence; }},
        {"queue out of time priority",
         [](BookState& state) {
             auto later = state.bids[0].orders[0];
             later.order_id = 99;
             later.sequence = state.next_sequence - 1;  // free: order 6 traded in full
             state.bids[0].orders.insert(state.bids[0].orders.begin(), later);
         }},
        {"nothing visible", [](BookState& state) { state.bids[0].orders[0].visible_quantity = 0; }},
        {"IOC resting",
         [](BookState& state) { state.bids[0].orders[0].time_in_force = TimeInForce::ImmediateOrCancel; }},
        {"hidden reserve without iceberg", [](BookState& state) { state.bids[0].orders[0].hidden_quantity = 5; }},
        {"tranche above display size",
         [](BookState& state) {
             auto& order = state.asks[0].orders[0];
             order.visible_quantity = order.display_quantity + 1;
         }},
        {"open quantity overflow",
         [](BookState& state) {
             auto& order = state.asks[0].orders[0];
             order.display_quantity = std::numeric_limits<tradematch::Quantity>::max();
             order.visible_quantity = order.display_quantity;
             order.hidden_quantity = 1;
         }},
        {"stop that is a limit order", [](BookState& state) { state.stops[0].request.type = OrderType::Limit; }},
        {"stop with a limit price", [](BookState& state) { state.stops[0].request.price = 10'100; }},
        {"stop already triggered", [](BookState& state) { state.stops[0].request.stop_price = 9'000; }},
        {"stops out of arrival order",
         [](BookState& state) {
             auto later = state.stops[0];
             later.request.order_id = 98;
             later.sequence = state.next_sequence - 1;
             state.stops.insert(state.stops.begin(), later);
         }},
        {"market order queued outside an auction",
         [](BookState& state) { state.auction_buys.push_back({97, 10, TimeInForce::GoodTillCancel}); }},
        {"IOC queued for an auction",
         [](BookState& state) {
             state.phase = tradematch::TradingPhase::Auction;
             state.auction_sells.push_back({97, 10, TimeInForce::ImmediateOrCancel});
         }},
    };
    for (const auto& [name, mutate] : cases) {
        BookState state = valid;
        mutate(state);
        tradematch::OrderBook target;
        target.submit({50, Side::Sell, OrderType::Limit, 5, 20'000});
        const BookState before = target.export_state();
        expect_throws<std::invalid_argument>([&] { target.import_state(state); },
                                             std::string{"import_state should reject: "} + name);
        expect(target.export_state() == before, std::string{"A rejected import should change nothing: "} + name);
    }
}

void test_state_hash_reflects_every_detail() {
    tradematch::OrderBook first;
    tradematch::OrderBook second(tradematch::BookCapacity{1, 1, 1});
    for (auto* book : {&first, &second}) {
        book->submit({1, Side::Buy, OrderType::Limit, 10, 10'000});
        book->submit({2, Side::Buy, OrderType::Limit, 20, 10'000});
    }
    expect(tradematch::state_hash(first) == tradematch::state_hash(second),
           "Equal states hash the same, whatever the pool capacity.");

    // Same orders and totals, different queue: the next sell fills a different order first.
    tradematch::OrderBook reordered;
    reordered.submit({2, Side::Buy, OrderType::Limit, 20, 10'000});
    reordered.submit({1, Side::Buy, OrderType::Limit, 10, 10'000});
    expect(tradematch::state_hash(first) != tradematch::state_hash(reordered), "Queue order is part of the state.");

    second.submit({3, Side::Sell, OrderType::Limit, 5, 10'500});
    second.cancel(3);  // back to the same orders, but sequence 3 has been used
    expect(tradematch::state_hash(first) != tradematch::state_hash(second),
           "The next sequence number is part of the state too.");

    tradematch::MatchingEngine engine;
    tradematch::MatchingEngine renamed;
    engine.add_symbol("ACME");
    renamed.add_symbol("GLOBEX");
    expect(tradematch::state_hash(engine) != tradematch::state_hash(renamed), "Symbol names are part of the state.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"crc32c_matches_reference_vectors", test_crc32c_matches_reference_vectors},
        {"varints_round_trip", test_varints_round_trip},
        {"commands_round_trip_through_a_journal", test_commands_round_trip_through_a_journal},
        {"reopening_a_journal_appends", test_reopening_a_journal_appends},
        {"a_torn_final_record_is_cut_off", test_a_torn_final_record_is_cut_off},
        {"corruption_is_an_error", test_corruption_is_an_error},
        {"durability_decides_when_records_reach_the_file", test_durability_decides_when_records_reach_the_file},
        {"writer_rejects_bad_input_without_damaging_the_journal",
         test_writer_rejects_bad_input_without_damaging_the_journal},
        {"error_paths_are_reported", test_error_paths_are_reported},
        {"replaying_a_simulated_session_rebuilds_identical_books",
         test_replaying_a_simulated_session_rebuilds_identical_books},
        {"outside_orders_replay_with_the_session", test_outside_orders_replay_with_the_session},
        {"recovery_from_a_snapshot_and_the_journal_tail", test_recovery_from_a_snapshot_and_the_journal_tail},
        {"recovery_refuses_gaps_between_snapshot_and_journal", test_recovery_refuses_gaps_between_snapshot_and_journal},
        {"snapshot_files_detect_damage", test_snapshot_files_detect_damage},
        {"import_rejects_states_a_book_cannot_reach", test_import_rejects_states_a_book_cannot_reach},
        {"state_hash_reflects_every_detail", test_state_hash_reflects_every_detail},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures == 0U) {
        std::cout << "All journal tests passed (" << tests.size() << ").\n";
        return 0;
    }
    std::cout << failures << " test(s) failed.\n";
    return 1;
}

}  // namespace

int main() {
    try {
        return run_all_tests();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
