// Coverage-guided fuzz target: decodes arbitrary bytes into order flow and aborts on the first observable
// difference between the optimized OrderBook and the naive reference book.
#include "support/differential.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    tradematch::testing::DifferentialHarness harness(tradematch::BookCapacity{1, 1, 1});
    tradematch::testing::ByteDecoder decoder(data, size);

    while (!decoder.done()) {
        const auto op = decoder.next();
        const auto mismatch = harness.apply(op);
        if (!mismatch.empty()) {
            std::fprintf(stderr,
                         "engine/reference mismatch after %s: %s\n",
                         tradematch::testing::describe(op).c_str(),
                         mismatch.c_str());
            std::abort();
        }
    }
    return 0;
}
