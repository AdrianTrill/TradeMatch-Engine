#ifndef TRADEMATCH_TESTS_DIFFERENTIAL_HPP
#define TRADEMATCH_TESTS_DIFFERENTIAL_HPP

#include "support/reference_book.hpp"
#include "tradematch/order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tradematch::testing {

enum class OpKind : std::uint8_t { Submit, Cancel, Replace };

struct Operation {
    OpKind kind{OpKind::Submit};
    OrderRequest order;  // Cancel only uses order.order_id.
};

inline std::string describe(const Operation& op) {
    std::ostringstream out;
    switch (op.kind) {
        case OpKind::Submit:
            out << "SUBMIT ";
            break;
        case OpKind::Cancel:
            out << "CANCEL id=" << op.order.order_id;
            return out.str();
        case OpKind::Replace:
            out << "REPLACE ";
            break;
    }
    out << "id=" << op.order.order_id << ' ' << to_string(op.order.side) << ' ' << to_string(op.order.type)
        << " qty=" << op.order.quantity << " price=" << op.order.price;
    return out.str();
}

// Runs every operation against both the optimized engine and the naive reference, and reports the first
// observable difference: a result field, a trade, the visible book, or an order lookup.
class DifferentialHarness {
public:
    struct Stats {
        std::size_t operations{0};
        std::size_t trades{0};
        std::size_t rejections{0};
        std::size_t cancels{0};
        std::size_t replaces{0};
        std::size_t max_resting_orders{0};
    };

    // Returns an empty string when both books agree, otherwise a description of the first mismatch.
    std::string apply(const Operation& op) {
        std::string mismatch;
        switch (op.kind) {
            case OpKind::Submit: {
                const auto result = engine_.submit(op.order);
                mismatch = compare_submit(result, reference_.submit(op.order), "submit");
                stats_.trades += result.trades.size();
                stats_.rejections += result.accepted ? 0U : 1U;
                break;
            }
            case OpKind::Cancel: {
                const auto result = engine_.cancel(op.order.order_id);
                mismatch = compare_cancel(result, reference_.cancel(op.order.order_id));
                stats_.cancels += result.cancelled ? 1U : 0U;
                break;
            }
            case OpKind::Replace: {
                const auto result = engine_.replace(op.order);
                mismatch = compare_replace(result, reference_.replace(op.order));
                stats_.replaces += result.replaced ? 1U : 0U;
                stats_.trades += result.submit_result.trades.size();
                break;
            }
        }

        if (mismatch.empty()) {
            mismatch = compare_books(op.order.order_id);
        }
        ++stats_.operations;
        stats_.max_resting_orders = std::max(stats_.max_resting_orders, engine_.order_count());
        return mismatch;
    }

    const Stats& stats() const noexcept {
        return stats_;
    }

private:
    template <typename T>
    static std::string field(const char* name, const T& engine, const T& reference) {
        std::ostringstream out;
        out << name << ": engine=" << engine << " reference=" << reference;
        return out.str();
    }

    static std::string compare_trade(const Trade& engine, const Trade& reference, std::size_t index) {
        const auto prefix = "trade[" + std::to_string(index) + "].";
        if (engine.trade_id != reference.trade_id) {
            return field((prefix + "trade_id").c_str(), engine.trade_id, reference.trade_id);
        }
        if (engine.buy_order_id != reference.buy_order_id) {
            return field((prefix + "buy_order_id").c_str(), engine.buy_order_id, reference.buy_order_id);
        }
        if (engine.sell_order_id != reference.sell_order_id) {
            return field((prefix + "sell_order_id").c_str(), engine.sell_order_id, reference.sell_order_id);
        }
        if (engine.price != reference.price) {
            return field((prefix + "price").c_str(), engine.price, reference.price);
        }
        if (engine.quantity != reference.quantity) {
            return field((prefix + "quantity").c_str(), engine.quantity, reference.quantity);
        }
        if (engine.aggressor_side != reference.aggressor_side) {
            return prefix + "aggressor_side differs";
        }
        return {};
    }

