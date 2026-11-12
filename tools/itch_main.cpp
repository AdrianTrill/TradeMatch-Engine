// Replays NASDAQ TotalView-ITCH 5.0 files into order books, and writes simulated trading days in the same format.
//
//   tradematch_itch replay FILE [--at TIME] [--symbol S]... [--top N]
//                                                           rebuild every stock's book from FILE ("-" reads stdin),
//                                                           up to a time of day if given
//   tradematch_itch generate FILE [--seed N] [--symbols A,B] [--minutes M] [--verify]
//                                                           write one simulated day of the TradeMatch exchange as ITCH
#include "tradematch/detail/file_io.hpp"
#include "tradematch/itch.hpp"
#include "tradematch/itch_feed.hpp"
#include "tradematch/simulator.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace itch = tradematch::itch;

constexpr std::uint64_t kMarketOpen = 34'200'000'000'000;  // 9:30 a.m., in nanoseconds since midnight
constexpr std::uint64_t kMinutesPerDay = 1'440;

std::string grouped(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (auto position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), 1, ',');
    }
    return digits;
}

std::string clock_time(std::uint64_t nanoseconds) {
    const auto seconds = nanoseconds / 1'000'000'000;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << seconds / 3'600 << ':' << std::setw(2) << seconds / 60 % 60 << ':'
         << std::setw(2) << seconds % 60 << '.' << std::setw(3) << nanoseconds / 1'000'000 % 1'000;
    return text.str();
}

