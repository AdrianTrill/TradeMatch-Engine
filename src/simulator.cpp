#include "tradematch/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tradematch {

namespace {

// The standard distributions are implementation-defined and yield different sequences with libc++, libstdc++, and the
// MSVC STL. These portable versions, with floating-point contraction disabled for this file, make a seed replay the
// same market on every platform.
double unit_interval(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11U) * 0x1.0p-53;
}

// Irwin-Hall approximation of a standard normal: twelve uniforms sum to mean 6 and variance 1. It needs only
// correctly rounded additions, where Box-Muller's log and cos can differ in the last bit between math libraries.
double standard_normal(std::mt19937_64& rng) {
    double sum = 0.0;
    for (int index = 0; index < 12; ++index) {
        sum += unit_interval(rng);
    }
    return sum - 6.0;
}

// A large order an execution trader has been given to buy or sell on one symbol, which it works in child orders.
struct ParentOrder {
    Side side{Side::Buy};
    Quantity quantity{};  // 0 when there is none
    Quantity executed{};
    Timestamp start{};
    Timestamp end{};          // when its schedule has released all of it
    Price limit{};            // the worst price a child may trade at
    OrderId resting{};        // a passive child at the touch, if any
    Quantity resting_open{};  // its open quantity when last seen
};

}  // namespace

const char* to_string(SessionPhase phase) noexcept {
    switch (phase) {
        case SessionPhase::OpeningAuction:
            return "OPENING AUCTION";
        case SessionPhase::Continuous:
            return "CONTINUOUS";
        case SessionPhase::ClosingAuction:
            return "CLOSING AUCTION";
        case SessionPhase::Closed:
            return "CLOSED";
    }
    return "UNKNOWN";
}

struct MarketSimulator::Agent {
    enum class Kind : std::uint8_t {
        MarketMaker,
        Noise,
        Momentum,
        Whale,
        Execution
    };

    Kind kind{Kind::Noise};
    OwnerId owner{};
    double activity{};  // chance of acting on each symbol at each step
    Price half_spread{};
    Price level_gap{};
    std::size_t levels{};
    Quantity size{};
    std::vector<std::pair<std::size_t, OrderId>> live;  // (symbol index, order ID) of possibly live orders
    std::vector<std::vector<OrderId>> quotes;           // market makers: [symbol][side * levels + level]
    std::vector<ParentOrder> parents;                   // execution traders: [symbol]
};

