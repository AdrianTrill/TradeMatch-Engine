#include "tradematch/journal.hpp"

#include "tradematch/detail/codec.hpp"
#include "tradematch/detail/file_io.hpp"
#include "tradematch/detail/spin.hpp"
#include "tradematch/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>

namespace tradematch {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'T', 'M', 'J', 'O', 'U', 'R', 'N', 'L'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderChecksumOffset = kJournalHeaderBytes - 4;
constexpr std::size_t kWriteThreshold = std::size_t{64} * 1'024;
constexpr std::size_t kBufferBytes = kWriteThreshold + kJournalRecordHeaderBytes + kMaxJournalPayloadBytes;
constexpr std::size_t kBackgroundBuffers = 4;
constexpr auto kIoSpinWindow = std::chrono::microseconds{200};

void store32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4U; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
}

// Timestamp deltas wrap modulo 2^64, which round-trips any pair of timestamps without signed overflow.
std::int64_t timestamp_delta(Timestamp from, Timestamp to) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(to) - static_cast<std::uint64_t>(from));
}

Timestamp add_delta(Timestamp from, std::int64_t delta) noexcept {
    return static_cast<Timestamp>(static_cast<std::uint64_t>(from) + static_cast<std::uint64_t>(delta));
}

std::vector<std::uint8_t> read_journal_file(const std::filesystem::path& path) {
    try {
        return detail::read_file(path);
    } catch (const std::runtime_error& error) {
        throw JournalError(error.what());
    }
}

void decode_payload(std::span<const std::uint8_t> payload, Timestamp previous, Command& command) {
    detail::ByteReader in(payload);
    const std::uint8_t type = in.u8();
    if (type == 0U || type > kCommandTypeCount) {
        throw detail::DecodeError("unknown command type " + std::to_string(type));
    }
    command.type = static_cast<CommandType>(type);
    command.timestamp = add_delta(previous, in.svarint());
    command.symbol = 0;
    command.order = OrderRequest{};
    command.order_id = 0;
    command.symbol_name.clear();
    command.capacity = BookCapacity{};

    switch (command.type) {
        case CommandType::AddSymbol: {
            const auto name = in.bytes(in.count("symbol name length"));
            if (name.size() > kMaxSymbolNameBytes) {
                throw detail::DecodeError("symbol name is too long");
            }
            command.symbol_name.assign(reinterpret_cast<const char*>(name.data()), name.size());
            command.capacity.orders = in.varint_as<std::size_t>("order capacity");
            command.capacity.price_levels = in.varint_as<std::size_t>("price level capacity");
            command.capacity.trades_per_call = in.varint_as<std::size_t>("trade capacity");
            command.capacity.stop_orders = in.varint_as<std::size_t>("stop order capacity");
            break;
        }
        case CommandType::Submit:
        case CommandType::Replace:
            command.symbol = in.varint_as<SymbolId>("symbol");
            command.order = detail::decode_order(in);
            break;
        case CommandType::Cancel:
            command.symbol = in.varint_as<SymbolId>("symbol");
            command.order_id = in.varint();
            break;
        case CommandType::BeginAuction:
        case CommandType::Uncross:
            command.symbol = in.varint_as<SymbolId>("symbol");
            break;
        case CommandType::ExpireDayOrders:
            break;
    }
    if (in.remaining() != 0U) {
        throw detail::DecodeError("unexpected bytes after the command");
    }
}

OrderBook& auction_book(MatchingEngine& engine, const Command& command) {
    if (!engine.has_symbol(command.symbol)) {
        throw JournalError(std::string{to_string(command.type)} + " names unknown symbol " +
                           std::to_string(command.symbol));
    }
    return engine.book(command.symbol);
}

}  // namespace

