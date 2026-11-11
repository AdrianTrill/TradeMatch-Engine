#include "workload.hpp"

#include "tradematch/order_book.hpp"

#include <algorithm>
#include <random>
#include <unordered_map>

namespace tradematch::bench {

namespace {

struct LiveOrder {
    OrderId order_id{};
    Side side{Side::Buy};
};

// Resting orders the generator may cancel or modify, with O(1) insert, erase, and uniform random pick.
class LiveOrderSet {
public:
    void insert(OrderId order_id, Side side) {
        if (positions_.contains(order_id)) {
            return;
        }
        positions_.emplace(order_id, orders_.size());
        orders_.push_back(LiveOrder{order_id, side});
    }

    void erase(OrderId order_id) {
        const auto it = positions_.find(order_id);
        if (it == positions_.end()) {
            return;
        }
        const auto position = it->second;
        positions_.erase(it);
        if (position + 1 != orders_.size()) {
            orders_[position] = orders_.back();
            positions_[orders_[position].order_id] = position;
        }
        orders_.pop_back();
    }

    bool empty() const noexcept {
        return orders_.empty();
    }

    std::size_t size() const noexcept {
        return orders_.size();
    }

    const LiveOrder& at(std::size_t position) const {
        return orders_.at(position);
    }

private:
    std::vector<LiveOrder> orders_;
    std::unordered_map<OrderId, std::size_t> positions_;
};

class Generator {
public:
    explicit Generator(const WorkloadConfig& config)
        : config_(config), rng_(config.seed), last_mid_(config.start_mid) {}

    Workload generate() {
        Workload workload;
        workload.commands.reserve(config_.prefill_orders + config_.warmup_commands + config_.measured_commands);

        for (std::size_t index = 0; index < config_.prefill_orders; ++index) {
            emit(make_add(), workload);
        }
        for (std::size_t index = 0; index < config_.warmup_commands; ++index) {
            emit(next_command(), workload);
        }

        workload.measured_begin = workload.commands.size();
        for (std::size_t index = 0; index < config_.measured_commands; ++index) {
            const auto command = next_command();
            ++workload.measured_mix.at(static_cast<std::size_t>(command.kind));
            emit(command, workload);
        }
        return workload;
    }

private:
    std::uint64_t uniform(std::uint64_t low, std::uint64_t high) {
        return std::uniform_int_distribution<std::uint64_t>(low, high)(rng_);
    }

    bool chance(unsigned percent) {
        return uniform(0, 99) < percent;
    }

    // Ticks behind the touch follow a geometric distribution, so most liquidity sits near the top of the book.
    Price depth_offset() {
        Price offset = 0;
        while (offset < 50 && chance(55)) {
            ++offset;
        }
        return offset;
    }

    Quantity lot_size() {
        if (chance(20)) {
            return static_cast<Quantity>(uniform(1, 99));  // odd lot
        }
        return static_cast<Quantity>(100 * uniform(1, 10));  // round lot
    }

    Price touch(Side side) const {
        const auto bid = scratch_.best_bid();
        const auto ask = scratch_.best_ask();
        if (side == Side::Buy) {
            if (bid) {
                return *bid;
            }
            return ask ? *ask - 1 : last_mid_ - 1;
        }
        if (ask) {
            return *ask;
        }
        return bid ? *bid + 1 : last_mid_ + 1;
    }

    // A price that never crosses the spread: occasionally improves the touch inside a wide spread, otherwise joins
    // the best price or sits a few ticks behind it.
    Price passive_price(Side side) {
        const Price bid = touch(Side::Buy);
        const Price ask = touch(Side::Sell);
        const bool wide_spread = ask - bid > 1;
        if (side == Side::Buy) {
            const Price price = wide_spread && chance(15) ? bid + 1 : bid - depth_offset();
            return std::max<Price>(1, price);
        }
        return wide_spread && chance(15) ? ask - 1 : ask + depth_offset();
    }