std::uint64_t parse_number(const std::string& option, const std::string& value) {
    std::uint64_t number = 0;
    const auto [stop, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || stop != value.data() + value.size()) {
        throw std::invalid_argument(std::string{"invalid value for "}.append(option).append(": ").append(value));
    }
    return number;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(separator, start), text.size());
        if (end > start) {
            parts.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

// A time of day such as 10:00, 10:00:30, or 10:00:30.25, in nanoseconds since midnight.
std::uint64_t parse_clock(const std::string& text) {
    const auto parts = split(text, ':');
    if (parts.size() < 2 || parts.size() > 3) {
        throw std::invalid_argument("expected a time of day such as 10:00 or 10:00:30.25, not " + text);
    }
    const std::uint64_t hours = parse_number("--at", parts[0]);
    const std::uint64_t minutes = parse_number("--at", parts[1]);
    std::uint64_t seconds = 0;
    std::uint64_t fraction = 0;
    if (parts.size() == 3) {
        const auto dot = parts[2].find('.');
        seconds = parse_number("--at", parts[2].substr(0, dot));
        if (dot != std::string::npos) {
            std::string digits = parts[2].substr(dot + 1);
            if (digits.empty() || digits.size() > 9) {
                throw std::invalid_argument("expected at most nine decimals of a second in " + text);
            }
            digits.append(9 - digits.size(), '0');
            fraction = parse_number("--at", digits);
        }
    }
    if (hours >= 24 || minutes >= 60 || seconds >= 60) {
        throw std::invalid_argument("not a time of day: " + text);
    }
    return ((hours * 60 + minutes) * 60 + seconds) * 1'000'000'000 + fraction;
}

void print_depth(const itch::BookReplica& replica, const std::string& symbol, const std::string& moment) {
    const auto id = replica.find(symbol);
    if (!id) {
        std::cout << '\n' << symbol << ": not in the feed\n";
        return;
    }
    const tradematch::OrderBook& book = replica.engine().book(*id);
    const tradematch::BookSnapshot snapshot = book.snapshot();
    std::cout << '\n' << symbol << " " << moment << ", " << grouped(book.order_count()) << " orders resting\n";
    std::cout << "            BID                    ASK\n";
    for (std::size_t level = 0; level < 10; ++level) {
        if (level >= snapshot.bids.size() && level >= snapshot.asks.size()) {
            break;
        }
        const auto side = [&](const std::vector<tradematch::PriceLevelView>& levels) {
            if (level >= levels.size()) {
                return std::string(23, ' ');
            }
            std::ostringstream text;
            text << std::setw(10) << grouped(levels[level].total_quantity) << " @ " << std::left << std::setw(10)
                 << itch::format_price(static_cast<std::uint32_t>(levels[level].price));
            return text.str();
        };
        std::string row = "  " + side(snapshot.bids) + "  " + side(snapshot.asks);
        row.erase(row.find_last_not_of(' ') + 1);
        std::cout << row << '\n';
    }
}

int replay(const std::string& path,
           std::optional<std::uint64_t> until,
           const std::vector<std::string>& symbols,
           std::size_t top) {
    std::FILE* file = nullptr;
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        file = stdin;
    } else {
        file = tradematch::detail::open_file(path, "rb");
        if (file == nullptr) {
            throw std::runtime_error("can't open " + path + ": " + tradematch::detail::last_error());
        }
    }

    itch::FileReader reader(file);
    itch::BookReplica replica;
    const auto start = std::chrono::steady_clock::now();
    while (const auto message = reader.next()) {
        if (until && itch::timestamp_of(*message).value_or(0) > *until) {
            break;
        }
        replica.apply(*message);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (file != stdin) {
        std::fclose(file);
    }

    const itch::ReplicaStats& stats = replica.stats();
    const std::string moment = until ? "at " + clock_time(*until) : std::string{"at the end"};
    std::cout << "Replayed " << grouped(stats.total_messages) << " messages"
              << (until ? " up to " + clock_time(*until) : "") << " (" << grouped(reader.bytes_read() / 1'000'000)
              << " MB read) in " << std::fixed << std::setprecision(3) << seconds << " s: " << std::setprecision(1)
              << static_cast<double>(stats.total_messages) / std::max(seconds, 1e-9) / 1e6 << " million messages/s\n";
    std::cout << "  last timestamp " << clock_time(stats.last_timestamp) << ", invalid messages "
              << grouped(stats.invalid_messages) << ", anomalies " << grouped(stats.anomalies) << '\n';

    std::vector<std::pair<std::uint64_t, char>> by_type;
    for (std::size_t type = 0; type < stats.messages.size(); ++type) {
        if (stats.messages.at(type) > 0U) {
            by_type.emplace_back(stats.messages.at(type), static_cast<char>(type));
        }
    }
    std::ranges::sort(by_type, std::greater{});
    std::cout << "  by type:";
    for (const auto& [count, type] : by_type) {
        std::cout << ' ' << type << ' ' << grouped(count);
    }
    std::cout << "\n  " << grouped(replica.engine().symbol_count()) << " stocks, " << grouped(stats.live_orders)
              << " orders resting " << moment << " (peak " << grouped(stats.peak_orders) << ")\n";

    std::vector<tradematch::SymbolId> active(replica.engine().symbol_count());
    std::iota(active.begin(), active.end(), tradematch::SymbolId{0});
    std::ranges::sort(active, std::greater{}, [&](tradematch::SymbolId id) { return replica.stock_stats(id).volume; });
    active.resize(std::min(active.size(), top));
    if (!active.empty() && replica.stock_stats(active.front()).volume > 0U) {
        std::cout << "\nMost traded stocks\n  stock         volume        value ($)   executions        last\n";
        for (const tradematch::SymbolId id : active) {
            const itch::StockStats& stock = replica.stock_stats(id);
            if (stock.volume == 0U) {
                break;
            }
            std::cout << "  " << std::left << std::setw(8) << replica.engine().symbol_name(id) << std::right
                      << std::setw(13) << grouped(stock.volume) << std::setw(17)
                      << grouped(static_cast<std::uint64_t>(stock.value)) << std::setw(13) << grouped(stock.executions)
                      << std::setw(12) << itch::format_price(stock.last_price) << '\n';
        }
    }
    for (const std::string& symbol : symbols) {
        print_depth(replica, symbol, moment);
    }
    return 0;
}

// Replays the written file and compares the rebuilt books with the engine that produced the feed.
bool verify(const std::filesystem::path& path,
            const tradematch::MatchingEngine& engine,
            const itch::FeedPublisher& publisher) {
    std::FILE* file = tradematch::detail::open_file(path, "rb");
    if (file == nullptr) {
        throw std::runtime_error("can't reopen " + path.string());
    }
    itch::FileReader reader(file);
    itch::BookReplica replica;
    while (const auto message = reader.next()) {
        replica.apply(*message);
    }
    std::fclose(file);
    const std::string difference = itch::first_difference(engine, publisher, replica);
    if (!difference.empty() || replica.stats().anomalies > 0U || replica.stats().invalid_messages > 0U) {
        std::cout << "verification FAILED: " << (difference.empty() ? "the replica found anomalies" : difference)
                  << '\n';
        return false;
    }
    std::cout << "verified: replaying the file rebuilds every book exactly, order by order\n";
    return true;
}

int generate(const std::filesystem::path& path,
             std::uint64_t seed,
             std::vector<std::string> symbols,
             std::uint64_t minutes,
             bool check) {
    tradematch::SimulationConfig config;
    config.seed = seed;
    config.symbols = std::move(symbols);
    config.continuous = static_cast<tradematch::Timestamp>(minutes) * 60'000'000'000;
    config.measure_latency = false;
    tradematch::MarketSimulator simulator(config);
    itch::FeedPublisher publisher(simulator.engine());
    simulator.set_order_listener(&publisher);

    std::FILE* file = tradematch::detail::open_file(path, "wb");
    if (file == nullptr) {
        throw std::runtime_error("can't create " + path.string() + ": " + tradematch::detail::last_error());
    }
    std::uint64_t written = 0;
    const auto flush = [&] {
        const auto bytes = publisher.take_bytes();
        if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            throw std::runtime_error("can't write " + path.string());
        }
        written += bytes.size();
    };

    // The opening auction ends at 9:30, when continuous trading begins.
    const std::uint64_t day_start = kMarketOpen - static_cast<std::uint64_t>(config.opening_auction);
    publisher.set_time(day_start);
    publisher.system_event('O');  // start of messages
    publisher.system_event('S');  // start of system hours
    for (std::size_t index = 0; index < simulator.symbol_count(); ++index) {
        publisher.announce(simulator.symbol(index));
    }
    const auto started = std::chrono::steady_clock::now();
    tradematch::SessionPhase phase = simulator.session();
    while (phase != tradematch::SessionPhase::Closed) {
        publisher.set_time(day_start + static_cast<std::uint64_t>(simulator.now() + config.step));
        publisher.set_cross_type(phase == tradematch::SessionPhase::OpeningAuction ? 'O' : 'C');
        simulator.step();
        if (simulator.session() != phase) {
            phase = simulator.session();
            if (phase == tradematch::SessionPhase::Continuous) {
                publisher.system_event('Q');  // start of market hours, after the opening cross
            } else if (phase == tradematch::SessionPhase::Closed) {
                publisher.system_event('M');  // end of market hours, after the closing cross
            }
        }
        if (publisher.pending_bytes() >= std::size_t{1} << 20U) {
            flush();
        }
    }
    publisher.system_event('E');  // end of system hours
    publisher.system_event('C');  // end of messages
    flush();
    std::fclose(file);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    const tradematch::SimulationStats& stats = simulator.stats();
    std::cout << "Wrote " << path.string() << ": " << grouped(publisher.message_count()) << " ITCH 5.0 messages, "
              << grouped(written) << " bytes, " << clock_time(day_start) << " to "
              << clock_time(day_start + static_cast<std::uint64_t>(simulator.now())) << '\n';
    std::cout << "  " << grouped(stats.requests) << " orders, cancels, and replaces made " << grouped(stats.trades)
              << " trades and " << stats.auctions << " auction crosses, simulated in " << std::fixed
              << std::setprecision(2) << seconds << " s\n  by type:";
    for (const char type : std::string_view{"SRAEXDUCQ"}) {
        std::cout << ' ' << type << ' ' << grouped(publisher.message_count(type));
    }
    std::cout << '\n';
    return !check || verify(path, simulator.engine(), publisher) ? 0 : 1;
}