const char* to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::AddSymbol:
            return "ADD_SYMBOL";
        case CommandType::Submit:
            return "SUBMIT";
        case CommandType::Cancel:
            return "CANCEL";
        case CommandType::Replace:
            return "REPLACE";
        case CommandType::BeginAuction:
            return "BEGIN_AUCTION";
        case CommandType::Uncross:
            return "UNCROSS";
        case CommandType::ExpireDayOrders:
            return "EXPIRE_DAY_ORDERS";
    }
    return "UNKNOWN";
}

JournalWriter::JournalWriter(const std::filesystem::path& path, const JournalOptions& options)
    : path_(path),
      durability_(options.durability),
      buffers_(options.durability == Durability::Background ? kBackgroundBuffers : 1,
               std::vector<std::uint8_t>(kBufferBytes)),
      active_(buffers_.front().data()) {
    std::error_code error;
    const std::uintmax_t existing_bytes = std::filesystem::file_size(path, error);
    const bool resume = !options.truncate && !error && existing_bytes > 0;  // an empty file is a new journal
    if (resume) {
        // Verify the whole journal before touching it. Corruption throws here; a torn final record is cut off.
        JournalReader reader(path);
        Command command;
        while (reader.next(command)) {
        }
        next_sequence_ = reader.sequence() + 1;
        last_timestamp_ = reader.last_timestamp();
        torn_bytes_ = reader.torn_bytes();
        if (torn_bytes_ > 0U) {
            std::filesystem::resize_file(path, reader.valid_bytes(), error);
            if (error) {
                throw JournalError("cannot truncate the torn tail of " + path.string() + ": " + error.message());
            }
        }
        file_bytes_ = reader.valid_bytes();
        file_ = detail::open_file(path, "ab");
    } else {
        if (options.first_sequence == 0U) {
            throw std::invalid_argument("journal sequence numbers start at 1");
        }
        next_sequence_ = options.first_sequence;
        file_ = detail::open_file(path, "wb");
    }
    if (file_ == nullptr) {
        throw JournalError("cannot open " + path.string() + ": " + detail::last_error());
    }
    std::setvbuf(file_, nullptr, _IONBF, 0);  // the writer does its own buffering

    if (!resume) {
        std::vector<std::uint8_t> header;
        detail::ByteWriter out(header);
        out.bytes(kMagic);
        out.fixed32(kFormatVersion);
        out.fixed64(next_sequence_);
        out.fixed32(detail::crc32c(header));
        std::ranges::copy(header, active_);
        buffered_ = header.size();
        try {
            write_buffer();
            if (durability_ == Durability::Sync) {
                sync();
                detail::sync_directory(path.parent_path());
            }
        } catch (const JournalError&) {
            // The destructor doesn't run when a constructor throws, so release the file here.
            std::fclose(file_);
            file_ = nullptr;
            throw;
        }
    }
    if (durability_ == Durability::Background) {
        try {
            start_background();
        } catch (const std::system_error&) {
            std::fclose(file_);
            file_ = nullptr;
            throw;
        }
    }
}

JournalWriter::~JournalWriter() {
    try {
        close();
    } catch (const std::exception&) {
        // A destructor can't report the failure; close() has still stopped the I/O thread and released the file.
        failed_ = true;
    }
}

void JournalWriter::check_open() const {
    if (file_ == nullptr) {
        throw JournalError("the journal " + path_.string() + " is closed");
    }
    if (failed_) {
        throw JournalError("the journal " + path_.string() + " is unusable after a failed write");
    }
}

detail::RawWriter JournalWriter::begin_record(CommandType type, Timestamp timestamp) {
    check_open();
    record_timestamp_ = timestamp;
    // The length and checksum go in front of the payload once it is complete.
    detail::RawWriter out(active_ + buffered_ + kJournalRecordHeaderBytes);
    out.u8(static_cast<std::uint8_t>(type));
    out.svarint(timestamp_delta(last_timestamp_, timestamp));
    return out;
}

