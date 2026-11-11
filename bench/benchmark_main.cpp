#include "latency_histogram.hpp"
#include "system_info.hpp"
#include "workload.hpp"

#include "tradematch/order_book.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using tradematch::bench::Command;
using tradematch::bench::CommandKind;
using tradematch::bench::kCommandKindCount;
using tradematch::bench::kCommandKindNames;
using tradematch::bench::LatencyHistogram;
using tradematch::bench::Workload;
using tradematch::bench::WorkloadConfig;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kEngineName = "v1: std::map price levels + std::list FIFO queues";

struct Options {
    WorkloadConfig workload;
    std::string json_path;
    bool show_distribution{true};
};

struct ClockProfile {
    std::uint64_t resolution_ns{0};
    double read_cost_ns{0.0};
};

struct EngineReport {
    std::string engine;
    std::array<LatencyHistogram, kCommandKindCount> by_kind{};
    LatencyHistogram all;
    std::uint64_t trades{0};
    std::size_t resting_orders{0};
    double throughput_ops_per_second{0.0};
};

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

std::uint64_t parse_number(std::string_view flag, const std::string& value) {
    std::uint64_t parsed = 0;
    const auto* const last = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), last, parsed);
    if (value.empty() || error != std::errc{} || stop != last) {
        throw std::invalid_argument("expected a non-negative integer for " + std::string{flag} + ", got '" + value +
                                    "'");
    }
    return parsed;
}

void print_usage() {
    std::cout << "usage: tradematch_benchmark [measured_ops] [--ops N] [--warmup N] [--prefill N] [--seed S]\n"
                 "                            [--json FILE] [--no-distribution]\n";
}

// Returns false when the program should exit immediately (after --help).
bool parse_options(int argc, char** argv, Options& options) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--no-distribution") {
            options.show_distribution = false;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            options.workload.measured_commands = parse_number("measured_ops", arg);
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const auto& value = args[++index];
        if (arg == "--ops") {
            options.workload.measured_commands = parse_number(arg, value);
        } else if (arg == "--warmup") {
            options.workload.warmup_commands = parse_number(arg, value);
        } else if (arg == "--prefill") {
            options.workload.prefill_orders = parse_number(arg, value);
        } else if (arg == "--seed") {
            options.workload.seed = parse_number(arg, value);
        } else if (arg == "--json") {
            options.json_path = value;
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    return true;
}

ClockProfile profile_clock() {
    ClockProfile profile;

    constexpr std::size_t kSamples = 200'000;
    std::uint64_t resolution = 0;
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
        const auto first = Clock::now();
        auto second = Clock::now();
        while (second == first) {
            second = Clock::now();
        }
        const auto delta = elapsed_ns(first, second);
        resolution = resolution == 0 ? delta : std::min(resolution, delta);
    }
    profile.resolution_ns = resolution;

    constexpr std::size_t kReads = 2'000'000;
    const auto start = Clock::now();
    auto last = start;
    for (std::size_t read_index = 0; read_index < kReads; ++read_index) {
        last = Clock::now();
    }
    profile.read_cost_ns = static_cast<double>(elapsed_ns(start, last)) / static_cast<double>(kReads);
    return profile;
}

template <typename Book>
std::size_t apply(Book& book, const Command& command) {
    switch (command.kind) {
        case CommandKind::Cancel:
            book.cancel(command.order.order_id);
            return 0;
        case CommandKind::Replace:
            return book.replace(command.order).submit_result.trades.size();
        case CommandKind::Add:
        case CommandKind::Aggress:
            return book.submit(command.order).trades.size();
    }
    return 0;
}

template <typename Book>
std::unique_ptr<EngineReport> measure(const Workload& workload, std::string_view engine_name) {
    auto report = std::make_unique<EngineReport>();
    report->engine = std::string{engine_name};

    {
        // Latency pass: every command is timed individually; prefill and warm-up samples are discarded.
        Book book;
        for (std::size_t index = 0; index < workload.commands.size(); ++index) {
            const auto& command = workload.commands[index];
            const auto start = Clock::now();
            const auto trades = apply(book, command);
            const auto end = Clock::now();
            if (index >= workload.measured_begin) {
                const auto latency = elapsed_ns(start, end);
                report->by_kind.at(static_cast<std::size_t>(command.kind)).record(latency);
                report->all.record(latency);
                report->trades += trades;
            }
        }
        report->resting_orders = book.order_count();
    }

    {
        // Throughput pass: the same stream on a fresh book with no clock reads inside the measured segment.
        Book book;
        for (std::size_t index = 0; index < workload.measured_begin; ++index) {
            apply(book, workload.commands[index]);
        }
        std::size_t sink = 0;
        const auto start = Clock::now();
        for (std::size_t index = workload.measured_begin; index < workload.commands.size(); ++index) {
            sink += apply(book, workload.commands[index]);
        }
        const auto elapsed = elapsed_ns(start, Clock::now());
        const auto measured = workload.commands.size() - workload.measured_begin;
        if (elapsed > 0) {
            report->throughput_ops_per_second = static_cast<double>(measured) * 1e9 / static_cast<double>(elapsed);
        }
        if (sink != report->trades) {
            throw std::runtime_error("throughput replay diverged from the latency pass");
        }
    }
    return report;
}

