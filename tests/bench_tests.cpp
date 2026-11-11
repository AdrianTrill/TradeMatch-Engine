#include "tradematch/latency_histogram.hpp"
#include "workload.hpp"

#include "tradematch/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tradematch::LatencyHistogram;
using tradematch::bench::CommandKind;
using tradematch::bench::WorkloadConfig;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

std::uint64_t exact_percentile(std::vector<std::uint64_t> values, double percent) {
    std::ranges::sort(values);
    const auto rank = static_cast<std::size_t>(std::ceil(percent / 100.0 * static_cast<double>(values.size())));
    return values.at(std::max<std::size_t>(rank, 1) - 1);
}

void test_histogram_is_exact_for_small_values() {
    auto histogram = std::make_unique<LatencyHistogram>();
    for (std::uint64_t value = 1; value <= 200; ++value) {
        histogram->record(value);
    }
    expect(histogram->count() == 200, "Every sample should be counted.");
    expect(histogram->min() == 1 && histogram->max() == 200, "Min and max should be exact.");
    expect(histogram->percentile(50.0) == 100, "The median of 1..200 should be exactly 100.");
    expect(histogram->percentile(99.0) == 198, "p99 of 1..200 should be exactly 198.");
    expect(histogram->percentile(100.0) == 200, "p100 should be the maximum.");
}

void test_histogram_percentiles_stay_within_error_bound() {
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> distribution(5.0, 1.2);  // median ~150 ns with a long tail

    auto histogram = std::make_unique<LatencyHistogram>();
    std::vector<std::uint64_t> values;
    for (int sample = 0; sample < 200'000; ++sample) {
        const auto value = static_cast<std::uint64_t>(distribution(rng)) + 1;
        values.push_back(value);
        histogram->record(value);
    }

    for (const double percent : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        const auto exact = static_cast<double>(exact_percentile(values, percent));
        const auto reported = static_cast<double>(histogram->percentile(percent));
        expect(reported >= exact, "Reported percentiles should never understate the true value.");
        expect(reported <= exact * (1.0 + 1.0 / 128.0) + 1.0, "Reported percentiles should be within 1/128.");
    }
}

void test_histogram_merge_matches_combined_recording() {
    auto left = std::make_unique<LatencyHistogram>();
    auto right = std::make_unique<LatencyHistogram>();
    auto combined = std::make_unique<LatencyHistogram>();
    for (std::uint64_t value = 1; value < 5'000; value += 7) {
        (value % 2 == 0 ? *left : *right).record(value);
        combined->record(value);
    }
    left->merge(*right);
    expect(left->count() == combined->count(), "Merged counts should add up.");
    for (const double percent : {1.0, 50.0, 99.0, 100.0}) {
        expect(left->percentile(percent) == combined->percentile(percent), "Merged percentiles should match.");
    }
}

void test_empty_histogram_reports_zero() {
    const auto histogram = std::make_unique<LatencyHistogram>();
    expect(histogram->percentile(99.0) == 0 && histogram->max() == 0 && histogram->min() == 0,
           "An empty histogram should report zeros.");
    expect(histogram->mean() == 0.0 && histogram->fraction_below(100) == 0.0, "An empty histogram has no mass.");
}

WorkloadConfig small_config(std::uint64_t seed) {
    WorkloadConfig config;
    config.seed = seed;
    config.prefill_orders = 2'000;
    config.warmup_commands = 5'000;
    config.measured_commands = 50'000;
    return config;
}

void test_workload_is_deterministic() {
    const auto first = tradematch::bench::generate_workload(small_config(11));
    const auto second = tradematch::bench::generate_workload(small_config(11));
    const auto other = tradematch::bench::generate_workload(small_config(12));

    expect(first.commands.size() == 57'000, "Prefill, warm-up, and measured commands should all be generated.");
    expect(first.measured_begin == 7'000, "Measurement should start after prefill and warm-up.");

    const auto same_command = [](const auto& left, const auto& right) {
        return left.kind == right.kind && left.order.order_id == right.order.order_id &&
               left.order.side == right.order.side && left.order.type == right.order.type &&
               left.order.quantity == right.order.quantity && left.order.price == right.order.price;
    };
    expect(std::ranges::equal(first.commands, second.commands, same_command),
           "The same seed should produce an identical stream.");
    expect(!std::ranges::equal(first.commands, other.commands, same_command),
           "A different seed should produce a different stream.");

    std::size_t mix_total = 0;
    for (const auto count : first.measured_mix) {
        mix_total += count;
    }
    expect(mix_total == 50'000, "The measured mix should account for every measured command.");
}

void test_workload_replays_without_rejections() {
    const auto workload = tradematch::bench::generate_workload(small_config(3));
    tradematch::OrderBook book;

    for (const auto& command : workload.commands) {
        switch (command.kind) {
            case CommandKind::Cancel:
                expect(book.cancel(command.order.order_id).cancelled, "Cancels should target live orders.");
                break;
            case CommandKind::Replace:
                expect(book.replace(command.order).replaced, "Replaces should target live orders.");
                break;
            case CommandKind::Add: {
                const auto result = book.submit(command.order);
                expect(result.accepted && result.trades.empty(), "Passive adds should rest without trading.");
                break;
            }
            case CommandKind::Aggress:
                expect(book.submit(command.order).accepted, "Aggressive orders should be accepted.");
                break;
        }
    }
    expect(book.order_count() > 1'000, "The book should stay populated near its target depth.");
    expect(book.order_count() < 3'000, "The book should not grow without bound.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"histogram_is_exact_for_small_values", test_histogram_is_exact_for_small_values},
        {"histogram_percentiles_stay_within_error_bound", test_histogram_percentiles_stay_within_error_bound},
        {"histogram_merge_matches_combined_recording", test_histogram_merge_matches_combined_recording},
        {"empty_histogram_reports_zero", test_empty_histogram_reports_zero},
        {"workload_is_deterministic", test_workload_is_deterministic},
        {"workload_replays_without_rejections", test_workload_replays_without_rejections},
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
        std::cout << "All benchmark tooling tests passed (" << tests.size() << ").\n";
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
