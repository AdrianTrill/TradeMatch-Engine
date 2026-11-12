#ifndef TRADEMATCH_JOURNAL_HPP
#define TRADEMATCH_JOURNAL_HPP

#include "tradematch/detail/codec.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tradematch {

enum class CommandType : std::uint8_t {
    AddSymbol = 1,
    Submit,
    Cancel,
    Replace,
    BeginAuction,
    Uncross,
    ExpireDayOrders
};

inline constexpr std::size_t kCommandTypeCount = 7;

const char* to_string(CommandType type) noexcept;

// One state-changing request to a MatchingEngine. Only the fields its type uses are meaningful.
struct Command {
    CommandType type{CommandType::Submit};
    Timestamp timestamp{};
    SymbolId symbol{};        // every type except AddSymbol and ExpireDayOrders
    OrderRequest order;       // Submit and Replace
    OrderId order_id{};       // Cancel
    std::string symbol_name;  // AddSymbol
    BookCapacity capacity;    // AddSymbol

    bool operator==(const Command&) const = default;
};

class JournalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// When an appended record reaches the operating system and the disk.
enum class Durability : std::uint8_t {
    Buffered,  // records are written in 64 KiB batches; a crash can lose the latest batch
    Flush,     // each record is handed to the operating system at once, so it survives a process crash
    Sync       // each record is forced to stable storage before append returns, so it survives power loss
};

struct JournalOptions {
    Durability durability{Durability::Buffered};
    bool truncate{false};             // start a new journal even if the file exists
    std::uint64_t first_sequence{1};  // the sequence number of a new journal's first record
};

// Journal file format, little-endian:
//   header   "TMJOURNL", u32 format version, u64 first sequence number, u32 CRC-32C of those 20 bytes
//   records  u32 payload length, u32 CRC-32C of the payload, payload
// A payload holds the command type, the timestamp as a zigzag varint delta from the previous record, and the
// command's fields as varints, so a typical order takes about 20 bytes. Records are numbered consecutively from the
// header's first sequence number, which lets a journal start after a snapshot.
inline constexpr std::size_t kJournalHeaderBytes = 24;
inline constexpr std::size_t kJournalRecordHeaderBytes = 8;
inline constexpr std::size_t kMaxJournalPayloadBytes = 1'024;
inline constexpr std::size_t kMaxSymbolNameBytes = 255;

// Appends commands to a journal file. Opening an existing journal verifies every record and cuts off an incomplete
// final record left by a crash (a torn write), so appending resumes cleanly; corruption anywhere else throws
// JournalError rather than discarding data. Appends perform no heap allocations once the buffer has grown.
class JournalWriter {
public:
    explicit JournalWriter(const std::filesystem::path& path, const JournalOptions& options = {});
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&&) = delete;
    JournalWriter& operator=(JournalWriter&&) = delete;
    ~JournalWriter();  // flushes; call close() first to see any error

    // Each append returns the new record's sequence number.
    std::uint64_t append(const Command& command);
    std::uint64_t append_add_symbol(Timestamp timestamp, std::string_view name, const BookCapacity& capacity);
    std::uint64_t append_submit(Timestamp timestamp, SymbolId symbol, const OrderRequest& order);
    std::uint64_t append_cancel(Timestamp timestamp, SymbolId symbol, OrderId order_id);
    std::uint64_t append_replace(Timestamp timestamp, SymbolId symbol, const OrderRequest& replacement);
    std::uint64_t append_begin_auction(Timestamp timestamp, SymbolId symbol);
    std::uint64_t append_uncross(Timestamp timestamp, SymbolId symbol);
    std::uint64_t append_expire_day_orders(Timestamp timestamp);

    void flush();  // hands buffered records to the operating system
    void sync();   // flushes, then forces everything to stable storage
    void close();  // flushes (and syncs, with Durability::Sync), then closes the file

    // The sequence number of the last record in the journal, or first_sequence - 1 if it has none.
    std::uint64_t last_sequence() const noexcept {
        return next_sequence_ - 1;
    }

    std::uint64_t size_bytes() const noexcept {
        return file_bytes_ + buffered_;
    }

    // Bytes of a torn final record that were cut off when the journal was opened.
    std::uint64_t recovered_torn_bytes() const noexcept {
        return torn_bytes_;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    // Records are encoded straight into the buffer after the bytes already buffered, and only end_record() counts
    // them, so an exception while encoding leaves nothing behind.
    detail::RawWriter begin_record(CommandType type, Timestamp timestamp);
    std::uint64_t end_record(const detail::RawWriter& out);
    void write_buffer();
    void check_open() const;

    std::filesystem::path path_;
    std::FILE* file_{nullptr};
    Durability durability_{Durability::Buffered};
    std::vector<std::uint8_t> buffer_;  // fixed size: a full batch plus one record of the largest size
    std::size_t buffered_{0};
    Timestamp record_timestamp_{0};
    Timestamp last_timestamp_{0};
    std::uint64_t next_sequence_{1};
    std::uint64_t file_bytes_{0};
    std::uint64_t torn_bytes_{0};
    bool failed_{false};
};

