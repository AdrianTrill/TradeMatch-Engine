#include "support/differential.hpp"
#include "tradematch/journal.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/pipeline.hpp"
#include "tradematch/snapshot.hpp"
#include "tradematch/spsc_ring.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using tradematch::CommandType;
using tradematch::PipelineEvent;
using tradematch::PipelineRequest;
using tradematch::Side;
using tradematch::SpscRing;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void test_ring_basics() {
    SpscRing<int> ring(5);
    expect(ring.capacity() == 8U && ring.size() == 0U, "Capacities round up to a power of two.");
    int value = 0;
    expect(!ring.try_pop(value), "A new ring is empty.");
    for (int item = 0; item < 8; ++item) {
        expect(ring.try_push(item), "A ring accepts up to its capacity.");
    }
    expect(!ring.try_push(8) && ring.size() == 8U, "A full ring refuses more.");
    for (int item = 0; item < 3; ++item) {
        expect(ring.try_pop(value) && value == item, "Items come out in FIFO order.");
    }
    std::array<int, 16> batch{};
    expect(ring.try_pop_batch(batch) == 5U && batch[0] == 3 && batch[4] == 7, "A batch pop takes what is there.");
    expect(ring.try_pop_batch(batch) == 0U, "An empty ring yields an empty batch.");

    // Indices keep growing past the capacity many times over.
    for (int item = 0; item < 1'000; ++item) {
        expect(ring.try_push(item) && ring.try_pop(value) && value == item, "The ring wraps around correctly.");
    }
}