void usage() {
    std::cout << "usage: tradematch_itch replay FILE [--at TIME] [--symbol S]... [--top N]\n"
                 "       tradematch_itch generate FILE [--seed N] [--symbols A,B] [--minutes M] [--verify]\n"
                 "\n"
                 "replay rebuilds every stock's order book from a NASDAQ TotalView-ITCH 5.0 file (\"-\" reads stdin,\n"
                 "so a compressed file can be piped in with gunzip -c), stopping at a time of day such as 10:00 if\n"
                 "given. generate writes one simulated trading day of the TradeMatch exchange in the same format.\n";
}

int run(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 2 || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() || args[0] == "--help" || args[0] == "-h" ? 0 : 2;
    }
    const std::string& command = args[0];
    const std::string& path = args[1];

    std::vector<std::string> show;
    std::optional<std::uint64_t> until;
    std::size_t top = 10;
    std::uint64_t seed = 42;
    std::vector<std::string> symbols{"ACME", "GLOBEX", "INITECH"};
    std::uint64_t minutes = 30;
    bool check = false;
    for (std::size_t index = 2; index < args.size(); ++index) {
        const std::string& option = args[index];
        if (option == "--verify") {
            check = true;
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string& value = args[++index];
        if (option == "--symbol") {
            show.push_back(value);
        } else if (option == "--at") {
            until = parse_clock(value);
        } else if (option == "--top") {
            top = static_cast<std::size_t>(parse_number(option, value));
        } else if (option == "--seed") {
            seed = parse_number(option, value);
        } else if (option == "--symbols") {
            symbols = split(value, ',');
        } else if (option == "--minutes") {
            minutes = parse_number(option, value);
        } else {
            throw std::invalid_argument("unknown option " + option);
        }
    }

    if (command == "replay") {
        return replay(path, until, show, top);
    }
    if (command == "generate") {
        if (symbols.empty() || minutes == 0U || minutes > kMinutesPerDay) {
            throw std::invalid_argument("generate needs at least one symbol and 1 to 1440 minutes");
        }
        return generate(path, seed, symbols, minutes, check);
    }
    usage();
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "tradematch_itch: " << error.what() << '\n';
        return 1;
    }
}
