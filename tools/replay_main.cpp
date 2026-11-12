// Rebuilds a matching engine from a journal, optionally starting from a snapshot, and reports what it replayed and
// the resulting state. Every record is verified on the way.
#include "tradematch/journal.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/snapshot.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using tradematch::CommandType;
using tradematch::format_price;
using tradematch::SymbolId;

struct Options {
    std::filesystem::path journal;
    std::optional<std::filesystem::path> snapshot;
    std::optional<std::filesystem::path> save_snapshot;
    std::optional<std::uint64_t> expected_hash;
    std::size_t depth{0};
};

std::string with_commas(std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), ",");
    }
    return digits;
}

std::string hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::uint64_t parse_unsigned(std::string_view flag, const std::string& text, int base) {
    std::uint64_t value = 0;
    const auto* const last = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), last, value, base);
    if (text.empty() || error != std::errc{} || stop != last) {
        throw std::invalid_argument("invalid value for " + std::string{flag} + ": '" + text + "'");
    }
    return value;
}

void print_usage() {
    std::cout << "usage: tradematch_replay JOURNAL [--snapshot FILE] [--save-snapshot FILE] [--expect-hash HEX]\n"
                 "                         [--depth N]\n"
                 "\n"
                 "  --snapshot FILE       start from a snapshot and replay only the journal records after it\n"
                 "  --save-snapshot FILE  write a snapshot of the replayed state\n"
                 "  --expect-hash HEX     exit with status 1 unless the final state hash matches\n"
                 "  --depth N             print N price levels per side for every book\n";
}

// Returns nothing when --help was printed.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return std::nullopt;
        }
        if (!arg.starts_with("--")) {
            if (!options.journal.empty()) {
                throw std::invalid_argument("only one journal can be replayed at a time");
            }
            options.journal = arg;
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const auto& value = args[++index];
        if (arg == "--snapshot") {
            options.snapshot = value;
        } else if (arg == "--save-snapshot") {
            options.save_snapshot = value;
        } else if (arg == "--expect-hash") {
            const std::string digits = value.starts_with("0x") ? value.substr(2) : value;
            options.expected_hash = parse_unsigned(arg, digits, 16);
        } else if (arg == "--depth") {
            options.depth = static_cast<std::size_t>(parse_unsigned(arg, value, 10));
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (options.journal.empty()) {
        throw std::invalid_argument("no journal given (see --help)");
    }
    return options;
}

void print_book(const tradematch::MatchingEngine& engine, SymbolId symbol, std::size_t depth) {
    const auto& book = engine.book(symbol);
    const auto last = book.last_trade_price();
    std::cout << "  " << std::left << std::setw(8) << engine.symbol_name(symbol) << std::right << ' '
              << (book.phase() == tradematch::TradingPhase::Auction ? "AUCTION   " : "CONTINUOUS") << "  last "
              << (last ? format_price(*last) : "-") << "  resting " << with_commas(book.order_count()) << "  stops "
              << with_commas(book.stop_order_count()) << '\n';

    const auto snapshot = book.snapshot();
    const auto level_text = [](const tradematch::PriceLevelView& level) {
        return format_price(level.price) + " x " + with_commas(level.total_quantity) + " (" +
               std::to_string(level.order_count) + ")";
    };
    const std::size_t rows = std::max<std::size_t>(depth, 1);
    for (std::size_t row = 0; row < rows; ++row) {
        const bool bid = row < snapshot.bids.size();
        const bool ask = row < snapshot.asks.size();
        if (!bid && !ask) {
            break;
        }
        std::cout << "           " << (row == 0 ? "bid " : "    ") << std::left << std::setw(26)
                  << (bid ? level_text(snapshot.bids[row]) : "-") << std::right << (row == 0 ? "ask " : "    ")
                  << (ask ? level_text(snapshot.asks[row]) : "-") << '\n';
    }
}

int run(int argc, char** argv) {
    const auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return 0;
    }
    const Options& options = *parsed;

    tradematch::MatchingEngine engine;
    std::uint64_t snapshot_sequence = 0;
    if (options.snapshot) {
        const auto snapshot = tradematch::load_snapshot(*options.snapshot);
        tradematch::restore(engine, snapshot);
        snapshot_sequence = snapshot.sequence;
    }

    tradematch::JournalReader reader(options.journal);
    if (reader.first_sequence() > snapshot_sequence + 1) {
        throw tradematch::JournalError("the journal starts at record " + std::to_string(reader.first_sequence()) +
                                       ", after the state's last record " + std::to_string(snapshot_sequence));
    }

    std::array<std::uint64_t, tradematch::kCommandTypeCount + 1> by_type{};
    std::uint64_t replayed = 0;
    std::uint64_t trades = 0;
    tradematch::Command command;
    const auto start = std::chrono::steady_clock::now();
    while (reader.next(command)) {
        if (reader.sequence() <= snapshot_sequence) {
            continue;
        }
        trades += tradematch::apply(engine, command).size();
        ++by_type.at(static_cast<std::size_t>(command.type));
        ++replayed;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (reader.sequence() < snapshot_sequence) {
        throw tradematch::JournalError("the journal ends at record " + std::to_string(reader.sequence()) +
                                       ", before the snapshot at record " + std::to_string(snapshot_sequence));
    }

    std::cout << "journal     " << options.journal.string() << "  " << with_commas(reader.size_bytes())
              << " bytes, records " << with_commas(reader.first_sequence()) << ".." << with_commas(reader.sequence())
              << '\n';
    if (options.snapshot) {
        std::cout << "snapshot    " << options.snapshot->string() << "  covers records up to "
                  << with_commas(snapshot_sequence) << '\n';
    }
    std::cout << "replayed    " << with_commas(replayed) << " commands in " << std::fixed << std::setprecision(1)
              << elapsed * 1e3 << " ms";
    if (replayed > 0U && elapsed > 0.0) {
        std::cout << "  (" << std::setprecision(2) << static_cast<double>(replayed) / elapsed / 1e6
                  << "M commands/s)\n          ";
        for (std::size_t type = 1; type <= tradematch::kCommandTypeCount; ++type) {
            if (by_type.at(type) != 0U) {
                std::cout << "  " << tradematch::to_string(static_cast<CommandType>(type)) << ' '
                          << with_commas(by_type.at(type));
            }
        }
    }
    std::cout << "\ntrades      " << with_commas(trades) << '\n';
    if (reader.torn_bytes() != 0U) {
        std::cout << "torn tail   " << reader.torn_bytes()
                  << " bytes of an incomplete final record were ignored (a crash during an append)\n";
    }
    if (options.save_snapshot) {
        tradematch::save_snapshot(*options.save_snapshot, tradematch::capture(engine, reader.sequence()));
        std::cout << "saved       " << options.save_snapshot->string() << "  covers records up to "
                  << with_commas(reader.sequence()) << '\n';
    }

    const std::uint64_t hash = tradematch::state_hash(engine);
    std::cout << "state hash  " << hex(hash) << '\n';
    for (SymbolId symbol = 0; symbol < engine.symbol_count(); ++symbol) {
        print_book(engine, symbol, options.depth);
    }
    if (options.expected_hash && *options.expected_hash != hash) {
        std::cerr << "error: the state hash " << hex(hash) << " does not match the expected "
                  << hex(*options.expected_hash) << '\n';
        return 1;
    }
    if (options.expected_hash) {
        std::cout << "verified    the replayed state matches the expected hash\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
