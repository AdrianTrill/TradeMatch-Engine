#ifndef TRADEMATCH_SPSC_RING_HPP
#define TRADEMATCH_SPSC_RING_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace tradematch {

// Keeps data that different cores write on separate cache lines. 128 bytes covers every current CPU: Apple's
// M-series cores use 128-byte lines, and Intel's adjacent-line prefetcher pairs 64-byte lines.
inline constexpr std::size_t kCacheLineBytes = 128;

// A bounded, lock-free queue for exactly one producer thread and one consumer thread.
//
// Each side owns one index and publishes it with a release store; the other side reads it with an acquire load, which
// is all the synchronization a single producer and consumer need. The indices only grow, and a slot is the index
// masked by the power-of-two capacity. The producer's and consumer's fields live on separate cache lines, and each
// side keeps a cached copy of the other's index, reloading it only when the queue looks full (producer) or empty
// (consumer). In steady state the two cores therefore touch each other's cache line once per batch of messages
// rather than once per message.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // structure padded due to alignas: separating the cache lines is the point
#endif

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>,
                  "slots are copied with plain assignment and never destroyed individually");

public:
    // The capacity is rounded up to a power of two.
    explicit SpscRing(std::size_t capacity)
        : mask_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity) - 1), slots_(mask_ + 1) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    // Producer only. Returns false when the queue is full.
    bool try_push(const T& value) noexcept {
        const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
        if (tail - producer_.cached_head > mask_) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            if (tail - producer_.cached_head > mask_) {
                return false;
            }
        }
        slots_[tail & mask_] = value;
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false when the queue is empty.
    bool try_pop(T& value) noexcept {
        const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cached_tail) {
                return false;
            }
        }
        value = slots_[head & mask_];
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Pops up to out.size() messages with a single release store, and returns how many it took.
    std::size_t try_pop_batch(std::span<T> out) noexcept {
        const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
        }
        const auto available = static_cast<std::size_t>(consumer_.cached_tail - head);
        const std::size_t count = available < out.size() ? available : out.size();
        for (std::size_t index = 0; index < count; ++index) {
            out[index] = slots_[(head + index) & mask_];
        }
        if (count != 0U) {
            consumer_.head.store(head + count, std::memory_order_release);
        }
        return count;
    }

    std::size_t capacity() const noexcept {
        return mask_ + 1;
    }

    // Exact when called by either side while the other is idle; otherwise a snapshot that may already be stale.
    std::size_t size() const noexcept {
        const std::uint64_t head = consumer_.head.load(std::memory_order_acquire);
        const std::uint64_t tail = producer_.tail.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

private:
    struct alignas(kCacheLineBytes) ProducerSide {
        std::atomic<std::uint64_t> tail{0};  // the next slot to fill
        std::uint64_t cached_head{0};        // the producer's last view of the consumer's index
    };

    struct alignas(kCacheLineBytes) ConsumerSide {
        std::atomic<std::uint64_t> head{0};  // the next slot to read
        std::uint64_t cached_tail{0};        // the consumer's last view of the producer's index
    };

    ProducerSide producer_;
    ConsumerSide consumer_;
    std::size_t mask_;
    std::vector<T> slots_;  // sized once; never grows
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

}  // namespace tradematch

#endif