std::uint64_t JournalWriter::end_record(const detail::RawWriter& out) {
    std::uint8_t* const header = active_ + buffered_;
    std::uint8_t* const payload = header + kJournalRecordHeaderBytes;
    const auto length = static_cast<std::size_t>(out.position() - payload);
    store32(header, static_cast<std::uint32_t>(length));
    store32(header + 4, detail::crc32c({payload, length}));
    buffered_ += kJournalRecordHeaderBytes + length;
    last_timestamp_ = record_timestamp_;
    const std::uint64_t sequence = next_sequence_++;

    if (durability_ == Durability::Background) {
        if (buffered_ >= kWriteThreshold) {
            hand_off();
        }
    } else if (durability_ != Durability::Buffered || buffered_ >= kWriteThreshold) {
        write_buffer();
        if (durability_ == Durability::Sync && !detail::sync_file(file_)) {
            failed_ = true;
            throw JournalError("cannot sync " + path_.string() + ": " + detail::last_error());
        }
    }
    return sequence;
}

void JournalWriter::write_buffer() {
    if (buffered_ == 0U) {
        return;
    }
    if (std::fwrite(active_, 1, buffered_, file_) != buffered_) {
        failed_ = true;
        throw JournalError("cannot write " + path_.string() + ": " + detail::last_error());
    }
    file_bytes_ += buffered_;
    buffered_ = 0;
}

std::uint64_t JournalWriter::append(const Command& command) {
    switch (command.type) {
        case CommandType::AddSymbol:
            return append_add_symbol(command.timestamp, command.symbol_name, command.capacity);
        case CommandType::Submit:
            return append_submit(command.timestamp, command.symbol, command.order);
        case CommandType::Cancel:
            return append_cancel(command.timestamp, command.symbol, command.order_id);
        case CommandType::Replace:
            return append_replace(command.timestamp, command.symbol, command.order);
        case CommandType::BeginAuction:
            return append_begin_auction(command.timestamp, command.symbol);
        case CommandType::Uncross:
            return append_uncross(command.timestamp, command.symbol);
        case CommandType::ExpireDayOrders:
            return append_expire_day_orders(command.timestamp);
    }
    throw std::invalid_argument("unknown command type");
}

