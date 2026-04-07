#include "tradematch/order_book.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

std::size_t parse_order_count(int argc, char** argv) {
    if (argc < 2) {
        return 200000;
    }

    try {
        return static_cast<std::size_t>(std::stoull(argv[1]));
    } catch (const std::exception&) {
        throw std::runtime_error("Expected an optional positive integer order count.");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t order_count = 0;

    try {
        order_count = parse_order_count(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    tradematch::OrderBook book;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> side_distribution(0, 1);
    std::uniform_int_distribution<int> type_distribution(1, 10);
    std::uniform_int_distribution<int> quantity_distribution(1, 200);
    std::uniform_int_distribution<int> price_offset_distribution(-50, 50);

    std::size_t accepted_orders = 0;
    std::size_t rejected_orders = 0;
    std::size_t rested_orders = 0;
    std::size_t expired_orders = 0;
    std::size_t market_orders = 0;
    std::size_t limit_orders = 0;
    std::size_t trade_count = 0;
    std::uint64_t executed_quantity = 0;

    const auto start = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < order_count; ++index) {
        const auto side = side_distribution(rng) == 0 ? tradematch::Side::Buy : tradematch::Side::Sell;
        const auto type = type_distribution(rng) == 1 ? tradematch::OrderType::Market : tradematch::OrderType::Limit;
        const auto quantity = static_cast<tradematch::Quantity>(quantity_distribution(rng));
        const auto price =
            type == tradematch::OrderType::Limit
                ? static_cast<tradematch::Price>(10000 + price_offset_distribution(rng) * 5)
                : 0;

        if (type == tradematch::OrderType::Market) {
            ++market_orders;
        } else {
            ++limit_orders;
        }

        const auto result = book.submit(
            tradematch::OrderRequest{static_cast<tradematch::OrderId>(index + 1), side, type, quantity, price});

        if (result.accepted) {
            ++accepted_orders;
        } else {
            ++rejected_orders;
        }

        if (result.rested) {
            ++rested_orders;
        }

        if (result.expired) {
            ++expired_orders;
        }

        trade_count += result.trades.size();
        for (const auto& trade : result.trades) {
            executed_quantity += trade.quantity;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = end - start;
    const double elapsed_milliseconds = elapsed.count() * 1000.0;
    const double nanoseconds_per_order =
        order_count > 0U ? (elapsed.count() * 1000000000.0) / static_cast<double>(order_count) : 0.0;
    const double throughput = elapsed.count() > 0.0 ? static_cast<double>(order_count) / elapsed.count() : 0.0;

    std::cout << "TradeMatch benchmark\n";
    std::cout << "  seed: 42\n";
    std::cout << "Processed orders: " << order_count << '\n';
    std::cout << "Accepted orders: " << accepted_orders << '\n';
    std::cout << "Rejected orders: " << rejected_orders << '\n';
    std::cout << "Limit orders: " << limit_orders << '\n';
    std::cout << "Market orders: " << market_orders << '\n';
    std::cout << "Elapsed time: " << std::fixed << std::setprecision(3) << elapsed_milliseconds << " ms\n";
    std::cout << "Average time/order: " << nanoseconds_per_order << " ns\n";
    std::cout << "Throughput: " << static_cast<std::uint64_t>(throughput) << " orders/s\n";
    std::cout << "Trades generated: " << trade_count << '\n';
    std::cout << "Executed quantity: " << executed_quantity << '\n';
    std::cout << "Orders rested: " << rested_orders << '\n';
    std::cout << "Orders expired: " << expired_orders << '\n';
    std::cout << "Resting orders: " << book.order_count() << '\n';
    std::cout << "Benchmark is deterministic for the same binary and input count.\n";

    return 0;
}
