#ifndef TRADEMATCH_BENCH_LATENCY_HISTOGRAM_HPP
#define TRADEMATCH_BENCH_LATENCY_HISTOGRAM_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tradematch::bench {

// Log-linear latency histogram in the spirit of HdrHistogram. Values below 256 ns are recorded exactly; above that,
// every power-of-two range is split into 128 linear sub-buckets, so any reported percentile is within 1/128 (0.8%)
// of the true value. Memory is constant (about 58 KiB) no matter how many samples are recorded.
class LatencyHistogram {
public:
    void record(std::uint64_t value) noexcept {
        ++counts_[index_of(value)];
        ++count_;
        sum_ += value;
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);
    }

    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t index = 0; index < kBucketCount; ++index) {
            counts_[index] += other.counts_[index];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = std::min(min_, other.min_);
        max_ = std::max(max_, other.max_);
    }

    std::uint64_t count() const noexcept {
        return count_;
    }

    std::uint64_t min() const noexcept {
        return count_ == 0 ? 0 : min_;
    }

    std::uint64_t max() const noexcept {
        return max_;
    }

    double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // Smallest recorded value v such that at least `percent` of all samples are <= v, reported as the upper edge of
    // its bucket (never above the true maximum).
    std::uint64_t percentile(double percent) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const auto clamped = std::clamp(percent, 0.0, 100.0);
        const auto target = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(count_))));

        std::uint64_t seen = 0;
        for (std::size_t index = 0; index < kBucketCount; ++index) {
            seen += counts_[index];
            if (seen >= target) {
                return std::min(upper_edge(index), max_);
            }
        }
        return max_;
    }

    // Fraction of samples strictly below `limit`, used for the ASCII distribution chart.
    double fraction_below(std::uint64_t limit) const noexcept {
        if (count_ == 0 || limit == 0) {
            return 0.0;
        }
        std::uint64_t below = 0;
        for (std::size_t index = 0; index < kBucketCount && upper_edge(index) < limit; ++index) {
            below += counts_[index];
        }
        return static_cast<double>(below) / static_cast<double>(count_);
    }

private:
    static constexpr unsigned kSubBucketBits = 7;
    static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << kSubBucketBits;  // 128 per power of two
    static constexpr std::uint64_t kExactLimit = kSubBuckets * 2;                     // values < 256 are exact
    static constexpr unsigned kFirstLogExponent = kSubBucketBits + 1;                 // 2^8 = 256
    static constexpr std::size_t kBucketCount = kExactLimit + (64 - kFirstLogExponent) * kSubBuckets;

    static std::size_t index_of(std::uint64_t value) noexcept {
        if (value < kExactLimit) {
            return static_cast<std::size_t>(value);
        }
        // Halve until the value fits in [128, 256): the number of halvings selects the power-of-two range and the
        // remaining value selects the linear sub-bucket inside it.
        std::uint64_t scaled = value;
        std::uint64_t halvings = 0;
        while (scaled >= kExactLimit) {
            scaled >>= 1U;
            ++halvings;
        }
        return static_cast<std::size_t>(kExactLimit + (halvings - 1) * kSubBuckets + (scaled - kSubBuckets));
    }

    static std::uint64_t upper_edge(std::size_t index) noexcept {
        if (index < kExactLimit) {
            return index;
        }
        const std::uint64_t offset = index - kExactLimit;
        const auto exponent = static_cast<unsigned>(kFirstLogExponent + offset / kSubBuckets);
        const std::uint64_t sub_bucket = kSubBuckets + offset % kSubBuckets;
        const unsigned shift = exponent - kSubBucketBits;
        return ((sub_bucket + 1) << shift) - 1;
    }

    std::array<std::uint64_t, kBucketCount> counts_{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_{0};
};

}  // namespace tradematch::bench

#endif
