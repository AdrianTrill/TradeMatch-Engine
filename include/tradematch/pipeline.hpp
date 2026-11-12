#ifndef TRADEMATCH_PIPELINE_HPP
#define TRADEMATCH_PIPELINE_HPP

#include "tradematch/book_listener.hpp"
#include "tradematch/journal.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tradematch {

// A request from the gateway thread to the matcher.
struct PipelineRequest {
    CommandType type{CommandType::Submit};  // Submit, Cancel, Replace, BeginAuction, Uncross, or ExpireDayOrders
    SymbolId symbol{};
    OrderRequest order;  // Cancel uses only order.order_id
    std::uint64_t request_id{};
    Timestamp sent_at{};  // steady-clock nanoseconds when the gateway sent the request
};

// What the matcher publishes. Each request produces its market data first, a Trade per execution and a LevelUpdate
// per price level whose displayed state changed, and then exactly one Report.
struct PipelineEvent {
    enum class Kind : std::uint8_t {
        Report,
        Trade,
        LevelUpdate
    };

    Kind kind{Kind::Report};
    SymbolId symbol{};
    std::uint64_t request_id{};  // the request that caused the event
    Timestamp sent_at{};         // copied from the request
    Timestamp matched_at{};      // Report: when the matcher finished the request
    bool accepted{false};        // Report: accepted, or for a cancel, an order was cancelled
    RejectReason reject_reason{RejectReason::None};
    Quantity filled_quantity{};  // Report
    Trade trade;                 // Trade
    LevelUpdate level;           // LevelUpdate
};

// How the matcher and publisher threads wait for work.
enum class WaitStrategy : std::uint8_t {
    SpinThenYield,  // spin for about a microsecond, then yield the core: friendly to a shared machine
    Spin            // never give up the core: the lowest latency, for threads that have a core to themselves
};

struct PipelineConfig {
    std::vector<std::string> symbols{"ACME"};
    BookCapacity capacity;
    std::size_t queue_capacity{std::size_t{1} << 16};  // per queue, rounded up to a power of two
    std::optional<std::filesystem::path> journal;      // journal every request (a new journal, Durability::Background)
    bool market_data{true};                            // publish Trade and LevelUpdate events
    WaitStrategy wait{WaitStrategy::SpinThenYield};
};

// Counters, each kept by the one thread that updates it. Read them after finish().
struct PipelineStats {
    std::uint64_t requests{0};       // accepted by send() or try_send()
    std::uint64_t gateway_waits{0};  // sends that found the request queue full and had to wait
    std::uint64_t matcher_waits{0};  // events that found the event queue full and had to wait
    std::uint64_t events{0};         // delivered to the consumer
    std::uint64_t trades{0};
    std::uint64_t rejects{0};
};

// A multi-threaded front end for a MatchingEngine, built like an exchange's order path:
//
//   gateway (the thread calling send) ──queue──► matcher thread ──queue──► publisher thread (calls the consumer)
//
// The matcher owns the engine, so matching stays single-threaded and deterministic: the pipeline produces exactly the
// results of applying the requests in order on one thread. Stages hand work over through lock-free single-producer,
// single-consumer rings and wait only when a queue is full or empty. With a journal, every request is journaled on
// the matcher thread before it is applied, and a fourth thread does the disk writes.
class Pipeline {
public:
    // Called on the publisher thread for every event.
    using Consumer = std::function<void(const PipelineEvent&)>;

    explicit Pipeline(PipelineConfig config, Consumer consumer = {});
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;
    ~Pipeline();  // finishes the pipeline if finish() wasn't called

    SymbolId symbol(std::string_view name) const;

    // The gateway side, for one thread at a time. send() waits while the request queue is full; try_send() returns
    // false instead.
    void send(const PipelineRequest& request);
    bool try_send(const PipelineRequest& request);

    // Delivers everything sent so far through both stages, stops the threads, and closes the journal. Rethrows the
    // first exception thrown on the matcher or publisher thread, such as a JournalError or one from the consumer.
    void finish();

    // Available after finish().
    const MatchingEngine& engine() const;
    const PipelineStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tradematch

#endif
