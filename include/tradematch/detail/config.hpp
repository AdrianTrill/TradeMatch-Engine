#ifndef TRADEMATCH_DETAIL_CONFIG_HPP
#define TRADEMATCH_DETAIL_CONFIG_HPP

// Inlining hints for the request path. Small helpers that run on every request are forced inline so the compiler's
// size heuristics cannot move them out of line as features are added; rarely used features (stops, icebergs,
// fill-or-kill, self-trade prevention) are kept out of line so they do not bloat the hot functions.
#if defined(_MSC_VER) && !defined(__clang__)
#define TRADEMATCH_HOT_INLINE __forceinline
#define TRADEMATCH_COLD __declspec(noinline)
#else
#define TRADEMATCH_HOT_INLINE [[gnu::always_inline]] inline
#define TRADEMATCH_COLD [[gnu::noinline, gnu::cold]]
#endif

#endif