void test_ring_moves_every_item_across_threads_in_order() {
    constexpr std::uint64_t kItems = 1'000'000;
    SpscRing<std::uint64_t> ring(16);  // tiny, so both sides constantly hit full and empty
    std::thread producer([&ring] {
        for (std::uint64_t item = 0; item < kItems; ++item) {
            while (!ring.try_push(item)) {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t expected = 0;
    bool in_order = true;
    std::array<std::uint64_t, 7> batch{};
    while (expected < kItems) {
        // Alternate single and batch pops.
        if (expected % 2 == 0U) {
            std::uint64_t value = 0;
            if (ring.try_pop(value)) {
                in_order = in_order && value == expected;
                ++expected;
            }
        } else {
            const std::size_t count = ring.try_pop_batch(batch);
            for (std::size_t index = 0; index < count; ++index) {
                in_order = in_order && batch[index] == expected;
                ++expected;
            }
        }
    }
    producer.join();
    expect(in_order && ring.size() == 0U, "Every item should arrive exactly once and in order.");
}

// The engine-side effects of one request, as seen through the pipeline or by applying it directly.
struct Outcome {
    bool accepted{false};
    tradematch::RejectReason reject_reason{tradematch::RejectReason::None};
    tradematch::Quantity filled{};
    std::vector<tradematch::Trade> trades;

    bool operator==(const Outcome&) const = default;
};

PipelineRequest to_request(const tradematch::testing::Operation& op, std::uint64_t request_id) {
    using tradematch::testing::OpKind;
    PipelineRequest request;
    request.request_id = request_id;
    request.order = op.order;
    switch (op.kind) {
        case OpKind::Submit:
            request.type = CommandType::Submit;
            break;
        case OpKind::Cancel:
            request.type = CommandType::Cancel;
            break;
        case OpKind::Replace:
            request.type = CommandType::Replace;
            break;
        case OpKind::ExpireDay:
            request.type = CommandType::ExpireDayOrders;
            break;
        case OpKind::BeginAuction:
            request.type = CommandType::BeginAuction;
            break;
        case OpKind::Uncross:
        case OpKind::Snapshot:
            request.type = CommandType::Uncross;
            break;
    }
    return request;
}

// Applies a request directly, on this thread, as the matcher would.
Outcome apply_directly(tradematch::MatchingEngine& engine, const PipelineRequest& request) {
    Outcome outcome;
    const auto copy_trades = [&](std::span<const tradematch::Trade> trades) {
        outcome.trades.assign(trades.begin(), trades.end());
    };
    switch (request.type) {
        case CommandType::Submit: {
            const auto result = engine.submit(request.symbol, request.order);
            outcome = {result.accepted, result.reject_reason, result.filled_quantity, {}};
            copy_trades(result.trades);
            break;
        }
        case CommandType::Cancel: {
            const auto result = engine.cancel(request.symbol, request.order.order_id);
            outcome = {result.cancelled, result.reject_reason, 0, {}};
            break;
        }
        case CommandType::Replace: {
            const auto result = engine.replace(request.symbol, request.order);
            outcome = {result.replaced, result.reject_reason, result.submit_result.filled_quantity, {}};
            copy_trades(result.submit_result.trades);
            break;
        }
        case CommandType::BeginAuction:
            engine.book(request.symbol).begin_auction();
            outcome.accepted = true;
            break;
        case CommandType::Uncross:
            copy_trades(engine.book(request.symbol).uncross().trades);
            outcome.accepted = true;
            break;
        case CommandType::ExpireDayOrders:
            engine.expire_day_orders();
            outcome.accepted = true;
            break;
        case CommandType::AddSymbol:
            break;
    }
    return outcome;
}

std::vector<PipelineRequest> generated_requests(std::uint64_t seed, std::size_t count) {
    tradematch::testing::OperationGenerator generator(seed);
    tradematch::testing::ReferenceBook view;  // only used to aim replacements
    std::vector<PipelineRequest> requests;
    for (std::size_t index = 0; index < count; ++index) {
        const auto op = generator.next(view);
        switch (op.kind) {
            case tradematch::testing::OpKind::Submit:
                view.submit(op.order);
                break;
            case tradematch::testing::OpKind::Cancel:
                view.cancel(op.order.order_id);
                break;
            case tradematch::testing::OpKind::Replace:
                view.replace(op.order);
                break;
            case tradematch::testing::OpKind::ExpireDay:
                view.expire_day_orders();
                break;
            case tradematch::testing::OpKind::BeginAuction:
                view.begin_auction();
                break;
            case tradematch::testing::OpKind::Uncross:
            case tradematch::testing::OpKind::Snapshot:
                view.uncross();
                break;
        }
        requests.push_back(to_request(op, index + 1));
    }
    return requests;
}

void test_pipeline_matches_the_single_threaded_engine() {
    const auto requests = generated_requests(17, 30'000);

    // Collected on the publisher thread, read after finish(), which joins it.
    std::vector<Outcome> outcomes;
    std::vector<tradematch::Trade> pending_trades;
    std::uint64_t last_report = 0;
    bool reports_in_order = true;
    tradematch::PipelineConfig config;
    config.queue_capacity = 256;
    tradematch::Pipeline pipeline(config, [&](const PipelineEvent& event) {
        if (event.kind == PipelineEvent::Kind::Trade) {
            pending_trades.push_back(event.trade);
        } else if (event.kind == PipelineEvent::Kind::Report) {
            reports_in_order = reports_in_order && event.request_id == last_report + 1;
            last_report = event.request_id;
            outcomes.push_back({event.accepted, event.reject_reason, event.filled_quantity, pending_trades});
            pending_trades.clear();
        }
    });
    for (const auto& request : requests) {
        pipeline.send(request);
    }
    pipeline.finish();

    tradematch::MatchingEngine direct;
    direct.add_symbol("ACME");
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (index >= outcomes.size() || !(apply_directly(direct, requests[index]) == outcomes[index])) {
            ++mismatches;
        }
    }
    expect(reports_in_order && outcomes.size() == requests.size(), "Every request should get one report, in order.");
    expect(
        mismatches == 0U,
        "The pipeline should produce exactly the single-threaded results; mismatches: " + std::to_string(mismatches));
    expect(tradematch::state_hash(pipeline.engine()) == tradematch::state_hash(direct),
           "The pipeline's books should end up identical to the single-threaded engine's.");
    const auto& stats = pipeline.stats();
    expect(stats.requests == requests.size() && stats.trades > 1'000U && stats.rejects > 0U,
           "The stats should count the flow.");
}

void test_market_data_rebuilds_the_book_on_the_publisher_thread() {
    const auto requests = generated_requests(29, 20'000);
    tradematch::DepthBook replica;
    tradematch::Pipeline pipeline({}, [&](const PipelineEvent& event) {
        if (event.kind == PipelineEvent::Kind::LevelUpdate) {
            replica.apply(event.level);
        }
    });
    for (const auto& request : requests) {
        pipeline.send(request);
    }
    pipeline.finish();

    const auto snapshot = pipeline.engine().book(0).snapshot();
    const auto matches = [&](Side side, const std::vector<tradematch::PriceLevelView>& levels) {
        const auto replicated = replica.levels(side);
        if (replicated.size() != levels.size()) {
            return false;
        }
        for (std::size_t index = 0; index < levels.size(); ++index) {
            if (replicated[index].price != levels[index].price ||
                replicated[index].quantity != levels[index].total_quantity ||
                replicated[index].order_count != levels[index].order_count) {
                return false;
            }
        }
        return true;
    };
    expect(matches(Side::Buy, snapshot.bids) && matches(Side::Sell, snapshot.asks),
           "Level updates published across threads should rebuild the engine's depth exactly.");
}

void test_journaled_pipeline_replays_to_the_same_state() {
    std::random_device device;
    const auto path =
        std::filesystem::temp_directory_path() / ("tradematch-pipeline-test-" + std::to_string(device()) + ".tmj");
    const auto requests = generated_requests(41, 20'000);
    std::uint64_t live_hash = 0;
    {
        tradematch::PipelineConfig config;
        config.journal = path;
        config.market_data = false;
        tradematch::Pipeline pipeline(config);
        for (const auto& request : requests) {
            pipeline.send(request);
        }
        pipeline.finish();
        live_hash = tradematch::state_hash(pipeline.engine());
    }
    tradematch::MatchingEngine replayed;
    const auto result = tradematch::recover(replayed, path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    expect(result.last_sequence == requests.size() + 1, "The journal should hold the symbol and every request.");
    expect(tradematch::state_hash(replayed) == live_hash, "Replaying the pipeline's journal rebuilds its state.");
}

void test_backpressure_never_loses_or_reorders_events() {
    const auto requests = generated_requests(53, 5'000);
    std::uint64_t reports = 0;
    std::uint64_t last_report = 0;
    bool in_order = true;
    tradematch::PipelineConfig config;
    config.queue_capacity = 2;
    tradematch::Pipeline pipeline(config, [&](const PipelineEvent& event) {
        if (event.kind != PipelineEvent::Kind::Report) {
            return;
        }
        in_order = in_order && event.request_id == last_report + 1;
        last_report = event.request_id;
        if (++reports % 500U == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // a consumer that stalls now and then
        }
    });
    for (const auto& request : requests) {
        pipeline.send(request);
    }
    pipeline.finish();
    const auto& stats = pipeline.stats();
    expect(in_order && reports == requests.size(), "Full queues should slow the flow down, never drop events.");
    expect(stats.gateway_waits > 0U && stats.matcher_waits > 0U, "Both queues should have filled up.");
}

void test_requests_the_matcher_cannot_apply_are_rejected() {
    std::vector<PipelineEvent> reports;
    tradematch::Pipeline pipeline({}, [&](const PipelineEvent& event) {
        if (event.kind == PipelineEvent::Kind::Report) {
            reports.push_back(event);
        }
    });
    PipelineRequest unknown_symbol;
    unknown_symbol.symbol = 9;
    unknown_symbol.order = {1, Side::Buy, tradematch::OrderType::Limit, 10, 100};
    pipeline.send(unknown_symbol);
    PipelineRequest auction = unknown_symbol;
    auction.type = CommandType::Uncross;
    pipeline.send(auction);
    PipelineRequest add_symbol;
    add_symbol.type = CommandType::AddSymbol;
    pipeline.send(add_symbol);
    pipeline.finish();

    bool all_rejected = reports.size() == 3U;
    for (const auto& report : reports) {
        all_rejected =
            all_rejected && !report.accepted && report.reject_reason == tradematch::RejectReason::UnknownSymbol;
    }
    expect(all_rejected, "Requests for unknown symbols, and symbol changes, should be rejected.");
    expect(pipeline.symbol("ACME") == 0U, "Configured symbols should be found by name.");
    bool threw = false;
    try {
        pipeline.send(unknown_symbol);
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "A finished pipeline should refuse new requests.");
}

void test_consumer_exceptions_surface_in_finish() {
    tradematch::Pipeline pipeline({}, [](const PipelineEvent& event) {
        if (event.request_id == 3U && event.kind == PipelineEvent::Kind::Report) {
            throw std::runtime_error("consumer failure");
        }
    });
    for (std::uint64_t request_id = 1; request_id <= 10; ++request_id) {
        PipelineRequest request;
        request.request_id = request_id;
        request.order = {request_id, Side::Buy, tradematch::OrderType::Limit, 10, 100};
        pipeline.send(request);
    }
    bool rethrown = false;
    try {
        pipeline.finish();
    } catch (const std::runtime_error& error) {
        rethrown = std::string{error.what()} == "consumer failure";
    }
    expect(rethrown, "finish() should rethrow what the consumer threw.");
    expect(pipeline.engine().book(0).order_count() == 10U, "Requests after the failure should still be matched.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"ring_basics", test_ring_basics},
        {"ring_moves_every_item_across_threads_in_order", test_ring_moves_every_item_across_threads_in_order},
        {"pipeline_matches_the_single_threaded_engine", test_pipeline_matches_the_single_threaded_engine},
        {"market_data_rebuilds_the_book_on_the_publisher_thread",
         test_market_data_rebuilds_the_book_on_the_publisher_thread},
        {"journaled_pipeline_replays_to_the_same_state", test_journaled_pipeline_replays_to_the_same_state},
        {"backpressure_never_loses_or_reorders_events", test_backpressure_never_loses_or_reorders_events},
        {"requests_the_matcher_cannot_apply_are_rejected", test_requests_the_matcher_cannot_apply_are_rejected},
        {"consumer_exceptions_surface_in_finish", test_consumer_exceptions_surface_in_finish},
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
        std::cout << "All pipeline tests passed (" << tests.size() << ").\n";
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
