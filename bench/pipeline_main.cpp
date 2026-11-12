// Measures the multi-threaded pipeline end to end. Requests travel from the gateway thread (this one) through the
// matcher thread to the publisher thread, and each request's latency runs from the moment it was due to be sent to the
// moment the publisher receives its report. Paced runs time from the intended send time rather than the actual one,
// so a stalled gateway shows up as latency instead of hiding it (no coordinated omission).
#include "system_info.hpp"
#include "tradematch/detail/spin.hpp"
#include "tradematch/latency_histogram.hpp"
#include "tradematch/pipeline.hpp"
#include "tradematch/snapshot.hpp"
#include "workload.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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

using tradematch::LatencyHistogram;
using tradematch::PipelineEvent;
using tradematch::PipelineRequest;
using tradematch::Timestamp;
using tradematch::bench::CommandKind;

struct Options {
    tradematch::bench::WorkloadConfig workload;
    double rate{0.0};  // requests per second during the measured segment; 0 sends as fast as possible
    std::string journal_path;
    bool market_data{true};
    bool share_cores{false};  // let the pipeline threads yield their cores when idle
    std::size_t queue_capacity{std::size_t{1} << 16};
};

Timestamp now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
    } else if (nanoseconds < 1'000'000) {
        out << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / 1e3 << " us";
    } else {
        out << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / 1e6 << " ms";
    }
    return out.str();
}

std::uint64_t parse_number(std::string_view flag, const std::string& value) {
    std::uint64_t parsed = 0;
    const auto* const last = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), last, parsed);
    if (value.empty() || error != std::errc{} || stop != last) {
        throw std::invalid_argument("expected a non-negative integer for " + std::string{flag});
    }
    return parsed;
}

