// Coverage-guided fuzz target for ITCH 5.0. Arbitrary bytes are read twice. First as messages, each typed by its first
// byte: any that decodes must re-encode to exactly the same bytes, and all of them go to a replica, which must count
// what doesn't fit its books rather than fail. Then as order flow for an engine whose ITCH feed must rebuild its book
// exactly after every request.
#include "support/differential.hpp"
#include "tradematch/itch.hpp"
#include "tradematch/itch_feed.hpp"
#include "tradematch/matching_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace {

namespace itch = tradematch::itch;

[[noreturn]] void fail(const std::string& problem) {
    std::fprintf(stderr, "%s\n", problem.c_str());
    std::abort();
}

void check_messages(std::span<const std::uint8_t> input) {
    itch::BookReplica replica;
    std::vector<std::uint8_t> again;
    while (!input.empty()) {
        // A known type takes its exact length, so most messages decode; anything else is a short run of garbage.
        const std::size_t wanted = itch::message_length(static_cast<char>(input.front()));
        const std::size_t length =
            std::min(input.size(), wanted != 0 ? wanted : 1 + (input.size() > 1 ? input[1] % 64U : 0U));
        const auto bytes = input.first(length);
        input = input.subspan(length);
        const auto message = itch::decode(bytes);
        if (!message) {
            if (replica.apply(bytes)) {
                fail("the replica accepted bytes that don't decode");
            }
            continue;
        }
        again.clear();
        itch::encode(*message, again);
        if (!std::ranges::equal(again, bytes)) {
            fail("a decoded message didn't re-encode to the same bytes");
        }
        replica.apply(*message);
    }
}

void apply_frames(itch::BookReplica& replica, std::span<const std::uint8_t> bytes) {
    while (bytes.size() >= 2) {
        const std::size_t length = (std::size_t{bytes[0]} << 8U) | bytes[1];
        if (!replica.apply(bytes.subspan(2, length))) {
            fail("the publisher wrote a message that doesn't decode");
        }
        bytes = bytes.subspan(2 + length);
    }
}

void check_feed(const std::uint8_t* data, std::size_t size) {
    using tradematch::testing::OpKind;
    tradematch::MatchingEngine engine;
    const auto symbol = engine.add_symbol("FUZZ", tradematch::BookCapacity{1, 1, 1});
    itch::FeedPublisher publisher(engine);
    engine.set_order_listener(&publisher);
    itch::BookReplica replica;
    tradematch::OrderBook& book = engine.book(symbol);

    tradematch::testing::ByteDecoder decoder(data, size);
    while (!decoder.done()) {
        const auto op = decoder.next();
        switch (op.kind) {
            case OpKind::Submit:
                book.submit(op.order);
                break;
            case OpKind::Cancel:
                book.cancel(op.order.order_id);
                break;
            case OpKind::Replace:
                book.replace(op.order);
                break;
            case OpKind::ExpireDay:
                book.expire_day_orders();
                break;
            case OpKind::BeginAuction:
                book.begin_auction();
                break;
            case OpKind::Uncross:
                book.uncross();
                break;
            case OpKind::Snapshot:
                book.import_state(book.export_state());
                break;
        }
        apply_frames(replica, publisher.take_bytes());
        if (const std::string difference = itch::first_difference(engine, publisher, replica); !difference.empty()) {
            fail("the ITCH feed didn't rebuild the book after " + tradematch::testing::describe(op) + ": " +
                 difference);
        }
    }
    if (replica.stats().anomalies != 0U) {
        fail("the replica found anomalies in the engine's own feed");
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    check_messages(std::span<const std::uint8_t>(data, size));
    check_feed(data, size);
    return 0;
}
