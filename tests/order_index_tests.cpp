#include "tradematch/detail/order_index.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using tradematch::OrderId;
using tradematch::detail::OrderIndex;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void expect_matches(const OrderIndex& index, const std::unordered_map<OrderId, std::uint32_t>& model, OrderId limit) {
    expect(index.size() == model.size(), "Index size should match the model.");
    for (OrderId key = 1; key <= limit; ++key) {
        const auto it = model.find(key);
        const auto expected = it == model.end() ? OrderIndex::kMissing : it->second;
        expect(index.find(key) == expected, "Lookup of key " + std::to_string(key) + " should match the model.");
    }
}

void test_random_churn_matches_unordered_map() {
    // A small key space forces long probe runs, wrap-around, and constant backward-shift deletions.
    constexpr OrderId kKeySpace = 512;
    std::mt19937_64 rng(2024);
    std::uniform_int_distribution<OrderId> key_distribution(1, kKeySpace);
    std::uniform_int_distribution<int> action_distribution(0, 99);

    OrderIndex index;
    std::unordered_map<OrderId, std::uint32_t> model;
    std::uint32_t next_value = 0;

    for (int step = 0; step < 300'000; ++step) {
        const OrderId key = key_distribution(rng);
        const bool present = model.contains(key);
        if (action_distribution(rng) < 55) {
            if (!present) {
                index.insert(key, next_value);
                model.emplace(key, next_value);
                ++next_value;
            }
        } else {
            expect(index.erase(key) == present, "Erase should report whether the key was present.");
            model.erase(key);
        }
        if (step % 25'000 == 0) {
            expect_matches(index, model, kKeySpace);
        }
    }
    expect_matches(index, model, kKeySpace);
}

void test_sequential_ids_grow_and_shrink() {
    OrderIndex index;
    constexpr OrderId kCount = 100'000;
    for (OrderId key = 1; key <= kCount; ++key) {
        index.insert(key, static_cast<std::uint32_t>(key * 3));
    }
    expect(index.size() == kCount, "All sequential keys should be stored.");
    for (OrderId key = 1; key <= kCount; ++key) {
        expect(index.find(key) == key * 3, "Every sequential key should map to its value.");
    }
    for (OrderId key = 2; key <= kCount; key += 2) {
        expect(index.erase(key), "Even keys should erase.");
    }
    for (OrderId key = 1; key <= kCount; ++key) {
        const bool expected = key % 2 == 1;
        expect(index.contains(key) == expected, "Only odd keys should remain.");
    }
    expect(index.size() == kCount / 2, "Half of the keys should remain.");
}

void test_reserved_capacity_does_not_grow() {
    OrderIndex index(10'000);
    const auto capacity = index.capacity();
    expect(capacity >= 10'000, "Reserving should make room for the expected size.");
    for (OrderId key = 1; key <= 10'000; ++key) {
        index.insert(key, 0);
    }
    expect(index.capacity() == capacity, "Inserting the reserved number of keys should not grow the table.");
}

void test_key_zero_is_never_found() {
    OrderIndex index;
    index.insert(7, 1);
    expect(index.find(0) == OrderIndex::kMissing, "Key 0 is reserved and never found.");
    expect(!index.erase(0), "Key 0 cannot be erased.");
    expect(!index.erase(8), "Absent keys cannot be erased.");
    expect(index.find(7) == 1, "Present keys are found.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"random_churn_matches_unordered_map", test_random_churn_matches_unordered_map},
        {"sequential_ids_grow_and_shrink", test_sequential_ids_grow_and_shrink},
        {"reserved_capacity_does_not_grow", test_reserved_capacity_does_not_grow},
        {"key_zero_is_never_found", test_key_zero_is_never_found},
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
        std::cout << "All order index tests passed (" << tests.size() << ").\n";
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
