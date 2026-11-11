#include "support/differential.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <string>

namespace {

using tradematch::testing::DifferentialHarness;
using tradematch::testing::Operation;
using tradematch::testing::OperationGenerator;

constexpr tradematch::BookCapacity kTinyCapacity{1, 1, 1};

struct Options {
    std::uint64_t first_seed{1};
    std::uint64_t seeds{200};
    std::uint64_t operations{2000};
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return false;
        }

        const auto value = std::stoull(argv[++index]);
        if (argument == "--seed") {
            options.first_seed = value;
            options.seeds = 1;
        } else if (argument == "--first-seed") {
            options.first_seed = value;
        } else if (argument == "--seeds") {
            options.seeds = value;
        } else if (argument == "--ops") {
            options.operations = value;
        } else {
            std::cerr << "usage: tradematch_differential_tests [--seed S | --first-seed S --seeds N] [--ops N]\n";
            return false;
        }
    }
    return true;
}

int run(int argc, char** argv) {
    Options options;
    try {
        if (!parse_options(argc, argv, options)) {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "invalid argument: " << error.what() << '\n';
        return 2;
    }

    DifferentialHarness::Stats totals;
    for (std::uint64_t seed = options.first_seed; seed < options.first_seed + options.seeds; ++seed) {
        // Odd seeds start the engine with room for one order, one level, and one trade, so every pool and the hash
        // index grow repeatedly while being checked against the reference.
        DifferentialHarness harness(seed % 2 == 1 ? kTinyCapacity : tradematch::BookCapacity{});
        OperationGenerator generator(seed);
        std::deque<Operation> history;

        for (std::uint64_t index = 0; index < options.operations; ++index) {
            const auto op = generator.next();
            history.push_back(op);
            if (history.size() > 12U) {
                history.pop_front();
            }

            const auto mismatch = harness.apply(op);
            if (!mismatch.empty()) {
                std::cout << "[FAIL] differential seed=" << seed << " op=" << index << ": " << mismatch << '\n';
                std::cout << "  most recent operations (last one diverged):\n";
                for (const auto& recent : history) {
                    std::cout << "    " << tradematch::testing::describe(recent) << '\n';
                }
                std::cout << "  reproduce with: tradematch_differential_tests --seed " << seed << " --ops " << index + 1
                          << '\n';
                return 1;
            }
        }

        const auto& stats = harness.stats();
        totals.operations += stats.operations;
        totals.trades += stats.trades;
        totals.rejections += stats.rejections;
        totals.cancels += stats.cancels;
        totals.replaces += stats.replaces;
        totals.max_resting_orders = std::max(totals.max_resting_orders, stats.max_resting_orders);
    }

    std::cout << "[PASS] engine matched the reference book on " << totals.operations << " random operations ("
              << options.seeds << " seeds x " << options.operations << ")\n"
              << "       trades=" << totals.trades << " rejections=" << totals.rejections
              << " cancels=" << totals.cancels << " replaces=" << totals.replaces
              << " max_resting_orders=" << totals.max_resting_orders << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