// Reads a journal from start to end, verifying every record.
class JournalReader {
public:
    explicit JournalReader(const std::filesystem::path& path);
    explicit JournalReader(std::vector<std::uint8_t> bytes);

    // Reads the next command into `command`. Returns false at the end of the journal, including when the final record
    // is torn (see torn_bytes()). Throws JournalError if any other record is corrupt or malformed.
    bool next(Command& command);

    std::uint64_t first_sequence() const noexcept {
        return first_sequence_;
    }

    // The sequence number of the last command read, or first_sequence() - 1 before the first.
    std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    // The offset just past the last complete, verified record.
    std::size_t valid_bytes() const noexcept {
        return position_;
    }

    // The size of an incomplete final record, known once next() has returned false.
    std::size_t torn_bytes() const noexcept {
        return torn_bytes_;
    }

    std::size_t size_bytes() const noexcept {
        return bytes_.size();
    }

    // The timestamp of the last command read; appending continues the timestamp deltas from here.
    Timestamp last_timestamp() const noexcept {
        return last_timestamp_;
    }

private:
    void read_header();
    [[noreturn]] void fail(const std::string& problem) const;
    bool torn_tail(std::size_t length);

    std::vector<std::uint8_t> bytes_;
    std::size_t position_{0};
    std::uint64_t first_sequence_{1};
    std::uint64_t sequence_{0};
    Timestamp last_timestamp_{0};
    std::size_t torn_bytes_{0};
};

// The write-ahead front end of a MatchingEngine. Every state-changing call is journaled before it is applied, so
// replaying the journal into an empty engine rebuilds exactly the same state. Rejected requests are journaled too,
// since replay rejects them in the same way. Without a journal, calls go straight to the engine.
class JournaledEngine {
public:
    explicit JournaledEngine(MatchingEngine& engine, JournalWriter* journal = nullptr) noexcept
        : engine_(engine), journal_(journal) {}

    // The timestamp recorded with the commands that follow.
    void set_time(Timestamp timestamp) noexcept {
        now_ = timestamp;
    }

    Timestamp time() const noexcept {
        return now_;
    }

    SymbolId add_symbol(std::string_view name, const BookCapacity& capacity = {});
    SubmitResult submit(SymbolId symbol, const OrderRequest& order);
    CancelResult cancel(SymbolId symbol, OrderId order_id);
    ReplaceResult replace(SymbolId symbol, const OrderRequest& replacement);
    // These two throw std::out_of_range for an unknown symbol, before anything is journaled.
    void begin_auction(SymbolId symbol);
    AuctionResult uncross(SymbolId symbol);
    std::vector<std::pair<SymbolId, OrderId>> expire_day_orders();

    // Flushes the journal, then atomically writes a snapshot of the engine as of the last journaled command. Returns
    // that command's sequence number. Recovery loads the snapshot and replays only the records after it.
    std::uint64_t save_snapshot(const std::filesystem::path& path);

    MatchingEngine& engine() noexcept {
        return engine_;
    }

    const MatchingEngine& engine() const noexcept {
        return engine_;
    }

    JournalWriter* journal() const noexcept {
        return journal_;
    }

private:
    MatchingEngine& engine_;
    JournalWriter* journal_;
    Timestamp now_{0};
};

// Applies one command to an engine exactly as JournaledEngine did when it recorded it, and returns the trades it
// caused (valid until the book's next request). Throws JournalError for an auction command naming an unknown symbol,
// which JournaledEngine never records.
std::span<const Trade> apply(MatchingEngine& engine, const Command& command);

}  // namespace tradematch

#endif
