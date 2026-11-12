// Live terminal view of the matching engine running a simulated exchange session.
#include "tradematch/simulator.hpp"
#include "tradematch/snapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
std::atomic<bool> g_stop{false};
}  // namespace

// Signal handlers need C linkage; this one only sets a lock-free flag, which is async-signal-safe.
extern "C" void tradematch_tui_interrupt(int /*signal*/) {
    g_stop = true;
}

namespace {

using tradematch::format_price;
using tradematch::Price;
using tradematch::Side;
using tradematch::Timestamp;

constexpr std::size_t kWidth = 100;
constexpr std::size_t kHeight = 34;
constexpr std::size_t kBookLevels = 10;
constexpr std::size_t kChartHeight = 8;

struct Options {
    std::uint64_t seed{7};
    double speed{1.0};  // simulated seconds per real second
    double fps{15.0};
    std::uint64_t frames{0};  // 0 = run until interrupted
    bool sleep{true};
    bool headless{false};
    std::string record_path;
    std::string journal_path;
};

// Visible width of UTF-8 text: every code point used here (box drawing, blocks, arrows) is one column wide.
std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    for (const char byte : text) {
        width += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1U : 0U;
    }
    return width;
}

// One screen row that tracks its visible width separately from the ANSI escape codes it contains.
class Line {
public:
    Line& text(std::string_view content, std::string_view style = {}) {
        if (width_ >= kWidth) {
            return *this;
        }
        std::string clipped{content};
        if (width_ + display_width(clipped) > kWidth) {
            std::size_t keep = 0;
            std::size_t columns = 0;
            while (keep < clipped.size() && columns < kWidth - width_) {
                ++keep;
                while (keep < clipped.size() && (static_cast<unsigned char>(clipped[keep]) & 0xC0U) == 0x80U) {
                    ++keep;
                }
                ++columns;
            }
            clipped.resize(keep);
        }
        if (!style.empty()) {
            buffer_ += "\x1b[";
            buffer_ += style;
            buffer_ += 'm';
        }
        buffer_ += clipped;
        if (!style.empty()) {
            buffer_ += "\x1b[0m";
        }
        width_ += display_width(clipped);
        return *this;
    }

    Line& pad_to(std::size_t width, std::string_view style = {}) {
        if (width > width_) {
            text(std::string(width - width_, ' '), style);
        }
        return *this;
    }

    std::size_t width() const noexcept {
        return width_;
    }

    const std::string& str() const noexcept {
        return buffer_;
    }

private:
    std::string buffer_;
    std::size_t width_{0};
};

std::string right(std::string_view text, std::size_t width) {
    const std::size_t visible = display_width(text);
    return visible >= width ? std::string{text} : std::string(width - visible, ' ') + std::string{text};
}

std::string left(std::string_view text, std::size_t width) {
    const std::size_t visible = display_width(text);
    return visible >= width ? std::string{text} : std::string{text} + std::string(width - visible, ' ');
}

std::string with_commas(std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), ",");
    }
    return digits;
}

std::string compact(std::uint64_t value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (value >= 1'000'000) {
        out << static_cast<double>(value) / 1e6 << 'M';
    } else if (value >= 10'000) {
        out << static_cast<double>(value) / 1e3 << 'K';
    } else {
        return with_commas(value);
    }
    return out.str();
}