    Command make_add() {
        Command command;
        command.kind = CommandKind::Add;
        command.order.order_id = next_order_id_++;
        command.order.side = chance(50) ? Side::Buy : Side::Sell;
        command.order.type = OrderType::Limit;
        command.order.quantity = lot_size();
        command.order.price = passive_price(command.order.side);
        return command;
    }

    Command make_cancel() {
        const auto& victim = live_.at(uniform(0, live_.size() - 1));
        Command command;
        command.kind = CommandKind::Cancel;
        command.order.order_id = victim.order_id;
        command.order.side = victim.side;
        return command;
    }

    Command make_replace() {
        const auto& target = live_.at(uniform(0, live_.size() - 1));
        Command command;
        command.kind = CommandKind::Replace;
        command.order.order_id = target.order_id;
        command.order.side = target.side;
        command.order.type = OrderType::Limit;
        command.order.quantity = lot_size();
        command.order.price = passive_price(target.side);
        return command;
    }

    // Takes liquidity: mostly marketable limits that can sweep a few levels past the touch, sometimes market orders.
    Command make_aggress() {
        Command command;
        command.kind = CommandKind::Aggress;
        command.order.order_id = next_order_id_++;
        command.order.side = chance(50) ? Side::Buy : Side::Sell;
        command.order.quantity = static_cast<Quantity>(100 * uniform(1, 8));

        if (chance(20)) {
            command.order.type = OrderType::Market;
            command.order.price = 0;
            return command;
        }

        command.order.type = OrderType::Limit;
        const auto sweep = static_cast<Price>(uniform(0, 3));
        if (command.order.side == Side::Buy) {
            command.order.price = touch(Side::Sell) + sweep;
        } else {
            command.order.price = std::max<Price>(1, touch(Side::Buy) - sweep);
        }
        return command;
    }

    Command next_command() {
        if (live_.empty()) {
            return make_add();
        }
        const auto roll = uniform(0, 99);
        if (roll < config_.add_percent) {
            // Real books are roughly stationary: above the target depth, a growing share of adds become cancels.
            const auto target = std::max<std::size_t>(config_.prefill_orders, 1'000);
            if (live_.size() > target) {
                const auto excess_percent = (live_.size() - target) * 100 / (target / 20);
                if (chance(static_cast<unsigned>(std::min<std::size_t>(excess_percent, 100)))) {
                    return make_cancel();
                }
            }
            return make_add();
        }
        if (roll < config_.add_percent + config_.cancel_percent) {
            return make_cancel();
        }
        if (roll < config_.add_percent + config_.cancel_percent + config_.replace_percent) {
            return make_replace();
        }
        return make_aggress();
    }

    // Applies the command to the scratch book and updates the live-order set from what actually happened.
    void emit(const Command& command, Workload& workload) {
        workload.commands.push_back(command);

        switch (command.kind) {
            case CommandKind::Cancel:
                scratch_.cancel(command.order.order_id);
                live_.erase(command.order.order_id);
                break;
            case CommandKind::Replace: {
                const auto result = scratch_.replace(command.order);
                live_.erase(command.order.order_id);
                track(result.submit_result, command.order.side);
                break;
            }
            case CommandKind::Add:
            case CommandKind::Aggress:
                track(scratch_.submit(command.order), command.order.side);
                break;
        }

        const auto bid = scratch_.best_bid();
        const auto ask = scratch_.best_ask();
        if (bid && ask) {
            last_mid_ = (*bid + *ask) / 2;
        }
    }

    void track(const SubmitResult& result, Side side) {
        for (const auto& trade : result.trades) {
            const OrderId resting = trade.aggressor_side == Side::Buy ? trade.sell_order_id : trade.buy_order_id;
            if (!scratch_.has_order(resting)) {
                live_.erase(resting);
            }
        }
        if (result.rested) {
            live_.insert(result.order_id, side);
        }
    }

    WorkloadConfig config_;
    std::mt19937_64 rng_;
    OrderBook scratch_;
    LiveOrderSet live_;
    OrderId next_order_id_{1};
    Price last_mid_;
};

}  // namespace

Workload generate_workload(const WorkloadConfig& config) {
    return Generator(config).generate();
}

}  // namespace tradematch::bench