std::string with_commas(std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), ",");
    }
    return digits;
}

std::string format_ns(std::uint64_t nanoseconds) {
    std::ostringstream out;
    if (nanoseconds < 10'000) {
        out << with_commas(nanoseconds) << " ns";
    } else if (nanoseconds < 10'000'000) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / 1e3 << " us";
    } else {
        out << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / 1e6 << " ms";
    }
    return out.str();
}

std::string percent(double fraction) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << fraction * 100.0 << '%';
    return out.str();
}

constexpr std::array<double, 5> kPercentiles{50.0, 90.0, 99.0, 99.9, 99.99};
constexpr std::array<std::string_view, 5> kPercentileLabels{"p50", "p90", "p99", "p99.9", "p99.99"};

void print_latency_row(std::string_view name, const LatencyHistogram& histogram) {
    std::cout << "  " << std::left << std::setw(9) << name << std::right << std::setw(11)
              << with_commas(histogram.count());
    for (const auto percentile : kPercentiles) {
        std::cout << std::setw(10) << format_ns(histogram.percentile(percentile));
    }
    std::cout << std::setw(10) << format_ns(histogram.max()) << std::setw(10)
              << format_ns(static_cast<std::uint64_t>(std::llround(histogram.mean()))) << '\n';
}

void print_distribution(const LatencyHistogram& histogram) {
    constexpr std::array<std::uint64_t, 9> kEdges{50, 100, 150, 200, 300, 500, 1'000, 2'000, 5'000};
    constexpr double kBarWidth = 50.0;

    std::cout << "\n  latency distribution, all operations\n";
    double previous = 0.0;
    for (std::size_t index = 0; index <= kEdges.size(); ++index) {
        const double cumulative = index < kEdges.size() ? histogram.fraction_below(kEdges.at(index)) : 1.0;
        const double share = cumulative - previous;
        previous = cumulative;

        std::string label;
        if (index == 0) {
            label = "< " + with_commas(kEdges.front()) + " ns";
        } else if (index == kEdges.size()) {
            label = ">= " + with_commas(kEdges.back()) + " ns";
        } else {
            label = with_commas(kEdges.at(index - 1)) + "-" + with_commas(kEdges.at(index)) + " ns";
        }
        const auto bar = static_cast<std::size_t>(std::llround(share * kBarWidth));
        std::cout << "  " << std::right << std::setw(14) << label << " | " << std::left
                  << std::setw(static_cast<int>(kBarWidth)) << std::string(bar, '#') << ' ' << std::right
                  << std::setw(6) << percent(share) << '\n';
    }
}

void print_report(const Options& options,
                  const Workload& workload,
                  const ClockProfile& clock,
                  const EngineReport& report) {
    const auto& config = options.workload;
    const auto measured = static_cast<double>(config.measured_commands);

    std::cout << "TradeMatch latency benchmark\n";
    std::cout << "  engine     " << report.engine << '\n';
    std::cout << "  host       " << tradematch::bench::cpu_name() << " | " << tradematch::bench::compiler_name()
              << " | " << (tradematch::bench::optimized_build() ? "optimized build" : "UNOPTIMIZED build") << '\n';
    std::cout << "  workload   seed " << config.seed << " | " << with_commas(config.prefill_orders)
              << " resting orders prefilled | " << with_commas(config.warmup_commands) << " warm-up | "
              << with_commas(config.measured_commands) << " measured\n";
    std::cout << "  mix       ";
    for (std::size_t kind = 0; kind < kCommandKindCount; ++kind) {
        const auto share = measured > 0 ? static_cast<double>(workload.measured_mix.at(kind)) / measured : 0.0;
        std::cout << (kind == 0 ? " " : " | ") << kCommandKindNames.at(kind) << ' ' << percent(share);
    }
    std::cout << '\n';
    std::cout << "  clock      steady_clock | " << clock.resolution_ns << " ns resolution | " << std::fixed
              << std::setprecision(1) << clock.read_cost_ns << " ns per read (included in every sample)\n";
    if (!tradematch::bench::optimized_build()) {
        std::cout << "  WARNING    unoptimized build: numbers are not representative (use a Release build)\n";
    }

    std::cout << "\n  " << std::left << std::setw(9) << "operation" << std::right << std::setw(11) << "count";
    for (const auto label : kPercentileLabels) {
        std::cout << std::setw(10) << label;
    }
    std::cout << std::setw(10) << "max" << std::setw(10) << "mean" << '\n';
    for (std::size_t kind = 0; kind < kCommandKindCount; ++kind) {
        print_latency_row(kCommandKindNames.at(kind), report.by_kind.at(kind));
    }
    print_latency_row("all", report.all);

    if (options.show_distribution) {
        print_distribution(report.all);
    }

    std::cout << "\n  throughput " << std::fixed << std::setprecision(2) << report.throughput_ops_per_second / 1e6
              << " M ops/s (same stream replayed without per-operation clock reads)\n";
    std::cout << "  activity   " << with_commas(report.trades) << " trades | " << with_commas(report.resting_orders)
              << " orders resting at the end\n";
}

std::string json_escape(std::string_view text) {
    std::string escaped;
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            escaped += '\\';
        }
        escaped += character;
    }
    return escaped;
}