std::string clock_time(Timestamp nanoseconds) {
    const auto milliseconds = nanoseconds / 1'000'000;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << milliseconds / 60'000 << ':' << std::setw(2)
        << (milliseconds / 1'000) % 60 << '.' << std::setw(3) << milliseconds % 1'000;
    return out.str();
}

std::string latency_text(std::uint64_t nanoseconds) {
    if (nanoseconds < 10'000) {
        return std::to_string(nanoseconds) + "ns";
    }
    return std::to_string(nanoseconds / 1'000) + "us";
}

// A horizontal bar with eighth-block resolution.
std::string bar(std::uint64_t value, std::uint64_t maximum, std::size_t width) {
    static constexpr std::array<std::string_view, 8> kPartials{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    if (maximum == 0U) {
        return {};
    }
    const std::uint64_t eighths = value * width * 8U / maximum;
    std::string result;
    for (std::uint64_t full = 0; full < eighths / 8U; ++full) {
        result += "█";
    }
    result += kPartials.at(eighths % 8U);
    return result;
}

std::string tape_title(const tradematch::MarketDataFeed& feed, tradematch::SymbolId symbol) {
    std::string title = " TRADES";
    if (const auto vwap = feed.vwap(symbol)) {
        std::ostringstream average;
        average << std::fixed << std::setprecision(2) << *vwap / 100.0;
        title += "   vol " + compact(feed.traded_volume(symbol)) + "   vwap " + average.str();
    }
    return title;
}

std::string_view phase_style(tradematch::SessionPhase phase) {
    switch (phase) {
        case tradematch::SessionPhase::Continuous:
            return "1;30;42";
        case tradematch::SessionPhase::OpeningAuction:
        case tradematch::SessionPhase::ClosingAuction:
            return "1;30;43";
        case tradematch::SessionPhase::Closed:
            return "1;37;100";
    }
    return "0";
}

std::vector<Line> render(const tradematch::MarketSimulator& sim) {
    const auto symbol = sim.symbol(0);
    const auto& feed = sim.feed();
    const auto& book = sim.engine().book(symbol);
    const auto last = feed.last_price(symbol);
    // Change against yesterday's close, or against today's opening auction on the first day.
    const auto reference = sim.previous_close(0) ? sim.previous_close(0) : sim.day_open(0);
    std::vector<Line> lines(kHeight);

    // Title bar.
    Line& title = lines[0];
    title.text(" TradeMatch ", "1;97;44")
        .text(" " + std::string{sim.engine().symbol_name(symbol)} + " ", "1;97;100")
        .text(" " + std::string{tradematch::to_string(sim.session())} + " " +
                  std::to_string(sim.phase_time_remaining() / 1'000'000'000) + "s ",
              phase_style(sim.session()))
        .text(" day " + std::to_string(sim.day()) + "  " + clock_time(sim.now()) + " ", "97;100");
    if (last) {
        title.text("  last ", "37").text(format_price(*last), "1;97");
        if (reference && *reference != 0) {
            const auto change = *last - *reference;
            std::ostringstream percent;
            percent << std::fixed << std::setprecision(2)
                    << 100.0 * static_cast<double>(change) / static_cast<double>(*reference);
            const bool up = change >= 0;
            title.text(std::string{up ? " ▲ +" : " ▼ "} + format_price(change) + " (" + percent.str() + "%)",
                       up ? "1;32" : "1;31");
        }
    }
    title.pad_to(kWidth);

    // Order book panel (left) and trade tape (right).
    constexpr std::size_t kBookWidth = 54;
    constexpr std::size_t kTapeWidth = kWidth - kBookWidth - 1;
    lines[1]
        .text(left(" ORDER BOOK", kBookWidth), "1;97;48;5;236")
        .text(" ")
        .text(left(tape_title(feed, symbol), kTapeWidth), "1;97;48;5;236");
    lines[2]
        .text(left("   price        size   orders", kBookWidth), "2")
        .text(" ")
        .text(left("   time          price      size", kTapeWidth), "2");

    const auto asks = feed.depth(symbol).levels(Side::Sell, kBookLevels);
    const auto bids = feed.depth(symbol).levels(Side::Buy, kBookLevels);
    std::uint64_t largest = 1;
    for (const auto* side_levels : {&asks, &bids}) {
        for (const auto& level : *side_levels) {
            largest = std::max<std::uint64_t>(largest, level.quantity);
        }
    }
    const auto book_row = [&](Line& line, const tradematch::DepthLevel* level, bool ask) {
        if (level == nullptr) {
            line.pad_to(kBookWidth);
            return;
        }
        line.text("  ")
            .text(right(format_price(level->price), 8), ask ? "1;31" : "1;32")
            .text(right(with_commas(level->quantity), 11), "97")
            .text(right(std::to_string(level->order_count), 8), "37")
            .text("  ")
            .text(bar(level->quantity, largest, 20), ask ? "31" : "32")
            .pad_to(kBookWidth);
    };
    for (std::size_t row = 0; row < kBookLevels; ++row) {
        // Asks are drawn from the tenth level down to the best, so the spread sits in the middle of the panel.
        const std::size_t level_index = kBookLevels - 1 - row;
        book_row(lines[3 + row], level_index < asks.size() ? &asks[level_index] : nullptr, true);
    }
    Line& middle = lines[3 + kBookLevels];
    if (const auto indicative = book.indicative_auction()) {
        std::string imbalance = "balanced";
        if (indicative->buy_surplus > 0U) {
            imbalance = "buy surplus " + compact(indicative->buy_surplus);
        } else if (indicative->sell_surplus > 0U) {
            imbalance = "sell surplus " + compact(indicative->sell_surplus);
        }
        middle.text(left("  ◆ indicative " + format_price(indicative->price) + " x " +
                             compact(indicative->matched_quantity) + "  " + imbalance,
                         kBookWidth),
                    "1;33");
    } else if (!asks.empty() && !bids.empty()) {
        const Price spread = asks.front().price - bids.front().price;
        middle.text(left("  ─── spread " + format_price(spread) + " ─── mid " +
                             format_price((asks.front().price + bids.front().price) / 2) + " ───",
                         kBookWidth),
                    "2");
    } else {
        middle.pad_to(kBookWidth);
    }
    for (std::size_t row = 0; row < kBookLevels; ++row) {
        book_row(lines[4 + kBookLevels + row], row < bids.size() ? &bids[row] : nullptr, false);
    }

    const auto& tape = feed.recent_trades(symbol);
    constexpr std::size_t kTapeRows = 2 * kBookLevels + 1;
    std::size_t row = 0;
    for (auto entry = tape.rbegin(); entry != tape.rend() && row < kTapeRows; ++entry, ++row) {
        const auto& trade = entry->trade;
        Line& line = lines[3 + row];
        line.text(" ");
        std::string arrow = trade.aggressor_side == Side::Buy ? "▲" : "▼";
        std::string_view style = trade.aggressor_side == Side::Buy ? "1;32" : "1;31";
        if (trade.auction) {
            arrow = "◆ auction";
            style = "1;33";
        }
        line.text(left("  " + clock_time(entry->timestamp), 14), "2")
            .text(right(format_price(trade.price), 8), style)
            .text(right(with_commas(trade.quantity), 10), "97")
            .text("  ")
            .text(arrow, style);
    }

    // Candle chart.
    const std::size_t chart_top = 5 + 2 * kBookLevels;
    lines[chart_top - 1].text(left(" PRICE  (1s candles)", kWidth), "1;97;48;5;236");
    const auto& candles = feed.candles(symbol).candles();
    constexpr std::size_t kChartWidth = kWidth - 12;
    const std::size_t first = candles.size() > kChartWidth ? candles.size() - kChartWidth : 0;
    Price low = 0;
    Price high = 1;
    if (first < candles.size()) {
        low = candles[first].low;
        high = candles[first].high;
        for (std::size_t index = first + 1; index < candles.size(); ++index) {
            low = std::min(low, candles[index].low);
            high = std::max(high, candles[index].high);
        }
        if (high == low) {
            ++high;
        }
    }
    // Each text row holds two price bands drawn with half blocks, doubling the vertical resolution of bodies.
    const double band = static_cast<double>(high - low) / static_cast<double>(2 * kChartHeight);
    const auto covers = [&](double bottom, double top, std::size_t half) {
        const double band_top = static_cast<double>(high) - band * static_cast<double>(half);
        const double band_bottom = band_top - band;
        return top >= band_bottom && bottom <= band_top;
    };
    for (std::size_t chart_row = 0; chart_row < kChartHeight; ++chart_row) {
        Line& line = lines[chart_top + chart_row];
        line.text(" ");
        for (std::size_t index = first; index < candles.size(); ++index) {
            const auto& candle = candles[index];
            const auto body_top = static_cast<double>(std::max(candle.open, candle.close));
            const auto body_bottom = static_cast<double>(std::min(candle.open, candle.close));
            const auto candle_low = static_cast<double>(candle.low);
            const auto candle_high = static_cast<double>(candle.high);
            const bool upper = covers(body_bottom, body_top, 2 * chart_row);
            const bool lower = covers(body_bottom, body_top, 2 * chart_row + 1);
            const bool wick =
                covers(candle_low, candle_high, 2 * chart_row) || covers(candle_low, candle_high, 2 * chart_row + 1);
            const std::string_view color = candle.close >= candle.open ? "32" : "31";
            if (upper && lower) {
                line.text("█", color);
            } else if (upper) {
                line.text("▀", color);
            } else if (lower) {
                line.text("▄", color);
            } else if (wick) {
                line.text("│", color);
            } else {
                line.text(" ");
            }
        }
        line.pad_to(kChartWidth + 2);
        if (!candles.empty() && chart_row == 0) {
            line.text(format_price(high), "2");
        } else if (!candles.empty() && chart_row == kChartHeight - 1) {
            line.text(format_price(low), "2");
        }
        line.pad_to(kWidth);
    }

    // Status bar.
    const auto& stats = sim.stats();
    const auto& latency = sim.request_latency();
    Line& status = lines[kHeight - 1];
    status
        .text(" requests " + compact(stats.requests) + "  trades " + compact(stats.trades) + "  resting " +
                  with_commas(book.order_count()) + "  stops " + std::to_string(book.stop_order_count()) +
                  "  auctions " + std::to_string(stats.auctions) + "  STP " + compact(stats.self_trade_cancels) +
                  "  lat p50 " + latency_text(latency.percentile(50.0)) + " p99 " +
                  latency_text(latency.percentile(99.0)) + " ",
              "97;48;5;238")
        .pad_to(kWidth, "48;5;238");
    return lines;
}

std::string compose(const std::vector<Line>& lines) {
    std::string frame = "\x1b[H";
    for (std::size_t index = 0; index < lines.size(); ++index) {
        frame += lines[index].str();
        frame += "\x1b[0m\x1b[K";
        if (index + 1 < lines.size()) {
            frame += "\r\n";
        }
    }
    return frame;
}

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const char character : text) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\x1b':
                escaped += "\\u001b";
                break;
            default:
                escaped += character;
        }
    }
    return escaped;
}