std::uint64_t JournalWriter::append_add_symbol(Timestamp timestamp,
                                               std::string_view name,
                                               const BookCapacity& capacity) {
    if (name.size() > kMaxSymbolNameBytes) {
        throw std::invalid_argument("symbol names are limited to " + std::to_string(kMaxSymbolNameBytes) + " bytes");
    }
    auto out = begin_record(CommandType::AddSymbol, timestamp);
    out.varint(name.size());
    out.bytes(std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    out.varint(capacity.orders);
    out.varint(capacity.price_levels);
    out.varint(capacity.trades_per_call);
    out.varint(capacity.stop_orders);
    return end_record(out);
}

std::uint64_t JournalWriter::append_submit(Timestamp timestamp, SymbolId symbol, const OrderRequest& order) {
    auto out = begin_record(CommandType::Submit, timestamp);
    out.varint(symbol);
    detail::encode_order(out, order);
    return end_record(out);
}

std::uint64_t JournalWriter::append_cancel(Timestamp timestamp, SymbolId symbol, OrderId order_id) {
    auto out = begin_record(CommandType::Cancel, timestamp);
    out.varint(symbol);
    out.varint(order_id);
    return end_record(out);
}

std::uint64_t JournalWriter::append_replace(Timestamp timestamp, SymbolId symbol, const OrderRequest& replacement) {
    auto out = begin_record(CommandType::Replace, timestamp);
    out.varint(symbol);
    detail::encode_order(out, replacement);
    return end_record(out);
}

std::uint64_t JournalWriter::append_begin_auction(Timestamp timestamp, SymbolId symbol) {
    auto out = begin_record(CommandType::BeginAuction, timestamp);
    out.varint(symbol);
    return end_record(out);
}

std::uint64_t JournalWriter::append_uncross(Timestamp timestamp, SymbolId symbol) {
    auto out = begin_record(CommandType::Uncross, timestamp);
    out.varint(symbol);
    return end_record(out);
}

std::uint64_t JournalWriter::append_expire_day_orders(Timestamp timestamp) {
    return end_record(begin_record(CommandType::ExpireDayOrders, timestamp));
}

void JournalWriter::flush() {
    check_open();
    if (durability_ == Durability::Background) {
        if (buffered_ != 0U) {
            hand_off();
        }
        wait_until_written();
        check_background_error();
        return;
    }
    write_buffer();
}

void JournalWriter::sync() {
    flush();
    if (!detail::sync_file(file_)) {
        failed_ = true;
        throw JournalError("cannot sync " + path_.string() + ": " + detail::last_error());
    }
}

void JournalWriter::close() {
    if (file_ == nullptr) {
        return;
    }
    std::string failure;
    try {
        if (durability_ == Durability::Sync) {
            sync();
        } else {
            flush();
        }
    } catch (const JournalError& error) {
        failure = error.what();
    }
    // Whatever happened, stop the I/O thread and release the file.
    stop_background();
    if (std::fclose(file_) != 0 && failure.empty()) {
        failure = "cannot close " + path_.string() + ": " + detail::last_error();
    }
    file_ = nullptr;
    if (!failure.empty()) {
        throw JournalError(failure);
    }
}

void JournalWriter::start_background() {
    filled_ = std::make_unique<SpscRing<Batch>>(buffers_.size());
    free_ = std::make_unique<SpscRing<std::size_t>>(buffers_.size());
    for (std::size_t buffer = 1; buffer < buffers_.size(); ++buffer) {
        free_->try_push(buffer);
    }
    io_thread_ = std::thread([this] { background_loop(); });
}

// Queues the active buffer for the I/O thread and continues in a free one. Waits only if every buffer is in flight,
// which applies backpressure when the disk can't keep up.
void JournalWriter::hand_off() {
    check_background_error();
    filled_->try_push(Batch{active_buffer_, buffered_});  // can't fail: the ring has a slot for every buffer
    file_bytes_ += buffered_;
    buffered_ = 0;
    ++batches_submitted_;
    // A wake-up costs a system call, so it is only sent when the I/O thread has actually parked. Both sides use
    // sequentially consistent operations here: either the I/O thread sees the new wake_ value before parking, or this
    // thread sees io_parked_ and notifies it, so no wake-up can be lost.
    wake_.fetch_add(1, std::memory_order_seq_cst);
    if (io_parked_.load(std::memory_order_seq_cst)) {
        wake_.notify_one();
    }

    std::size_t next = 0;
    while (!free_->try_pop(next)) {
        const std::uint64_t written = batches_written_.load(std::memory_order_acquire);
        if (free_->try_pop(next)) {
            break;
        }
        batches_written_.wait(written, std::memory_order_acquire);
    }
    active_buffer_ = next;
    active_ = buffers_[next].data();
}

void JournalWriter::wait_until_written() {
    while (true) {
        const std::uint64_t written = batches_written_.load(std::memory_order_acquire);
        if (written >= batches_submitted_) {
            return;
        }
        batches_written_.wait(written, std::memory_order_acquire);
    }
}

void JournalWriter::check_background_error() {
    if (io_failed_.load(std::memory_order_acquire)) {
        failed_ = true;
        throw JournalError("cannot write " + path_.string() + ": " + std::generic_category().message(io_errno_));
    }
}

void JournalWriter::stop_background() noexcept {
    if (!io_thread_.joinable()) {
        return;
    }
    stopping_.store(true, std::memory_order_seq_cst);
    wake_.fetch_add(1, std::memory_order_seq_cst);
    wake_.notify_one();
    io_thread_.join();
}

// The I/O thread: writes batches in order and returns their buffers. After a failed write it stops writing, so
// nothing lands after a gap, but keeps returning buffers so the appending thread never blocks; the appending thread
// reports the error on its next hand-off or flush.
void JournalWriter::background_loop() noexcept {
    Batch batch;
    while (true) {
        const std::uint64_t wake = wake_.load(std::memory_order_acquire);
        const bool stopping = stopping_.load(std::memory_order_acquire);
        while (filled_->try_pop(batch)) {
            if (!io_failed_.load(std::memory_order_relaxed) &&
                std::fwrite(buffers_[batch.buffer].data(), 1, batch.bytes, file_) != batch.bytes) {
                io_errno_ = errno;
                io_failed_.store(true, std::memory_order_release);
            }
            free_->try_push(batch.buffer);
            batches_written_.fetch_add(1, std::memory_order_release);
            batches_written_.notify_one();
        }
        if (stopping) {
            return;  // every batch handed off before the stop request has been written
        }
        // Under load the next batch arrives within the spin window, so the appending thread never has to pay for a
        // wake-up. Otherwise the thread parks instead of burning a core.
        if (spin_until_woken(wake)) {
            continue;
        }
        io_parked_.store(true, std::memory_order_seq_cst);
        if (wake_.load(std::memory_order_seq_cst) == wake) {
            wake_.wait(wake, std::memory_order_acquire);
        }
        io_parked_.store(false, std::memory_order_relaxed);
    }
}

bool JournalWriter::spin_until_woken(std::uint64_t wake) const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kIoSpinWindow;
    detail::Backoff backoff;
    for (unsigned round = 1;; ++round) {
        if (wake_.load(std::memory_order_acquire) != wake) {
            return true;
        }
        backoff.pause();
        if (round % 64U == 0U && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
}

JournalReader::JournalReader(const std::filesystem::path& path) : bytes_(read_journal_file(path)) {
    read_header();
}

JournalReader::JournalReader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
    read_header();
}

void JournalReader::fail(const std::string& problem) const {
    throw JournalError("journal record " + std::to_string(sequence_ + 1) + " at byte " + std::to_string(position_) +
                       ' ' + problem);
}

void JournalReader::read_header() {
    const std::span<const std::uint8_t> bytes(bytes_);
    if (bytes.size() < kJournalHeaderBytes || !std::ranges::equal(bytes.first(kMagic.size()), kMagic)) {
        throw JournalError("not a TradeMatch journal");
    }
    detail::ByteReader header(bytes.first(kJournalHeaderBytes));
    header.bytes(kMagic.size());
    const std::uint32_t version = header.fixed32();
    first_sequence_ = header.fixed64();
    if (header.fixed32() != detail::crc32c(bytes.first(kHeaderChecksumOffset))) {
        throw JournalError("the journal header is corrupt");
    }
    if (version != kFormatVersion) {
        throw JournalError("unsupported journal format version " + std::to_string(version));
    }
    if (first_sequence_ == 0U) {
        throw JournalError("journal sequence numbers start at 1");
    }
    sequence_ = first_sequence_ - 1;
    position_ = kJournalHeaderBytes;
}

// An append interrupted by a crash leaves an incomplete record at the very end of the file: a cut-off header or
// payload, a payload that was only partly persisted (bad checksum), or zero-filled space. Anything short of the end
// that fails these checks is corruption.
bool JournalReader::torn_tail(std::size_t length) {
    torn_bytes_ = length;
    return false;
}

bool JournalReader::next(Command& command) {
    const auto rest = std::span<const std::uint8_t>(bytes_).subspan(position_);
    if (rest.empty()) {
        return false;
    }
    if (rest.size() < kJournalRecordHeaderBytes) {
        return torn_tail(rest.size());
    }
    detail::ByteReader header(rest.first(kJournalRecordHeaderBytes));
    const std::uint32_t length = header.fixed32();
    const std::uint32_t checksum = header.fixed32();
    if (length == 0U || length > kMaxJournalPayloadBytes) {
        if (std::ranges::all_of(rest, [](std::uint8_t byte) { return byte == 0U; })) {
            return torn_tail(rest.size());
        }
        fail("has an invalid length");
    }
    const std::size_t record_bytes = kJournalRecordHeaderBytes + length;
    if (record_bytes > rest.size()) {
        return torn_tail(rest.size());
    }
    const auto payload = rest.subspan(kJournalRecordHeaderBytes, length);
    if (detail::crc32c(payload) != checksum) {
        if (record_bytes == rest.size()) {
            return torn_tail(rest.size());
        }
        fail("fails its checksum");
    }
    try {
        decode_payload(payload, last_timestamp_, command);
    } catch (const detail::DecodeError& error) {
        fail(std::string{"is malformed: "} + error.what());
    }
    position_ += record_bytes;
    ++sequence_;
    last_timestamp_ = command.timestamp;
    return true;
}

SymbolId JournaledEngine::add_symbol(std::string_view name, const BookCapacity& capacity) {
    if (journal_ != nullptr) {
        journal_->append_add_symbol(now_, name, capacity);
    }
    return engine_.add_symbol(name, capacity);
}

SubmitResult JournaledEngine::submit(SymbolId symbol, const OrderRequest& order) {
    if (journal_ != nullptr) {
        journal_->append_submit(now_, symbol, order);
    }
    return engine_.submit(symbol, order);
}

CancelResult JournaledEngine::cancel(SymbolId symbol, OrderId order_id) {
    if (journal_ != nullptr) {
        journal_->append_cancel(now_, symbol, order_id);
    }
    return engine_.cancel(symbol, order_id);
}

ReplaceResult JournaledEngine::replace(SymbolId symbol, const OrderRequest& replacement) {
    if (journal_ != nullptr) {
        journal_->append_replace(now_, symbol, replacement);
    }
    return engine_.replace(symbol, replacement);
}

void JournaledEngine::begin_auction(SymbolId symbol) {
    OrderBook& book = engine_.book(symbol);
    if (journal_ != nullptr) {
        journal_->append_begin_auction(now_, symbol);
    }
    book.begin_auction();
}

AuctionResult JournaledEngine::uncross(SymbolId symbol) {
    OrderBook& book = engine_.book(symbol);
    if (journal_ != nullptr) {
        journal_->append_uncross(now_, symbol);
    }
    return book.uncross();
}

std::vector<std::pair<SymbolId, OrderId>> JournaledEngine::expire_day_orders() {
    if (journal_ != nullptr) {
        journal_->append_expire_day_orders(now_);
    }
    return engine_.expire_day_orders();
}

std::uint64_t JournaledEngine::save_snapshot(const std::filesystem::path& path) {
    std::uint64_t sequence = 0;
    if (journal_ != nullptr) {
        // Every record the snapshot covers must be durable first; otherwise a crash could leave a journal that ends
        // before its snapshot, and appending would then reuse sequence numbers the snapshot already includes.
        journal_->sync();
        sequence = journal_->last_sequence();
    }
    tradematch::save_snapshot(path, capture(engine_, sequence));
    return sequence;
}

std::span<const Trade> apply(MatchingEngine& engine, const Command& command) {
    switch (command.type) {
        case CommandType::AddSymbol:
            engine.add_symbol(command.symbol_name, command.capacity);
            return {};
        case CommandType::Submit:
            return engine.submit(command.symbol, command.order).trades;
        case CommandType::Cancel:
            engine.cancel(command.symbol, command.order_id);
            return {};
        case CommandType::Replace:
            return engine.replace(command.symbol, command.order).submit_result.trades;
        case CommandType::BeginAuction:
            auction_book(engine, command).begin_auction();
            return {};
        case CommandType::Uncross:
            return auction_book(engine, command).uncross().trades;
        case CommandType::ExpireDayOrders:
            engine.expire_day_orders();
            return {};
    }
    throw JournalError("unknown command type");
}

}  // namespace tradematch