void write_histogram_json(std::ostream& out, const LatencyHistogram& histogram) {
    out << R"({"count": )" << histogram.count();
    for (std::size_t index = 0; index < kPercentiles.size(); ++index) {
        auto key = std::string{kPercentileLabels.at(index)};
        std::replace(key.begin(), key.end(), '.', '_');
        out << R"(, ")" << key << R"(": )" << histogram.percentile(kPercentiles.at(index));
    }
    out << R"(, "max": )" << histogram.max() << R"(, "mean": )" << std::fixed << std::setprecision(2)
        << histogram.mean() << '}';
}

void write_json(const std::string& path,
                const Options& options,
                const Workload& workload,
                const ClockProfile& clock,
                const std::vector<std::unique_ptr<EngineReport>>& reports) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }

    const auto& config = options.workload;
    out << "{\n";
    out << R"(  "benchmark": "tradematch-latency",)" << '\n';
    out << R"(  "cpu": ")" << json_escape(tradematch::bench::cpu_name()) << "\",\n";
    out << R"(  "compiler": ")" << json_escape(tradematch::bench::compiler_name()) << "\",\n";
    out << R"(  "optimized": )" << (tradematch::bench::optimized_build() ? "true" : "false") << ",\n";
    out << R"(  "clock": {"resolution_ns": )" << clock.resolution_ns << R"(, "read_cost_ns": )" << std::fixed
        << std::setprecision(2) << clock.read_cost_ns << "},\n";
    out << R"(  "workload": {"seed": )" << config.seed << R"(, "prefill_orders": )" << config.prefill_orders
        << R"(, "warmup_commands": )" << config.warmup_commands << R"(, "measured_commands": )"
        << config.measured_commands << R"(, "mix": {)";
    for (std::size_t kind = 0; kind < kCommandKindCount; ++kind) {
        out << (kind == 0 ? "" : ", ") << '"' << kCommandKindNames.at(kind) << R"(": )"
            << workload.measured_mix.at(kind);
    }
    out << "}},\n";
    out << R"(  "engines": [)" << '\n';
    for (std::size_t engine = 0; engine < reports.size(); ++engine) {
        const auto& report = *reports.at(engine);
        out << R"(    {"name": ")" << json_escape(report.engine) << R"(", "throughput_ops_per_sec": )" << std::fixed
            << std::setprecision(0) << report.throughput_ops_per_second << R"(, "trades": )" << report.trades
            << R"(, "resting_orders_at_end": )" << report.resting_orders << ",\n"
            << R"(     "latency_ns": {)";
        for (std::size_t kind = 0; kind < kCommandKindCount; ++kind) {
            out << "\n       \"" << kCommandKindNames.at(kind) << R"(": )";
            write_histogram_json(out, report.by_kind.at(kind));
            out << ',';
        }
        out << "\n" << R"(       "all": )";
        write_histogram_json(out, report.all);
        out << "}}" << (engine + 1 < reports.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
}

int run(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 0;
    }

    const auto workload = tradematch::bench::generate_workload(options.workload);
    const auto clock = profile_clock();

    std::vector<std::unique_ptr<EngineReport>> reports;
    reports.push_back(measure<tradematch::OrderBook>(workload, kEngineName));
    print_report(options, workload, clock, *reports.front());

    if (!options.json_path.empty()) {
        write_json(options.json_path, options, workload, clock, reports);
        std::cout << "  json       " << options.json_path << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