MarketSimulator::MarketSimulator(SimulationConfig config, JournalWriter* journal)
    : config_(std::move(config)),
      rng_(config_.seed),
      journaled_(engine_, journal),
      feed_(std::make_unique<MarketDataFeed>(FeedConfig{1'000'000'000, 32, false})),
      latency_(std::make_unique<LatencyHistogram>()) {
    engine_.set_listener(feed_.get());

    BookCapacity capacity;
    capacity.orders = 16'384;
    capacity.price_levels = 1'024;
    capacity.trades_per_call = 256;
    for (std::size_t index = 0; index < config_.symbols.size(); ++index) {
        const SymbolId id = journaled_.add_symbol(config_.symbols[index], capacity);
        const auto start = static_cast<double>(config_.initial_price + static_cast<Price>(index) * 5'000);
        symbols_.push_back(Instrument{id, start, std::nullopt, std::nullopt, start, start});
    }

    OwnerId owner = 1;
    for (std::size_t index = 0; index < config_.market_makers; ++index) {
        Agent maker;
        maker.kind = Agent::Kind::MarketMaker;
        maker.owner = owner++;
        maker.activity = 0.35;
        maker.half_spread = 1 + static_cast<Price>(index % 3);
        maker.level_gap = 1 + static_cast<Price>(index % 2);
        maker.levels = 4;
        maker.size = static_cast<Quantity>(200 + 100 * index);
        maker.quotes.assign(symbols_.size(), std::vector<OrderId>(2 * maker.levels, 0));
        agents_.push_back(std::move(maker));
    }
    for (std::size_t index = 0; index < config_.noise_traders; ++index) {
        Agent noise;
        noise.kind = Agent::Kind::Noise;
        noise.owner = owner++;
        noise.activity = 0.03 + 0.003 * static_cast<double>(index % 10);
        noise.size = 100;
        agents_.push_back(std::move(noise));
    }
    for (std::size_t index = 0; index < config_.momentum_traders; ++index) {
        Agent momentum;
        momentum.kind = Agent::Kind::Momentum;
        momentum.owner = owner++;
        momentum.activity = 0.02;
        momentum.size = 300;
        agents_.push_back(std::move(momentum));
    }
    for (std::size_t index = 0; index < config_.execution_traders; ++index) {
        Agent execution;
        execution.kind = Agent::Kind::Execution;
        execution.owner = owner++;
        execution.activity = 1.0;  // it looks at every step and times its children itself
        execution.size = 100;
        execution.parents.assign(symbols_.size(), ParentOrder{});
        agents_.push_back(std::move(execution));
    }
    Agent whale;
    whale.kind = Agent::Kind::Whale;
    whale.owner = owner;
    whale.activity = 0.0015;
    whale.size = 5'000;
    agents_.push_back(std::move(whale));

    enter_phase(SessionPhase::OpeningAuction);
}

MarketSimulator::~MarketSimulator() = default;

Price MarketSimulator::fair_value(std::size_t index) const {
    return tick(symbols_.at(index).fair);
}

std::optional<Price> MarketSimulator::previous_close(std::size_t index) const {
    return symbols_.at(index).previous_close;
}

std::optional<Price> MarketSimulator::day_open(std::size_t index) const {
    return symbols_.at(index).open;
}

void MarketSimulator::run_for(Timestamp duration) {
    const Timestamp until = now_ + duration;
    while (now_ < until) {
        step();
    }
}

void MarketSimulator::step() {
    now_ += config_.step;
    feed_->set_time(now_);
    journaled_.set_time(now_);

    for (auto& instrument : symbols_) {
        instrument.fair += config_.volatility * standard_normal(rng_);
        if (chance(config_.news_probability)) {
            const auto jump = static_cast<double>(uniform(15, 45));
            instrument.fair += chance(0.5) ? jump : -jump;
        }
        instrument.fair = std::max(instrument.fair, 100.0);
        if (const auto last = feed_->last_price(instrument.id)) {
            instrument.fast_average = 0.2 * static_cast<double>(*last) + 0.8 * instrument.fast_average;
            instrument.slow_average = 0.02 * static_cast<double>(*last) + 0.98 * instrument.slow_average;
        }
    }

    if (phase_ != SessionPhase::Closed) {
        for (auto& agent : agents_) {
            for (std::size_t index = 0; index < symbols_.size(); ++index) {
                if (chance(agent.activity)) {
                    act(agent, index);
                }
            }
            if (agent.live.size() > 96) {
                prune(agent);
            }
        }
    }

    if (now_ >= phase_end_) {
        switch (phase_) {
            case SessionPhase::OpeningAuction:
                enter_phase(SessionPhase::Continuous);
                break;
            case SessionPhase::Continuous:
                enter_phase(SessionPhase::ClosingAuction);
                break;
            case SessionPhase::ClosingAuction:
                enter_phase(SessionPhase::Closed);
                break;
            case SessionPhase::Closed:
                enter_phase(SessionPhase::OpeningAuction);
                break;
        }
    }
}

void MarketSimulator::enter_phase(SessionPhase phase) {
    const auto uncross_all = [this] {
        for (auto& instrument : symbols_) {
            const auto result = journaled_.uncross(instrument.id);
            if (result.executed && phase_ == SessionPhase::Continuous) {
                instrument.open = result.price;
            }
            stats_.auctions += result.executed ? 1U : 0U;
            stats_.trades += result.trades.size();
            stats_.stops_triggered += result.triggered_stops.size();
        }
    };
    const auto begin_auctions = [this] {
        for (const auto& instrument : symbols_) {
            journaled_.begin_auction(instrument.id);
        }
    };

    phase_ = phase;
    switch (phase) {
        case SessionPhase::OpeningAuction:
            ++day_;
            for (auto& instrument : symbols_) {
                instrument.open.reset();
            }
            begin_auctions();
            phase_end_ = now_ + config_.opening_auction;
            break;
        case SessionPhase::Continuous:
            uncross_all();
            phase_end_ = now_ + config_.continuous;
            break;
        case SessionPhase::ClosingAuction:
            begin_auctions();
            phase_end_ = now_ + config_.closing_auction;
            break;
        case SessionPhase::Closed:
            uncross_all();
            for (auto& instrument : symbols_) {
                instrument.previous_close = feed_->last_price(instrument.id);
            }
            journaled_.expire_day_orders();
            for (auto& agent : agents_) {
                prune(agent);
            }
            phase_end_ = now_ + config_.closed;
            break;
    }
}

void MarketSimulator::act(Agent& agent, std::size_t index) {
    switch (agent.kind) {
        case Agent::Kind::MarketMaker:
            quote(agent, index);
            break;
        case Agent::Kind::Noise:
            trade_noise(agent, index);
            break;
        case Agent::Kind::Momentum:
            trade_momentum(agent, index);
            break;
        case Agent::Kind::Whale:
            trade_whale(agent, index);
            break;
        case Agent::Kind::Execution:
            trade_execution(agent, index);
            break;
    }
}

// Market makers keep `levels` quotes on each side around fair value and move each one only when its price is stale.
// In continuous trading the quotes are post-only, so a maker never takes liquidity by accident.
void MarketSimulator::quote(Agent& agent, std::size_t index) {
    const Instrument& instrument = symbols_[index];
    const OrderBook& book = engine_.book(instrument.id);
    const bool auction = phase_ != SessionPhase::Continuous;

    for (std::size_t side_index = 0; side_index < 2; ++side_index) {
        const Side side = side_index == 0 ? Side::Buy : Side::Sell;
        for (std::size_t level = 0; level < agent.levels; ++level) {
            const double distance = static_cast<double>(agent.half_spread) +
                                    static_cast<double>(level) * static_cast<double>(agent.level_gap);
            Price price = tick(side == Side::Buy ? instrument.fair - distance : instrument.fair + distance);
            if (!auction) {
                // Never quote through the opposite touch: post-only quotes would be rejected, and real makers join
                // or improve the spread instead.
                if (const auto best_ask = book.best_ask(); side == Side::Buy && best_ask) {
                    price = std::min(price, *best_ask - 1);
                }
                if (const auto best_bid = book.best_bid(); side == Side::Sell && best_bid) {
                    price = std::max(price, *best_bid + 1);
                }
                price = std::max<Price>(price, 1);
            }
            OrderId& slot = agent.quotes[index][side_index * agent.levels + level];
            const auto size = static_cast<Quantity>(agent.size + 100 * uniform(0, 3));

            if (slot != 0) {
                const auto current = book.get_order(slot);
                if (current && current->price == price) {
                    continue;
                }
                if (current) {
                    OrderRequest move{slot, side, OrderType::Limit, size, price, TimeInForce::Day, !auction};
                    move.owner = agent.owner;
                    const auto start = std::chrono::steady_clock::now();
                    const auto result = journaled_.replace(instrument.id, move);
                    record_latency(start);
                    ++stats_.requests;
                    stats_.trades += result.submit_result.trades.size();
                    if (result.replaced && result.submit_result.rested) {
                        continue;
                    }
                    if (!result.replaced) {
                        ++stats_.rejects;
                        ++stats_.rejects_by_reason[result.reject_reason];
                    }
                }
                slot = 0;
            }

            const OrderRequest order{0, side, OrderType::Limit, size, price, TimeInForce::Day, !auction};
            const OrderId order_id = next_order_id_;
            send(agent, index, order);
            if (book.has_order(order_id)) {
                slot = order_id;
            }
        }
    }
}

void MarketSimulator::trade_noise(Agent& agent, std::size_t index) {
    const Instrument& instrument = symbols_[index];
    const OrderBook& book = engine_.book(instrument.id);
    const bool auction = phase_ != SessionPhase::Continuous;
    const Side side = chance(0.5) ? Side::Buy : Side::Sell;
    const auto size = static_cast<Quantity>(agent.size * uniform(1, 5));
    const auto roll = uniform(0, 99);

    if (roll < 55) {
        // Passive: join or sit a few ticks behind fair value on its own side.
        const auto offset = static_cast<double>(1 + uniform(0, 6));
        const Price price = tick(side == Side::Buy ? instrument.fair - offset : instrument.fair + offset);
        const TimeInForce time_in_force = chance(0.7) ? TimeInForce::Day : TimeInForce::GoodTillCancel;
        send(agent, index, OrderRequest{0, side, OrderType::Limit, size, price, time_in_force});
        return;
    }
    if (roll < 75) {
        // Marketable: cross the touch by up to two ticks, as IOC in continuous trading.
        const auto touch = side == Side::Buy ? book.best_ask() : book.best_bid();
        const Price reference = touch.value_or(tick(instrument.fair));
        const auto through = static_cast<Price>(uniform(0, 2));
        const Price price = std::max<Price>(1, side == Side::Buy ? reference + through : reference - through);
        const TimeInForce time_in_force = auction ? TimeInForce::Day : TimeInForce::ImmediateOrCancel;
        send(agent, index, OrderRequest{0, side, OrderType::Limit, size, price, time_in_force});
        return;
    }
    if (roll < 85) {
        send(agent,
             index,
             OrderRequest{0, side, OrderType::Market, static_cast<Quantity>(agent.size * uniform(1, 3)), 0});
        return;
    }
    // Cancel one of its own orders on this symbol.
    for (const auto& [symbol_index, order_id] : agent.live) {
        if (symbol_index == index && chance(0.5)) {
            cancel(agent, index, order_id);
            return;
        }
    }
}

void MarketSimulator::trade_momentum(Agent& agent, std::size_t index) {
    if (phase_ != SessionPhase::Continuous) {
        return;
    }
    const Instrument& instrument = symbols_[index];
    const double signal = instrument.fast_average - instrument.slow_average;
    if (std::abs(signal) < 1.5) {
        return;
    }
    const Side side = signal > 0.0 ? Side::Buy : Side::Sell;
    const OrderBook& book = engine_.book(instrument.id);
    const auto touch = side == Side::Buy ? book.best_ask() : book.best_bid();
    if (!touch) {
        return;
    }
    const Price price = side == Side::Buy ? *touch + 2 : std::max<Price>(1, *touch - 2);
    const auto size = static_cast<Quantity>(agent.size + 100 * uniform(0, 3));
    send(agent, index, OrderRequest{0, side, OrderType::Limit, size, price, TimeInForce::ImmediateOrCancel});
}

// Occasionally works a large iceberg near the touch or parks a stop order beyond the market.
void MarketSimulator::trade_whale(Agent& agent, std::size_t index) {
    const Instrument& instrument = symbols_[index];
    const Side side = chance(0.5) ? Side::Buy : Side::Sell;
    if (chance(0.5)) {
        const auto behind = static_cast<double>(1 + uniform(0, 2));
        OrderRequest order{0,
                           side,
                           OrderType::Limit,
                           static_cast<Quantity>(agent.size + 1'000 * uniform(0, 10)),
                           tick(side == Side::Buy ? instrument.fair - behind : instrument.fair + behind),
                           TimeInForce::Day};
        order.display_quantity = static_cast<Quantity>(300 + 100 * uniform(0, 2));
        send(agent, index, order);
        return;
    }
    const auto distance = static_cast<double>(8 + uniform(0, 12));
    const Price trigger = tick(side == Side::Buy ? instrument.fair + distance : instrument.fair - distance);
    const bool stop_limit = chance(0.5);
    OrderRequest order{0,
                       side,
                       stop_limit ? OrderType::StopLimit : OrderType::Stop,
                       static_cast<Quantity>(300 + 100 * uniform(0, 12)),
                       0,
                       TimeInForce::Day};
    order.stop_price = trigger;
    if (stop_limit) {
        order.price = side == Side::Buy ? trigger + 5 : std::max<Price>(1, trigger - 5);
    }
    send(agent, index, order);
}

// Now and then an execution trader is given a large parent order on a random side, and slices it into child orders
// the size of other agents' orders. A schedule releases the parent evenly over its horizon. Every 100 ms or so the
// trader looks at the book, keeps one child waiting at the best price on its own side, and catches up with the
// schedule with marketable IOC children at or through the touch, which do most of the work. Runs of children on one
// side, for as long as a parent lasts, make order flow persistent, as algorithms slicing large orders make real order
// flow.
void MarketSimulator::trade_execution(Agent& agent, std::size_t index) {
    constexpr Timestamp kSecond = 1'000'000'000;
    ParentOrder& parent = agent.parents[index];
    const Instrument& instrument = symbols_[index];
    const OrderBook& book = engine_.book(instrument.id);

    std::optional<RestingOrderView> resting;
    if (parent.resting != 0) {
        // Fills of the passive child since the last look count toward the parent.
        resting = book.get_order(parent.resting);
        const Quantity open = resting ? resting->remaining_quantity : 0;
        parent.executed += parent.resting_open - open;
        parent.resting_open = open;
        if (!resting) {
            parent.resting = 0;
        }
    }
    const bool continuous = phase_ == SessionPhase::Continuous;
    if (parent.quantity != 0 && (!continuous || parent.executed == parent.quantity || now_ >= parent.end + kSecond)) {
        // Done, a second past its schedule, or out of continuous trading: whatever is left is abandoned.
        if (parent.resting != 0) {
            cancel(agent, index, parent.resting);
        }
        parent = ParentOrder{};
        return;
    }
    if (!continuous) {
        return;
    }

    if (parent.quantity == 0) {
        // After about two seconds without one, a parent of 3,000 to 30,000 shares to work at 6,000 to 12,000 shares a
        // second, over one to five seconds, at prices no more than 20 ticks worse than the touch on arrival.
        if (phase_time_remaining() < kSecond || !chance(0.005)) {
            return;
        }
        const Side side = chance(0.5) ? Side::Buy : Side::Sell;
        const auto quantity = static_cast<Quantity>(100 * uniform(30, 300));
        const std::uint64_t pace = 100 * uniform(60, 120);
        const auto horizon = std::max(static_cast<Timestamp>(quantity * 1'000'000'000ULL / pace), kSecond);
        const auto touch = side == Side::Buy ? book.best_ask() : book.best_bid();
        const Price arrival = touch.value_or(tick(instrument.fair));
        parent = ParentOrder{side, quantity, 0, now_, now_ + std::min(horizon, phase_time_remaining()), 0, 0, 0};
        parent.limit = side == Side::Buy ? arrival + 20 : std::max<Price>(1, arrival - 20);
        return;
    }
    if (!chance(0.1)) {
        return;
    }

    const bool buy = parent.side == Side::Buy;
    const auto best = buy ? book.best_bid() : book.best_ask();
    if (resting && (now_ >= parent.end || resting->price != best.value_or(resting->price))) {
        // The market has moved away from the passive child, or the schedule is over and the rest must be taken.
        cancel(agent, index, parent.resting);
        parent.resting = 0;
        parent.resting_open = 0;
    }
    if (parent.resting == 0 && now_ < parent.end && best && (buy ? *best <= parent.limit : *best >= parent.limit)) {
        // One child waits at the best price on its own side, for the other side to come to it.
        const auto size =
            std::min(static_cast<Quantity>(agent.size * uniform(1, 2)), parent.quantity - parent.executed);
        const OrderId order_id = next_order_id_;
        parent.executed +=
            send(agent, index, OrderRequest{0, parent.side, OrderType::Limit, size, *best, TimeInForce::Day});
        if (const auto child = book.get_order(order_id)) {
            parent.resting = order_id;
            parent.resting_open = child->remaining_quantity;
        }
    }

    // Catch up with the schedule, a child at a time, while the touch holds out within the limit.
    const auto elapsed = static_cast<std::uint64_t>(now_ - parent.start);
    const auto horizon = static_cast<std::uint64_t>(parent.end - parent.start);
    const auto due = elapsed < horizon ? static_cast<Quantity>(parent.quantity * elapsed / horizon) : parent.quantity;
    for (int child = 0; child < 20 && parent.executed + parent.resting_open < due; ++child) {
        const auto touch = buy ? book.best_ask() : book.best_bid();
        if (!touch || (buy ? *touch > parent.limit : *touch < parent.limit)) {
            break;
        }
        const auto through = static_cast<Price>(uniform(0, 1));
        const Price price = buy ? std::min(*touch + through, parent.limit) : std::max(*touch - through, parent.limit);
        const auto size = std::min(static_cast<Quantity>(agent.size * uniform(1, 2)),
                                   parent.quantity - parent.executed - parent.resting_open);
        const Quantity filled =
            send(agent,
                 index,
                 OrderRequest{0, parent.side, OrderType::Limit, size, price, TimeInForce::ImmediateOrCancel});
        parent.executed += filled;
        if (filled < size) {
            break;
        }
    }
}

Quantity MarketSimulator::send(Agent& agent, std::size_t index, OrderRequest order) {
    order.order_id = next_order_id_++;
    order.owner = agent.owner;
    order.self_trade_prevention = SelfTradePrevention::CancelOldest;
    const SubmitResult result = place(index, order);
    if (result.rested || result.pending_trigger) {
        agent.live.emplace_back(index, order.order_id);
    }
    return result.filled_quantity;
}

// Sends an order to the exchange, timed, and counted in the stats.
SubmitResult MarketSimulator::place(std::size_t index, const OrderRequest& order) {
    const auto start = std::chrono::steady_clock::now();
    const SubmitResult result = journaled_.submit(symbols_[index].id, order);
    record_latency(start);

    ++stats_.requests;
    if (!result.accepted) {
        ++stats_.rejects;
        ++stats_.rejects_by_reason[result.reject_reason];
    }
    stats_.trades += result.trades.size();
    stats_.stops_triggered += result.triggered_stops.size();
    stats_.self_trade_cancels += result.self_trade_cancels.size();
    return result;
}

OwnerId MarketSimulator::participant_owner() const noexcept {
    return static_cast<OwnerId>(agents_.size() + 1);  // agents own 1 through agents_.size()
}

void MarketSimulator::check_outside_owner(OwnerId owner) const {
    if (owner != 0 && owner < participant_owner()) {
        throw std::invalid_argument("an outside order can't use an agent's owner ID");
    }
}

SubmitResult MarketSimulator::submit(std::size_t symbol_index, OrderRequest order) {
    check_outside_owner(order.owner);
    if (symbol_index >= symbols_.size()) {
        throw std::out_of_range("no symbol has this index");
    }
    order.order_id = next_order_id_++;
    const SubmitResult result = place(symbol_index, order);
    if (result.accepted) {
        outside_orders_.insert(order.order_id);
    }
    return result;
}

CancelResult MarketSimulator::cancel(std::size_t symbol_index, OrderId order_id) {
    const SymbolId symbol = symbols_.at(symbol_index).id;
    ++stats_.requests;
    if (!outside_orders_.contains(order_id)) {
        ++stats_.rejects;
        ++stats_.rejects_by_reason[RejectReason::UnknownOrder];
        return CancelResult{order_id, false, RejectReason::UnknownOrder, 0, "no outside order has this ID"};
    }
    const auto start = std::chrono::steady_clock::now();
    const CancelResult result = journaled_.cancel(symbol, order_id);
    record_latency(start);
    if (!result.cancelled) {
        ++stats_.rejects;
        ++stats_.rejects_by_reason[result.reject_reason];
    }
    return result;
}

ReplaceResult MarketSimulator::replace(std::size_t symbol_index, const OrderRequest& replacement) {
    check_outside_owner(replacement.owner);
    const SymbolId symbol = symbols_.at(symbol_index).id;
    ++stats_.requests;
    if (!outside_orders_.contains(replacement.order_id)) {
        ++stats_.rejects;
        ++stats_.rejects_by_reason[RejectReason::UnknownOrder];
        ReplaceResult rejected;
        rejected.order_id = replacement.order_id;
        rejected.reject_reason = RejectReason::UnknownOrder;
        rejected.message = "no outside order has this ID";
        return rejected;
    }
    const auto start = std::chrono::steady_clock::now();
    const ReplaceResult result = journaled_.replace(symbol, replacement);
    record_latency(start);
    if (!result.replaced) {
        ++stats_.rejects;
        ++stats_.rejects_by_reason[result.reject_reason];
    }
    stats_.trades += result.submit_result.trades.size();
    stats_.stops_triggered += result.submit_result.triggered_stops.size();
    stats_.self_trade_cancels += result.submit_result.self_trade_cancels.size();
    return result;
}

void MarketSimulator::cancel(Agent& agent, std::size_t index, OrderId order_id) {
    const auto start = std::chrono::steady_clock::now();
    journaled_.cancel(symbols_[index].id, order_id);
    record_latency(start);
    ++stats_.requests;
    std::erase_if(agent.live, [&](const auto& live) { return live.first == index && live.second == order_id; });
}

// Forgets orders that have filled, been cancelled, or expired.
void MarketSimulator::prune(Agent& agent) {
    std::erase_if(agent.live, [this](const auto& live) {
        const OrderBook& book = engine_.book(symbols_[live.first].id);
        return !book.has_order(live.second) && !book.has_stop_order(live.second);
    });
    for (std::size_t index = 0; index < agent.quotes.size(); ++index) {
        for (auto& slot : agent.quotes[index]) {
            if (slot != 0 && !engine_.book(symbols_[index].id).has_order(slot)) {
                slot = 0;
            }
        }
    }
}

void MarketSimulator::record_latency(std::chrono::steady_clock::time_point start) {
    if (config_.measure_latency) {
        latency_->record(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
    }
}

Price MarketSimulator::tick(double price) noexcept {
    return std::max<Price>(1, static_cast<Price>(std::llround(price)));
}

bool MarketSimulator::chance(double probability) {
    return unit_interval(rng_) < probability;
}

std::uint64_t MarketSimulator::uniform(std::uint64_t low, std::uint64_t high) {
    const std::uint64_t span = high - low + 1U;
    // Redrawing values from the final partial block of `span` keeps every result equally likely.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t limit = kMax - (kMax % span);
    std::uint64_t value = rng_();
    while (value >= limit) {
        value = rng_();
    }
    return low + (value % span);
}

}  // namespace tradematch
