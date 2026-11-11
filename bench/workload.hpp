#ifndef TRADEMATCH_BENCH_WORKLOAD_HPP
#define TRADEMATCH_BENCH_WORKLOAD_HPP

#include "tradematch/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tradematch::bench {

enum class CommandKind : std::uint8_t {
    Add,      // passive limit order that rests at or behind the touch
    Cancel,   // cancel of a live resting order
    Replace,  // modify price and/or quantity of a live resting order
    Aggress   // marketable limit or market order that takes liquidity
};

inline constexpr std::size_t kCommandKindCount = 4;
inline constexpr std::array<std::string_view, kCommandKindCount> kCommandKindNames{"add",
                                                                                   "cancel",
                                                                                   "replace",
                                                                                   "aggress"};

struct Command {
    CommandKind kind{CommandKind::Add};
    OrderRequest order;  // Cancel only uses order.order_id.
};

struct WorkloadConfig {
    std::uint64_t seed{42};
    std::size_t prefill_orders{10'000};    // resting orders placed first; the flow keeps the book near this depth
    std::size_t warmup_commands{100'000};  // replayed but not recorded, to warm caches and branch predictors
    std::size_t measured_commands{1'000'000};
    unsigned add_percent{48};
    unsigned cancel_percent{37};
    unsigned replace_percent{8};  // the remainder of the mix is aggressive orders
    Price start_mid{10'000};
};

struct Workload {
    std::vector<Command> commands;  // prefill, then warm-up, then the measured segment
    std::size_t measured_begin{0};  // index of the first measured command
    std::array<std::size_t, kCommandKindCount> measured_mix{};
};

// Builds a realistic, fully deterministic order flow. The generator simulates the stream against a scratch book as it
// goes, so cancels and modifies always target orders that are still live and passive orders never cross the spread.
// Pre-generating the stream keeps generator cost out of the timed loop and lets every engine replay identical input.
Workload generate_workload(const WorkloadConfig& config);

}  // namespace tradematch::bench

#endif
