#ifndef TRADEMATCH_DETAIL_ORDER_INDEX_HPP
#define TRADEMATCH_DETAIL_ORDER_INDEX_HPP

#include "tradematch/types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace tradematch::detail {

// Open-addressing hash map from order ID to pool slot, specialized for the matching engine:
// - Linear probing over one flat array, so a lookup usually touches a single cache line.
// - Fibonacci hashing spreads sequential IDs, the common case, evenly across the table.
// - Backward-shift deletion instead of tombstones, so heavy cancel traffic never lengthens probe sequences.
// - Order ID 0 is invalid in the engine and doubles as the empty-slot marker.
// - The load factor never exceeds 1/2. The table only grows, and allocates, when an insert would pass that limit.
class OrderIndex {
public:
    using Value = std::uint32_t;
    static constexpr Value kMissing = std::numeric_limits<Value>::max();

    explicit OrderIndex(std::size_t expected_size = 0) {
        reserve(expected_size);
    }

    // Ensures `expected_size` entries fit without growing the table.
    void reserve(std::size_t expected_size) {
        const std::size_t wanted = std::bit_ceil(std::max(kMinSlots, expected_size * 2));
        if (wanted > slots_.size()) {
            rehash(wanted);
        }
    }

    std::size_t size() const noexcept {
        return size_;
    }

    // Number of entries that fit before the next insert grows the table.
    std::size_t capacity() const noexcept {
        return slots_.size() / 2;
    }

    Value find(OrderId key) const noexcept {
        if (key == kEmptyKey) {
            return kMissing;
        }
        for (std::size_t slot = home(key);; slot = next(slot)) {
            const Slot& entry = slots_[slot];
            if (entry.key == key) {
                return entry.value;
            }
            if (entry.key == kEmptyKey) {
                return kMissing;
            }
        }
    }

    bool contains(OrderId key) const noexcept {
        return find(key) != kMissing;
    }

    // Inserts a key that is not present. Key 0 is reserved as the empty marker.
    void insert(OrderId key, Value value) {
        if ((size_ + 1) * 2 > slots_.size()) {
            rehash(slots_.size() * 2);
        }
        place(key, value);
        ++size_;
    }

    // Removes `key` and returns whether it was present.
    bool erase(OrderId key) noexcept {
        if (key == kEmptyKey) {
            return false;
        }
        std::size_t hole = home(key);
        while (slots_[hole].key != key) {
            if (slots_[hole].key == kEmptyKey) {
                return false;
            }
            hole = next(hole);
        }

        // Backward-shift deletion: pull later members of the probe run into the hole whenever their probe path passes
        // through it, so every remaining key stays reachable without tombstones.
        for (std::size_t candidate = next(hole); slots_[candidate].key != kEmptyKey; candidate = next(candidate)) {
            const std::size_t displacement = (candidate - home(slots_[candidate].key)) & mask_;
            const std::size_t distance_to_hole = (candidate - hole) & mask_;
            if (displacement >= distance_to_hole) {
                slots_[hole] = slots_[candidate];
                hole = candidate;
            }
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

private:
    static constexpr OrderId kEmptyKey = 0;
    static constexpr std::size_t kMinSlots = 16;
    static constexpr std::uint64_t kFibonacciMultiplier = 0x9E3779B97F4A7C15ULL;  // 2^64 / golden ratio

    struct Slot {
        OrderId key{kEmptyKey};
        Value value{kMissing};
    };

    std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * kFibonacciMultiplier) >> shift_);
    }

    std::size_t next(std::size_t slot) const noexcept {
        return (slot + 1) & mask_;
    }

    void place(OrderId key, Value value) noexcept {
        std::size_t slot = home(key);
        while (slots_[slot].key != kEmptyKey) {
            slot = next(slot);
        }
        slots_[slot] = Slot{key, value};
    }

    void rehash(std::size_t slot_count) {
        std::vector<Slot> previous(slot_count);
        previous.swap(slots_);
        mask_ = slot_count - 1;
        shift_ = 64U - static_cast<unsigned>(std::countr_zero(slot_count));
        for (const Slot& entry : previous) {
            if (entry.key != kEmptyKey) {
                place(entry.key, entry.value);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t size_{0};
    std::size_t mask_{0};
    unsigned shift_{64};
};

}  // namespace tradematch::detail

#endif
