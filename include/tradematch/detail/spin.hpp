#ifndef TRADEMATCH_DETAIL_SPIN_HPP
#define TRADEMATCH_DETAIL_SPIN_HPP

#include <thread>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace tradematch::detail {

// Tells the CPU this is a spin-wait loop: it saves power and frees resources for the other hyper-thread, and on x86
// it avoids the memory-order mis-speculation penalty when the awaited value finally changes.
inline void cpu_relax() noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

// Waiting strategy for threads that poll a queue: spin briefly, which keeps wake-up latency in the tens of
// nanoseconds, then yield the core so an idle pipeline doesn't starve other threads on a busy machine. With
// `spin_forever`, the thread never gives up its core, which gives the lowest and steadiest latency when it has a
// core to itself.
class Backoff {
public:
    explicit Backoff(bool spin_forever = false) noexcept : spin_forever_(spin_forever) {}

    void pause() noexcept {
        if (spin_forever_ || spins_ < kSpinLimit) {
            ++spins_;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }

    void reset() noexcept {
        spins_ = 0;
    }

private:
    static constexpr unsigned kSpinLimit = 2'048;
    unsigned spins_{0};
    bool spin_forever_;
};

// Asks the scheduler to treat the calling thread as latency-critical. On Apple silicon this keeps it on the
// performance cores; elsewhere it does nothing, and pinning threads to isolated cores is the operator's job.
inline void prefer_performance_cores() noexcept {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace tradematch::detail

#endif