    static std::string compare_submit(const SubmitResult& engine, const SubmitResult& reference, const char* what) {
        const auto prefix = std::string(what) + ".";
        if (engine.accepted != reference.accepted) {
            return field((prefix + "accepted").c_str(), engine.accepted, reference.accepted);
        }
        if (!engine.accepted) {
            return {};
        }
        if (engine.rested != reference.rested) {
            return field((prefix + "rested").c_str(), engine.rested, reference.rested);
        }
        if (engine.expired != reference.expired) {
            return field((prefix + "expired").c_str(), engine.expired, reference.expired);
        }
        if (engine.filled_quantity != reference.filled_quantity) {
            return field((prefix + "filled").c_str(), engine.filled_quantity, reference.filled_quantity);
        }
        if (engine.remaining_quantity != reference.remaining_quantity) {
            return field((prefix + "remaining").c_str(), engine.remaining_quantity, reference.remaining_quantity);
        }
        if (engine.trades.size() != reference.trades.size()) {
            return field((prefix + "trade_count").c_str(), engine.trades.size(), reference.trades.size());
        }
        for (std::size_t index = 0; index < engine.trades.size(); ++index) {
            auto mismatch = compare_trade(engine.trades[index], reference.trades[index], index);
            if (!mismatch.empty()) {
                return prefix + mismatch;
            }
        }
        return {};
    }

    static std::string compare_cancel(const CancelResult& engine, const CancelResult& reference) {
        if (engine.cancelled != reference.cancelled) {
            return field("cancel.cancelled", engine.cancelled, reference.cancelled);
        }
        if (engine.cancelled_quantity != reference.cancelled_quantity) {
            return field("cancel.quantity", engine.cancelled_quantity, reference.cancelled_quantity);
        }
        return {};
    }

    static std::string compare_replace(const ReplaceResult& engine, const ReplaceResult& reference) {
        if (engine.replaced != reference.replaced) {
            return field("replace.replaced", engine.replaced, reference.replaced);
        }
        if (!engine.replaced) {
            return {};
        }
        if (engine.previous_remaining_quantity != reference.previous_remaining_quantity) {
            return field("replace.previous_remaining",
                         engine.previous_remaining_quantity,
                         reference.previous_remaining_quantity);
        }
        return compare_submit(engine.submit_result, reference.submit_result, "replace.submit");
    }

    static std::string compare_levels(const std::vector<PriceLevelView>& engine,
                                      const std::vector<PriceLevelView>& reference,
                                      const char* side) {
        const auto prefix = std::string(side) + ".";
        if (engine.size() != reference.size()) {
            return field((prefix + "level_count").c_str(), engine.size(), reference.size());
        }
        for (std::size_t index = 0; index < engine.size(); ++index) {
            const auto& left = engine[index];
            const auto& right = reference[index];
            const auto level = prefix + "level[" + std::to_string(index) + "].";
            if (left.price != right.price) {
                return field((level + "price").c_str(), left.price, right.price);
            }
            if (left.total_quantity != right.total_quantity) {
                return field((level + "total_quantity").c_str(), left.total_quantity, right.total_quantity);
            }
            if (left.order_count != right.order_count) {
                return field((level + "order_count").c_str(), left.order_count, right.order_count);
            }
            if (left.order_ids != right.order_ids) {
                return level + "FIFO order ids differ";
            }
        }
        return {};
    }

    std::string compare_books(OrderId touched_order_id) const {
        const auto engine_snapshot = engine_.snapshot();
        const auto reference_snapshot = reference_.snapshot();

        auto mismatch = compare_levels(engine_snapshot.bids, reference_snapshot.bids, "bids");
        if (mismatch.empty()) {
            mismatch = compare_levels(engine_snapshot.asks, reference_snapshot.asks, "asks");
        }
        if (!mismatch.empty()) {
            return mismatch;
        }

        if (engine_.order_count() != reference_.order_count()) {
            return field("order_count", engine_.order_count(), reference_.order_count());
        }

        const auto engine_order = engine_.get_order(touched_order_id);
        const auto reference_order = reference_.get_order(touched_order_id);
        if (engine_order.has_value() != reference_order.has_value()) {
            return field("get_order.has_value", engine_order.has_value(), reference_order.has_value());
        }
        if (engine_order && (engine_order->remaining_quantity != reference_order->remaining_quantity ||
                             engine_order->price != reference_order->price ||
                             engine_order->sequence != reference_order->sequence)) {
            return "get_order view differs for id " + std::to_string(touched_order_id);
        }
        return {};
    }

    OrderBook engine_;
    ReferenceBook reference_;
    Stats stats_;
};

// Produces adversarial order flow: a narrow price band so orders cross constantly, small quantities so
// partial fills are common, plus a steady trickle of invalid, duplicate, and unknown-ID requests.
class OperationGenerator {
public:
    explicit OperationGenerator(std::uint64_t seed) : rng_(seed) {}

    Operation next() {
        const auto roll = uniform(0, 99);
        if (roll < 55 || recent_ids_.empty()) {
            return make_submit();
        }
        if (roll < 85) {
            return make_cancel();
        }
        return make_replace();
    }

private:
    std::uint64_t uniform(std::uint64_t low, std::uint64_t high) {
        return std::uniform_int_distribution<std::uint64_t>(low, high)(rng_);
    }