void print_usage() {
    std::cout << "usage: tradematch_pipeline_benchmark [--ops N] [--rate REQUESTS_PER_SECOND] [--journal FILE]\n"
                 "                                     [--no-market-data] [--share-cores] [--queue N] [--seed S]\n"
                 "                                     [--warmup N] [--prefill N]\n"
                 "\n"
                 "  --rate R         pace the measured requests at R per second (default: as fast as possible)\n"
                 "  --share-cores    let idle pipeline threads yield their cores instead of spinning\n";
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
        if (arg == "--no-market-data") {
            options.market_data = false;
            continue;
        }
        if (arg == "--share-cores") {
            options.share_cores = true;
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const auto& value = args[++index];
        if (arg == "--ops") {
            options.workload.measured_commands = parse_number(arg, value);
        } else if (arg == "--rate") {
            options.rate = static_cast<double>(parse_number(arg, value));
        } else if (arg == "--journal") {
            options.journal_path = value;
        } else if (arg == "--queue") {
            options.queue_capacity = parse_number(arg, value);
        } else if (arg == "--seed") {
            options.workload.seed = parse_number(arg, value);
        } else if (arg == "--warmup") {
            options.workload.warmup_commands = parse_number(arg, value);
        } else if (arg == "--prefill") {
            options.workload.prefill_orders = parse_number(arg, value);
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    return options;
}

PipelineRequest to_request(const tradematch::bench::Command& command, std::uint64_t request_id) {
    PipelineRequest request;
    request.request_id = request_id;
    request.order = command.order;
    switch (command.kind) {
        case CommandKind::Cancel:
            request.type = tradematch::CommandType::Cancel;
            break;
        case CommandKind::Replace:
            request.type = tradematch::CommandType::Replace;
            break;
        case CommandKind::Add:
        case CommandKind::Aggress:
            request.type = tradematch::CommandType::Submit;
            break;
    }
    return request;
}

// Filled in on the publisher thread; read by the gateway only through `reports`, or after finish().
struct Measurements {
    std::uint64_t measured_after{0};  // requests with higher IDs are measured
    std::atomic<std::uint64_t> reports{0};
    LatencyHistogram end_to_end;
    LatencyHistogram to_matcher;
    std::uint64_t trades{0};
    std::uint64_t level_updates{0};
    Timestamp last_report_at{0};
};

void print_latency(std::string_view label, const LatencyHistogram& histogram, std::string_view note) {
    constexpr std::array<double, 5> kPercentiles{50.0, 90.0, 99.0, 99.9, 99.99};
    constexpr std::array<std::string_view, 5> kLabels{"p50", "p90", "p99", "p99.9", "p99.99"};
    std::cout << "  " << std::left << std::setw(11) << label << std::right;
    for (std::size_t index = 0; index < kPercentiles.size(); ++index) {
        std::cout << kLabels.at(index) << ' ' << format_ns(histogram.percentile(kPercentiles.at(index))) << "  ";
    }
    std::cout << "max " << format_ns(histogram.max()) << "  (" << note << ")\n";
}

int run(int argc, char** argv) {
    const auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return 0;
    }
    const Options& options = *parsed;
    const auto workload = tradematch::bench::generate_workload(options.workload);
    const auto total = workload.commands.size();
    const auto measured = total - workload.measured_begin;

    tradematch::PipelineConfig config;
    config.symbols = {"BENCH"};
    config.capacity.orders = std::max<std::size_t>(options.workload.prefill_orders * 2, 4'096);
    config.capacity.price_levels = 4'096;
    config.capacity.trades_per_call = 1'024;
    config.queue_capacity = options.queue_capacity;
    config.market_data = options.market_data;
    config.wait = options.share_cores ? tradematch::WaitStrategy::SpinThenYield : tradematch::WaitStrategy::Spin;
    if (!options.journal_path.empty()) {
        config.journal = options.journal_path;
    }

    Measurements measurements;
    measurements.measured_after = workload.measured_begin;
    tradematch::Pipeline pipeline(config, [&measurements](const PipelineEvent& event) {
        switch (event.kind) {
            case PipelineEvent::Kind::Trade:
                ++measurements.trades;
                return;
            case PipelineEvent::Kind::LevelUpdate:
                ++measurements.level_updates;
                return;
            case PipelineEvent::Kind::Report:
                break;
        }
        const Timestamp received = now_ns();
        if (event.request_id > measurements.measured_after) {
            measurements.end_to_end.record(
                static_cast<std::uint64_t>(std::max<Timestamp>(0, received - event.sent_at)));
            measurements.to_matcher.record(
                static_cast<std::uint64_t>(std::max<Timestamp>(0, event.matched_at - event.sent_at)));
        }
        measurements.last_report_at = received;
        measurements.reports.fetch_add(1, std::memory_order_release);
    });

    tradematch::detail::prefer_performance_cores();  // the gateway is this thread

    // Prefill and warm-up go through unpaced; measuring starts once they have been fully processed.
    for (std::size_t index = 0; index < workload.measured_begin; ++index) {
        auto request = to_request(workload.commands[index], index + 1);
        request.sent_at = now_ns();
        pipeline.send(request);
    }
    while (measurements.reports.load(std::memory_order_acquire) < workload.measured_begin) {
        tradematch::detail::cpu_relax();
    }

    const double period_ns = options.rate > 0.0 ? 1e9 / options.rate : 0.0;
    const Timestamp start = now_ns();
    for (std::size_t index = workload.measured_begin; index < total; ++index) {
        auto request = to_request(workload.commands[index], index + 1);
        if (period_ns > 0.0) {
            const auto due =
                start + static_cast<Timestamp>(static_cast<double>(index - workload.measured_begin) * period_ns);
            while (now_ns() < due) {
                tradematch::detail::cpu_relax();
            }
            request.sent_at = due;
        } else {
            request.sent_at = now_ns();
        }
        pipeline.send(request);
    }
    pipeline.finish();
    const auto elapsed = static_cast<double>(measurements.last_report_at - start);

    // The pipeline must have produced exactly the books a single thread produces from the same requests.
    tradematch::MatchingEngine direct;
    direct.add_symbol("BENCH", config.capacity);
    for (const auto& command : workload.commands) {
        if (command.kind == CommandKind::Cancel) {
            direct.cancel(0, command.order.order_id);
        } else if (command.kind == CommandKind::Replace) {
            direct.replace(0, command.order);
        } else {
            direct.submit(0, command.order);
        }
    }
    const bool verified = tradematch::state_hash(direct) == tradematch::state_hash(pipeline.engine());

    const auto& stats = pipeline.stats();
    std::cout << "TradeMatch pipeline benchmark\n";
    std::cout << "  host       " << tradematch::bench::cpu_name() << " | " << tradematch::bench::compiler_name()
              << " | " << (tradematch::bench::optimized_build() ? "optimized build" : "UNOPTIMIZED build") << '\n';
    std::cout << "  workload   seed " << options.workload.seed << " | " << with_commas(options.workload.prefill_orders)
              << " prefilled | " << with_commas(options.workload.warmup_commands) << " warm-up | "
              << with_commas(measured) << " measured\n";
    std::cout << "  pipeline   gateway -> matcher -> publisher | queues of " << with_commas(options.queue_capacity)
              << " | market data " << (options.market_data ? "on" : "off") << " | journal "
              << (options.journal_path.empty() ? "off" : "on (background writes)") << " | idle threads "
              << (options.share_cores ? "yield" : "spin") << '\n';
    std::cout << "  rate       "
              << (options.rate > 0.0 ? with_commas(static_cast<std::uint64_t>(options.rate)) + " requests/s, paced"
                                     : std::string{"unpaced: as fast as the gateway can send"})
              << "\n\n";
    if (elapsed > 0.0) {
        std::cout << "  throughput " << std::fixed << std::setprecision(2)
                  << static_cast<double>(measured) * 1e3 / elapsed << " M requests/s, end to end\n";
    }
    print_latency("latency", measurements.end_to_end, "send to report at the publisher");
    print_latency("matched", measurements.to_matcher, "send to matched");
    std::cout << "  events     " << with_commas(stats.events) << " (" << with_commas(measurements.trades) << " trades, "
              << with_commas(measurements.level_updates) << " level updates, " << with_commas(stats.requests)
              << " reports)\n";
    std::cout << "  waits      gateway " << with_commas(stats.gateway_waits) << " | matcher "
              << with_commas(stats.matcher_waits) << " (sends that found the next queue full)\n";
    if (!options.journal_path.empty()) {
        std::cout << "  journal    " << options.journal_path << " | "
                  << with_commas(std::filesystem::file_size(options.journal_path)) << " bytes\n";
    }
    std::cout << "  verified   "
              << (verified ? "the pipeline's book matches a single-threaded replay"
                           : "MISMATCH against a single-threaded replay")
              << '\n';
    return verified ? 0 : 1;
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