bool stdout_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void prepare_terminal() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode) != 0) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
}

double parse_double(std::string_view flag, const std::string& value) {
    const auto invalid = [&] {
        return std::invalid_argument("expected a positive number for " + std::string{flag});
    };
    std::size_t consumed = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(value, &consumed);
    } catch (const std::exception&) {
        throw invalid();
    }
    if (consumed != value.size() || parsed <= 0.0) {
        throw invalid();
    }
    return parsed;
}

std::uint64_t parse_count(std::string_view flag, const std::string& value) {
    std::uint64_t parsed = 0;
    const auto* const last = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), last, parsed);
    if (value.empty() || error != std::errc{} || stop != last) {
        throw std::invalid_argument("expected a non-negative integer for " + std::string{flag});
    }
    return parsed;
}

// Returns nothing when --help was printed.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--no-sleep") {
            options.sleep = false;
            continue;
        }
        if (arg == "--headless") {
            options.headless = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tradematch_tui [--seed N] [--speed X] [--fps N] [--frames N] [--record FILE.cast]\n"
                         "                      [--journal FILE] [--no-sleep] [--headless]\n"
                         "Press Ctrl+C to quit.\n";
            return std::nullopt;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const auto& value = args[++index];
        if (arg == "--seed") {
            options.seed = parse_count(arg, value);
        } else if (arg == "--speed") {
            options.speed = parse_double(arg, value);
        } else if (arg == "--fps") {
            options.fps = parse_double(arg, value);
        } else if (arg == "--frames") {
            options.frames = parse_count(arg, value);
        } else if (arg == "--record") {
            options.record_path = value;
        } else if (arg == "--journal") {
            options.journal_path = value;
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    return options;
}

int run(int argc, char** argv) {
    const auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return 0;
    }
    const Options& options = *parsed;
    tradematch::SimulationConfig config;
    config.seed = options.seed;
    config.opening_auction = 8'000'000'000;  // long enough to watch the indicative price form
    config.closing_auction = 8'000'000'000;
    std::unique_ptr<tradematch::JournalWriter> journal;
    if (!options.journal_path.empty()) {
        tradematch::JournalOptions journal_options;
        journal_options.truncate = true;  // a new session starts from an empty engine
        journal = std::make_unique<tradematch::JournalWriter>(options.journal_path, journal_options);
    }
    tradematch::MarketSimulator sim(config, journal.get());

    std::ofstream cast;
    if (!options.record_path.empty()) {
        cast.open(options.record_path);
        if (!cast) {
            throw std::runtime_error("cannot write " + options.record_path);
        }
        cast << R"({"version": 2, "width": )" << kWidth << R"(, "height": )" << kHeight
             << R"(, "env": {"TERM": "xterm-256color", "SHELL": "/bin/zsh"}})" << '\n';
    }

    const bool draw = !options.headless;
    const bool terminal = draw && stdout_is_terminal();
    if (draw) {
        prepare_terminal();
        if (terminal) {
            std::cout << "\x1b[?1049h\x1b[?25l\x1b[2J" << std::flush;
        }
    }
    std::signal(SIGINT, tradematch_tui_interrupt);

    const auto frame_interval = std::chrono::duration<double>(1.0 / options.fps);
    const auto sim_per_frame = static_cast<Timestamp>(1e9 * options.speed / options.fps);
    auto next_frame = std::chrono::steady_clock::now();
    for (std::uint64_t frame = 0; !g_stop && (options.frames == 0 || frame < options.frames); ++frame) {
        sim.run_for(sim_per_frame);
        const std::string output = compose(render(sim));
        if (draw) {
            std::cout << output << std::flush;
        }
        if (cast.is_open()) {
            const double elapsed = static_cast<double>(frame) / options.fps;
            cast << '[' << std::fixed << std::setprecision(3) << elapsed << R"(, "o", ")" << json_escape(output)
                 << "\"]\n";
        }
        if (options.sleep) {
            next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_interval);
            std::this_thread::sleep_until(next_frame);
        }
    }

    if (terminal) {
        std::cout << "\x1b[0m\x1b[?25h\x1b[?1049l" << std::flush;
    }
    const auto& stats = sim.stats();
    std::cout << "simulated " << clock_time(sim.now()) << " over " << sim.day()
              << " day(s): " << with_commas(stats.requests) << " requests, " << with_commas(stats.trades) << " trades, "
              << stats.auctions << " auctions\n";
    if (journal) {
        journal->close();
        std::cout << "journal " << options.journal_path << ": " << with_commas(journal->last_sequence())
                  << " commands, " << with_commas(journal->size_bytes()) << " bytes\n";
    }
    std::cout << "state hash 0x" << std::hex << std::setw(16) << std::setfill('0')
              << tradematch::state_hash(sim.engine()) << std::dec << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cout << "\x1b[0m\x1b[?25h";
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