    OrderId pick_recent_id() {
        return recent_ids_[uniform(0, recent_ids_.size() - 1)];
    }

    Price pick_price(OrderType type) {
        if (type == OrderType::Market) {
            return uniform(0, 49) == 0 ? 10000 : 0;  // Occasionally invalid: market orders must use price 0.
        }
        const auto roll = uniform(0, 99);
        if (roll == 0) {
            return 0;  // Invalid limit price.
        }
        if (roll == 1) {
            return -5;  // Negative prices are rejected.
        }
        return 9990 + static_cast<Price>(uniform(0, 20));
    }

    Quantity pick_quantity() {
        return uniform(0, 99) == 0 ? 0U : static_cast<Quantity>(uniform(1, 50));
    }

    Operation make_submit() {
        Operation op;
        op.kind = OpKind::Submit;

        const auto id_roll = uniform(0, 99);
        if (id_roll == 0) {
            op.order.order_id = 0;  // Invalid ID.
        } else if (id_roll < 4 && !recent_ids_.empty()) {
            op.order.order_id = pick_recent_id();  // Possibly still active: exercises duplicate rejection.
        } else {
            op.order.order_id = next_id_++;
        }

        op.order.side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
        op.order.type = uniform(0, 9) == 0 ? OrderType::Market : OrderType::Limit;
        op.order.quantity = pick_quantity();
        op.order.price = pick_price(op.order.type);
        remember(op.order);
        return op;
    }

    Operation make_cancel() {
        Operation op;
        op.kind = OpKind::Cancel;
        op.order.order_id = uniform(0, 9) == 0 ? next_id_ + 1000 : pick_recent_id();
        return op;
    }

    Operation make_replace() {
        Operation op;
        op.kind = OpKind::Replace;
        op.order.order_id = uniform(0, 19) == 0 ? next_id_ + 1000 : pick_recent_id();

        const auto known = sides_.find(op.order.order_id);
        const Side side = known != sides_.end() ? known->second : Side::Buy;
        const bool flip_side = uniform(0, 19) == 0;
        op.order.side = flip_side ? (side == Side::Buy ? Side::Sell : Side::Buy) : side;
        op.order.type = uniform(0, 19) == 0 ? OrderType::Market : OrderType::Limit;
        op.order.quantity = pick_quantity();
        op.order.price = pick_price(op.order.type);
        return op;
    }

    void remember(const OrderRequest& order) {
        if (order.order_id == 0U) {
            return;
        }
        sides_.emplace(order.order_id, order.side);
        if (recent_ids_.size() < kRecentWindow) {
            recent_ids_.push_back(order.order_id);
        } else {
            recent_ids_[recent_cursor_++ % kRecentWindow] = order.order_id;
        }
    }

    static constexpr std::size_t kRecentWindow = 64;

    std::mt19937_64 rng_;
    OrderId next_id_{1};
    std::vector<OrderId> recent_ids_;
    std::size_t recent_cursor_{0};
    std::unordered_map<OrderId, Side> sides_;
};

// Decodes raw fuzzer bytes into operations. Small ID and price ranges keep the book dense, so random input
// quickly reaches deep matching, cancel, and replace paths instead of mostly-empty books.
class ByteDecoder {
public:
    ByteDecoder(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool done() const noexcept {
        return position_ >= size_;
    }

    Operation next() {
        Operation op;
        const auto selector = byte();
        op.kind = selector < 128 ? OpKind::Submit : (selector < 208 ? OpKind::Cancel : OpKind::Replace);
        op.order.order_id = byte() % 32;  // ID 0 is invalid and exercises validation.

        const auto flags = byte();
        op.order.side = (flags & 1U) != 0U ? Side::Sell : Side::Buy;
        op.order.type = (flags & 0x0EU) == 0U ? OrderType::Market : OrderType::Limit;
        op.order.quantity = static_cast<Quantity>(byte() % 64);

        const auto price_byte = byte();
        if (op.order.type == OrderType::Market) {
            op.order.price = (flags & 0x10U) != 0U && price_byte < 8 ? 10000 : 0;
        } else {
            op.order.price = price_byte < 4 ? -static_cast<Price>(price_byte) : 9984 + static_cast<Price>(price_byte % 32);
        }
        return op;
    }

private:
    std::uint8_t byte() noexcept {
        return position_ < size_ ? data_[position_++] : 0U;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_{0};
};

}  // namespace tradematch::testing

#endif
