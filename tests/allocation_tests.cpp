// Proves the zero-allocation claim. Global operator new is replaced to count every heap allocation in the process,
// then the benchmark's 1,000,000-operation stream is replayed through each engine after warm-up. The test fails if
// the v2 engine allocates even once, with or without a write-ahead journal; the v1 baseline count is printed for
// comparison.
#include "baseline/map_order_book.hpp"
#include "journaled_book.hpp"
#include "tradematch/order_book.hpp"
#include "workload.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

std::uint64_t g_allocations = 0;
bool g_counting = false;

void* counted_allocate(std::size_t size) {
    if (g_counting) {
        ++g_allocations;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

}  // namespace

// libc++ declares these with reserved parameter names that user definitions cannot reuse, hence the NOLINTs.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    return counted_allocate(size);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new[](std::size_t size) {
    return counted_allocate(size);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete[](void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

namespace {

using tradematch::bench::Command;
using tradematch::bench::CommandKind;

template <typename Book>
std::size_t apply(Book& book, const Command& command) {
    switch (command.kind) {
        case CommandKind::Cancel:
            return book.cancel(command.order.order_id).cancelled ? 1U : 0U;
        case CommandKind::Replace:
            return book.replace(command.order).submit_result.trades.size();
        case CommandKind::Add:
        case CommandKind::Aggress:
            return book.submit(command.order).trades.size();
    }
    return 0;
}

// Replays warm-up untracked, then counts allocations over the measured segment.
template <typename Book>
std::uint64_t count_allocations(Book& book, std::span<const Command> warmup, std::span<const Command> measured) {
    std::size_t sink = 0;
    for (const auto& command : warmup) {
        sink += apply(book, command);
    }

    g_allocations = 0;
    g_counting = true;
    for (const auto& command : measured) {
        sink += apply(book, command);
    }
    g_counting = false;

    if (sink == 0) {
        throw std::runtime_error("the replay produced no activity");
    }
    return g_allocations;
}

int run() {
    const tradematch::bench::WorkloadConfig
        config;  // the benchmark's default stream: 10k prefill, 100k warm-up, 1M measured
    const auto workload = tradematch::bench::generate_workload(config);
    const std::span<const Command> commands{workload.commands};
    const auto warmup = commands.first(workload.measured_begin);
    const auto measured = commands.subspan(workload.measured_begin);

    tradematch::BookCapacity capacity;
    capacity.orders = 32'768;
    capacity.price_levels = 4'096;
    capacity.trades_per_call = 1'024;
    auto v2 = std::make_unique<tradematch::OrderBook>(capacity);
    const auto v2_allocations = count_allocations(*v2, warmup, measured);

    std::random_device device;
    const auto journal_path =
        std::filesystem::temp_directory_path() / ("tradematch-allocation-test-" + std::to_string(device()) + ".tmj");
    auto journaled = std::make_unique<tradematch::bench::JournaledOrderBook>(capacity,
                                                                             journal_path,
                                                                             tradematch::Durability::Buffered);
    const auto journaled_allocations = count_allocations(*journaled, warmup, measured);
    journaled.reset();
    std::error_code ignored;
    std::filesystem::remove(journal_path, ignored);

    auto v1 = std::make_unique<tradematch::baseline::MapOrderBook>();
    const auto v1_allocations = count_allocations(*v1, warmup, measured);

    const auto per_operation = [&](std::uint64_t allocations) {
        return static_cast<double>(allocations) / static_cast<double>(measured.size());
    };
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "heap allocations over " << measured.size() << " measured operations:\n";
    std::cout << "  v1 baseline  " << v1_allocations << " (" << per_operation(v1_allocations) << " per operation)\n";
    std::cout << "  v2           " << v2_allocations << " (" << per_operation(v2_allocations) << " per operation)\n";
    std::cout << "  v2 + journal " << journaled_allocations << " (" << per_operation(journaled_allocations)
              << " per operation)\n";

    if (v2_allocations != 0 || journaled_allocations != 0) {
        std::cout << "[FAIL] the v2 request path allocated on the heap\n";
        return 1;
    }
    if (v1_allocations == 0) {
        std::cout << "[FAIL] allocation counting is not working: the v1 baseline should allocate\n";
        return 1;
    }
    std::cout << "[PASS] the v2 engine performed zero heap allocations in steady state, journaled or not\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
