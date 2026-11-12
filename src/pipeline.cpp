#include "tradematch/pipeline.hpp"

#include "tradematch/detail/spin.hpp"
#include "tradematch/spsc_ring.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace tradematch {

namespace {

Timestamp steady_now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Inbound {
    PipelineRequest request;
    bool stop{false};  // sent once, last, by finish()
};

struct Outbound {
    PipelineEvent event;
    bool stop{false};
};

// Messages taken from a queue at once: enough to amortize the queue's atomics, small enough to stay in L1.
constexpr std::size_t kBatchSize = 64;

}  // namespace

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // structure padded due to alignas: separating the threads' cache lines is the point
#endif

struct Pipeline::Impl final : public MarketDataListener {
    Impl(PipelineConfig pipeline_config, Consumer event_consumer)
        : config(std::move(pipeline_config)),
          consumer(std::move(event_consumer)),
          journal(config.journal ? std::make_unique<JournalWriter>(*config.journal,
                                                                   JournalOptions{Durability::Background, true, 1})
                                 : nullptr),
          journaled(engine, journal.get()),
          requests(config.queue_capacity),
          events(config.queue_capacity) {
        for (const std::string& name : config.symbols) {
            journaled.add_symbol(name, config.capacity);
        }
        if (config.market_data) {
            engine.set_listener(this);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() override = default;

    // Market data arrives here on the matcher thread while it applies a request.
    void on_trade(SymbolId symbol, const Trade& trade) override {
        Outbound message;
        tag(message.event, PipelineEvent::Kind::Trade, symbol);
        message.event.trade = trade;
        publish(message);
    }

    void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
        Outbound message;
        tag(message.event, PipelineEvent::Kind::LevelUpdate, symbol);
        message.event.level = update;
        publish(message);
    }

    void tag(PipelineEvent& event, PipelineEvent::Kind kind, SymbolId symbol) const noexcept {
        event.kind = kind;
        event.symbol = symbol;
        if (matcher_state.current != nullptr) {
            event.request_id = matcher_state.current->request_id;
            event.sent_at = matcher_state.current->sent_at;
        }
    }

    // Matcher thread. After a failure it keeps draining requests, without applying them, so the gateway never
    // blocks; finish() rethrows the failure.
    void run_matcher() noexcept {
        detail::prefer_performance_cores();
        std::array<Inbound, kBatchSize> batch;  // every member has a default initializer
        detail::Backoff backoff(config.wait == WaitStrategy::Spin);
        while (true) {
            const std::size_t count = requests.try_pop_batch(batch);
            if (count == 0U) {
                backoff.pause();
                continue;
            }
            backoff.reset();
            for (std::size_t index = 0; index < count; ++index) {
                if (batch[index].stop) {
                    publish(Outbound{{}, true});
                    return;
                }
                if (matcher_state.failure) {
                    continue;
                }
                try {
                    process(batch[index].request);
                } catch (...) {
                    matcher_state.failure = std::current_exception();
                }
            }
        }
    }

    void process(const PipelineRequest& request) {
        matcher_state.current = &request;
        Outbound message;
        PipelineEvent& report = message.event;
        report.symbol = request.symbol;
        report.request_id = request.request_id;
        report.sent_at = request.sent_at;

        switch (request.type) {
            case CommandType::Submit: {
                const SubmitResult result = journaled.submit(request.symbol, request.order);
                report.accepted = result.accepted;
                report.reject_reason = result.reject_reason;
                report.filled_quantity = result.filled_quantity;
                break;
            }
            case CommandType::Cancel: {
                const CancelResult result = journaled.cancel(request.symbol, request.order.order_id);
                report.accepted = result.cancelled;
                report.reject_reason = result.reject_reason;
                break;
            }
            case CommandType::Replace: {
                const ReplaceResult result = journaled.replace(request.symbol, request.order);
                report.accepted = result.replaced;
                report.reject_reason = result.reject_reason;
                report.filled_quantity = result.submit_result.filled_quantity;
                break;
            }
            case CommandType::BeginAuction:
            case CommandType::Uncross:
                if (!engine.has_symbol(request.symbol)) {
                    report.reject_reason = RejectReason::UnknownSymbol;
                } else if (request.type == CommandType::BeginAuction) {
                    journaled.begin_auction(request.symbol);
                    report.accepted = true;
                } else {
                    journaled.uncross(request.symbol);
                    report.accepted = true;
                }
                break;
            case CommandType::ExpireDayOrders:
                journaled.expire_day_orders();
                report.accepted = true;
                break;
            case CommandType::AddSymbol:
                report.reject_reason = RejectReason::UnknownSymbol;  // the symbols are fixed when the pipeline starts
                break;
        }
        matcher_state.current = nullptr;
        report.matched_at = steady_now();
        publish(message);
    }

    void publish(const Outbound& message) noexcept {
        if (events.try_push(message)) {
            return;
        }
        ++matcher_state.waits;
        detail::Backoff backoff(config.wait == WaitStrategy::Spin);
        while (!events.try_push(message)) {
            backoff.pause();
        }
    }

    // Publisher thread.
    void run_publisher() noexcept {
        detail::prefer_performance_cores();
        std::array<Outbound, kBatchSize> batch;  // every member has a default initializer
        detail::Backoff backoff(config.wait == WaitStrategy::Spin);
        while (true) {
            const std::size_t count = events.try_pop_batch(batch);
            if (count == 0U) {
                backoff.pause();
                continue;
            }
            backoff.reset();
            for (std::size_t index = 0; index < count; ++index) {
                if (batch[index].stop) {
                    return;
                }
                const PipelineEvent& event = batch[index].event;
                ++publisher_state.events;
                if (event.kind == PipelineEvent::Kind::Trade) {
                    ++publisher_state.trades;
                } else if (event.kind == PipelineEvent::Kind::Report && !event.accepted) {
                    ++publisher_state.rejects;
                }
                if (consumer && !publisher_state.failure) {
                    try {
                        consumer(event);
                    } catch (...) {
                        publisher_state.failure = std::current_exception();
                    }
                }
            }
        }
    }

    PipelineConfig config;
    Consumer consumer;
    MatchingEngine engine;
    std::unique_ptr<JournalWriter> journal;
    JournaledEngine journaled;
    SpscRing<Inbound> requests;
    SpscRing<Outbound> events;
    std::thread matcher;
    std::thread publisher;
    bool finished{false};
    PipelineStats stats;  // assembled by finish()

    // Each thread's own state lives on its own cache lines, so updating it never disturbs another core.
    struct alignas(kCacheLineBytes) GatewayState {
        std::uint64_t requests{0};
        std::uint64_t waits{0};
    } gateway;

    struct alignas(kCacheLineBytes) MatcherState {
        const PipelineRequest* current{nullptr};  // the request being applied, to tag its market data
        std::uint64_t waits{0};
        std::exception_ptr failure;
    } matcher_state;

    struct alignas(kCacheLineBytes) PublisherState {
        std::uint64_t events{0};
        std::uint64_t trades{0};
        std::uint64_t rejects{0};
        std::exception_ptr failure;
    } publisher_state;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

Pipeline::Pipeline(PipelineConfig config, Consumer consumer)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(consumer))) {
    Impl* const impl = impl_.get();
    impl->publisher = std::thread([impl] { impl->run_publisher(); });
    try {
        impl->matcher = std::thread([impl] { impl->run_matcher(); });
    } catch (...) {
        impl->publish(Outbound{{}, true});  // nothing else produces events yet, so this thread may
        impl->publisher.join();
        throw;
    }
}

Pipeline::~Pipeline() {
    try {
        finish();
    } catch (const std::exception&) {
        // A destructor can't report the failure; finish() has already stopped both threads.
        impl_->finished = true;
    }
}

SymbolId Pipeline::symbol(std::string_view name) const {
    const auto symbol = impl_->engine.find_symbol(name);  // the symbol table never changes once threads start
    if (!symbol) {
        throw std::invalid_argument("unknown symbol " + std::string{name});
    }
    return *symbol;
}

bool Pipeline::try_send(const PipelineRequest& request) {
    Impl& impl = *impl_;
    if (impl.finished) {
        throw std::logic_error("the pipeline has finished");
    }
    if (!impl.requests.try_push(Inbound{request, false})) {
        return false;
    }
    ++impl.gateway.requests;
    return true;
}

void Pipeline::send(const PipelineRequest& request) {
    if (try_send(request)) {
        return;
    }
    ++impl_->gateway.waits;
    detail::Backoff backoff;
    while (!try_send(request)) {
        backoff.pause();
    }
}

void Pipeline::finish() {
    Impl& impl = *impl_;
    if (impl.finished) {
        return;
    }
    impl.finished = true;
    detail::Backoff backoff;
    while (!impl.requests.try_push(Inbound{{}, true})) {
        backoff.pause();
    }
    impl.matcher.join();
    impl.publisher.join();

    impl.stats.requests = impl.gateway.requests;
    impl.stats.gateway_waits = impl.gateway.waits;
    impl.stats.matcher_waits = impl.matcher_state.waits;
    impl.stats.events = impl.publisher_state.events;
    impl.stats.trades = impl.publisher_state.trades;
    impl.stats.rejects = impl.publisher_state.rejects;

    if (impl.matcher_state.failure) {
        std::rethrow_exception(impl.matcher_state.failure);
    }
    if (impl.publisher_state.failure) {
        std::rethrow_exception(impl.publisher_state.failure);
    }
    if (impl.journal) {
        impl.journal->close();
    }
}

const MatchingEngine& Pipeline::engine() const {
    if (!impl_->finished) {
        throw std::logic_error("the engine belongs to the matcher thread until finish()");
    }
    return impl_->engine;
}

const PipelineStats& Pipeline::stats() const {
    if (!impl_->finished) {
        throw std::logic_error("pipeline stats are complete only after finish()");
    }
    return impl_->stats;
}

}  // namespace tradematch
