#ifndef TRADEMATCH_TESTS_DIFFERENTIAL_HPP
#define TRADEMATCH_TESTS_DIFFERENTIAL_HPP

#include "support/reference_book.hpp"
#include "tradematch/detail/codec.hpp"
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

enum class OpKind : std::uint8_t {
    Submit,
    Cancel,
    Replace,
    ExpireDay,
    BeginAuction,
    Uncross,
    Snapshot  // replace the engine with a book restored from a binary snapshot of its state
};

struct Operation {
    OpKind kind{OpKind::Submit};
    OrderRequest order;  // Cancel only uses order.order_id; ExpireDay uses nothing.
};

inline Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

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
        case OpKind::ExpireDay:
            return "EXPIRE_DAY_ORDERS";
        case OpKind::BeginAuction:
            return "BEGIN_AUCTION";
        case OpKind::Uncross:
            return "UNCROSS";
        case OpKind::Snapshot:
            return "SNAPSHOT_RESTORE";
    }
    out << "id=" << op.order.order_id << ' ' << to_string(op.order.side) << ' ' << to_string(op.order.type)
        << " qty=" << op.order.quantity << " price=" << op.order.price << ' ' << to_string(op.order.time_in_force)
        << (op.order.post_only ? " POST_ONLY" : "") << " stop=" << op.order.stop_price
        << " display=" << op.order.display_quantity << " owner=" << op.order.owner
        << " stp=" << to_string(op.order.self_trade_prevention);
    return out.str();
}

// Runs every operation against both the optimized engine and the naive reference, and reports the first
// observable difference: a result field, a trade, the visible book, or an order lookup.
class DifferentialHarness {
public:
    DifferentialHarness() = default;
    explicit DifferentialHarness(const BookCapacity& capacity) : capacity_(capacity), engine_(capacity) {}

    struct Stats {
        std::size_t operations{0};
        std::size_t trades{0};
        std::size_t rejections{0};
        std::size_t cancels{0};
        std::size_t replaces{0};
        std::size_t amends_in_place{0};
        std::size_t day_expiries{0};
        std::size_t stops_triggered{0};
        std::size_t self_trade_cancels{0};
        std::size_t auctions{0};
        std::size_t auction_trades{0};
        std::size_t snapshots{0};
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
                stats_.stops_triggered += result.triggered_stops.size();
                stats_.self_trade_cancels += result.self_trade_cancels.size();
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
                stats_.amends_in_place += result.priority_kept ? 1U : 0U;
                stats_.trades += result.submit_result.trades.size();
                stats_.stops_triggered += result.submit_result.triggered_stops.size();
                stats_.self_trade_cancels += result.submit_result.self_trade_cancels.size();
                break;
            }
            case OpKind::ExpireDay: {
                const auto engine_expired = engine_.expire_day_orders();
                if (engine_expired != reference_.expire_day_orders()) {
                    mismatch = "expire_day_orders returned different order IDs";
                }
                stats_.day_expiries += engine_expired.size();
                break;
            }
            case OpKind::BeginAuction:
                engine_.begin_auction();
                reference_.begin_auction();
                break;
            case OpKind::Uncross: {
                const auto result = engine_.uncross();
                mismatch = compare_uncross(result, reference_.uncross());
                stats_.auctions += result.executed ? 1U : 0U;
                stats_.auction_trades += result.executed ? result.trades.size() : 0U;
                break;
            }
            case OpKind::Snapshot:
                mismatch = restore_from_snapshot();
                ++stats_.snapshots;
                break;
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

    const ReferenceBook& reference() const noexcept {
        return reference_;
    }

private:
    // Round-trips the engine's state through the binary snapshot encoding into a brand-new book, which then replaces
    // the engine. The reference keeps running untouched, so any state a snapshot fails to capture shows up as a later
    // mismatch.
    std::string restore_from_snapshot() {
        const BookState state = engine_.export_state();
        std::vector<std::uint8_t> bytes;
        detail::ByteWriter out(bytes);
        detail::encode_book_state(out, state);
        detail::ByteReader in(bytes);
        const BookState decoded = detail::decode_book_state(in);
        if (!(decoded == state) || in.remaining() != 0U) {
            return "the snapshot encoding did not round-trip the book state";
        }
        OrderBook restored(capacity_);
        restored.import_state(decoded);
        if (!(restored.export_state() == state)) {
            return "the restored book exports a different state";
        }
        engine_ = std::move(restored);
        return {};
    }

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
        if (engine.auction != reference.auction) {
            return prefix + "auction flag differs";
        }
        return {};
    }

    static std::string compare_reason(const char* what, RejectReason engine, RejectReason reference) {
        if (engine == reference) {
            return {};
        }
        return std::string(what) + ".reject_reason: engine=" + to_string(engine) + " reference=" + to_string(reference);
    }

    static std::string compare_submit(const SubmitResult& engine, const SubmitResult& reference, const char* what) {
        const auto prefix = std::string(what) + ".";
        if (engine.accepted != reference.accepted) {
            return field((prefix + "accepted").c_str(), engine.accepted, reference.accepted);
        }
        if (!engine.accepted) {
            return compare_reason(what, engine.reject_reason, reference.reject_reason);
        }
        if (engine.rested != reference.rested) {
            return field((prefix + "rested").c_str(), engine.rested, reference.rested);
        }
        if (engine.expired != reference.expired) {
            return field((prefix + "expired").c_str(), engine.expired, reference.expired);
        }
        if (engine.queued_for_auction != reference.queued_for_auction) {
            return field((prefix + "queued_for_auction").c_str(),
                         engine.queued_for_auction,
                         reference.queued_for_auction);
        }
        if (engine.pending_trigger != reference.pending_trigger) {
            return field((prefix + "pending_trigger").c_str(), engine.pending_trigger, reference.pending_trigger);
        }
        if (engine.self_trade_prevented != reference.self_trade_prevented) {
            return field((prefix + "self_trade_prevented").c_str(),
                         engine.self_trade_prevented,
                         reference.self_trade_prevented);
        }
        if (!std::ranges::equal(engine.triggered_stops, reference.triggered_stops)) {
            return prefix + "triggered_stops differ";
        }
        if (!std::ranges::equal(engine.self_trade_cancels, reference.self_trade_cancels)) {
            return prefix + "self_trade_cancels differ";
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

    static std::string compare_uncross(const AuctionResult& engine, const AuctionResult& reference) {
        if (engine.executed != reference.executed) {
            return field("uncross.executed", engine.executed, reference.executed);
        }
        if (engine.price != reference.price) {
            return field("uncross.price", engine.price, reference.price);
        }
        if (engine.matched_quantity != reference.matched_quantity) {
            return field("uncross.matched_quantity", engine.matched_quantity, reference.matched_quantity);
        }
        if (engine.trades.size() != reference.trades.size()) {
            return field("uncross.trade_count", engine.trades.size(), reference.trades.size());
        }
        for (std::size_t index = 0; index < engine.trades.size(); ++index) {
            auto mismatch = compare_trade(engine.trades[index], reference.trades[index], index);
            if (!mismatch.empty()) {
                return "uncross." + mismatch;
            }
        }
        if (!std::ranges::equal(engine.expired_market_orders, reference.expired_market_orders)) {
            return "uncross.expired_market_orders differ";
        }
        if (!std::ranges::equal(engine.triggered_stops, reference.triggered_stops)) {
            return "uncross.triggered_stops differ";
        }
        return {};
    }

    static std::string compare_cancel(const CancelResult& engine, const CancelResult& reference) {
        if (engine.cancelled != reference.cancelled) {
            return field("cancel.cancelled", engine.cancelled, reference.cancelled);
        }
        if (!engine.cancelled) {
            return compare_reason("cancel", engine.reject_reason, reference.reject_reason);
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
            return compare_reason("replace", engine.reject_reason, reference.reject_reason);
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

        if (engine_.best_bid() != reference_.best_bid()) {
            return field("best_bid", engine_.best_bid().value_or(-1), reference_.best_bid().value_or(-1));
        }
        if (engine_.best_ask() != reference_.best_ask()) {
            return field("best_ask", engine_.best_ask().value_or(-1), reference_.best_ask().value_or(-1));
        }
        if (engine_.phase() != reference_.phase()) {
            return "phase differs";
        }
        // Outside an auction the book can never stay crossed; in particular, every uncross must leave it uncrossed.
        if (engine_.phase() == TradingPhase::Continuous && engine_.best_bid() && engine_.best_ask() &&
            *engine_.best_bid() >= *engine_.best_ask()) {
            return "the book is crossed in continuous trading";
        }
        if (engine_.auction_market_order_count() != reference_.auction_market_order_count()) {
            return field("auction_market_order_count",
                         engine_.auction_market_order_count(),
                         reference_.auction_market_order_count());
        }
        if (engine_.indicative_auction() != reference_.indicative_auction()) {
            const auto engine_price = engine_.indicative_auction().value_or(AuctionIndicative{}).price;
            const auto reference_price = reference_.indicative_auction().value_or(AuctionIndicative{}).price;
            return field("indicative_auction.price", engine_price, reference_price) + " (or volumes differ)";
        }
        if (engine_.stop_order_count() != reference_.stop_order_count()) {
            return field("stop_order_count", engine_.stop_order_count(), reference_.stop_order_count());
        }
        if (engine_.last_trade_price() != reference_.last_trade_price()) {
            return field("last_trade_price",
                         engine_.last_trade_price().value_or(-1),
                         reference_.last_trade_price().value_or(-1));
        }

        const auto engine_order = engine_.get_order(touched_order_id);
        const auto reference_order = reference_.get_order(touched_order_id);
        if (engine_order.has_value() != reference_order.has_value()) {
            return field("get_order.has_value", engine_order.has_value(), reference_order.has_value());
        }
        if (engine_order && reference_order &&
            (engine_order->remaining_quantity != reference_order->remaining_quantity ||
             engine_order->price != reference_order->price || engine_order->sequence != reference_order->sequence ||
             engine_order->time_in_force != reference_order->time_in_force ||
             engine_order->visible_quantity != reference_order->visible_quantity ||
             engine_order->display_quantity != reference_order->display_quantity ||
             engine_order->owner != reference_order->owner)) {
            return "get_order view differs for id " + std::to_string(touched_order_id);
        }
        return {};
    }

    BookCapacity capacity_;
    OrderBook engine_;
    ReferenceBook reference_;
    Stats stats_;
};

// Produces adversarial order flow: a narrow price band so orders cross constantly, small quantities so
// partial fills are common, every time-in-force and post-only combination, replacements that sometimes amend in
// place, end-of-day expiries, and a steady trickle of invalid, duplicate, and unknown-ID requests. The reference
// book is consulted only to aim replacements at an order's current price and size.
class OperationGenerator {
public:
    explicit OperationGenerator(std::uint64_t seed) : rng_(seed) {}

    Operation next(const ReferenceBook& book) {
        // Occasional call auctions: open one about every 150 operations, then uncross after about 30.
        if (uniform(0, in_auction_ ? 29 : 149) == 0) {
            Operation op;
            op.kind = in_auction_ ? OpKind::Uncross : OpKind::BeginAuction;
            in_auction_ = !in_auction_;
            return op;
        }
        const auto roll = uniform(0, 199);
        if (roll == 0) {
            Operation op;
            op.kind = OpKind::ExpireDay;
            return op;
        }
        if (roll < 110 || recent_ids_.empty()) {
            return make_submit();
        }
        if (roll < 170) {
            return make_cancel();
        }
        return make_replace(book);
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

    // Mostly the resting kinds a replace accepts, with an occasional IOC or FOK to exercise its rejection.
    TimeInForce pick_replacement_time_in_force() {
        if (uniform(0, 9) == 0) {
            return pick_time_in_force();
        }
        return uniform(0, 3) == 0 ? TimeInForce::Day : TimeInForce::GoodTillCancel;
    }

    OrderType pick_order_type() {
        const auto roll = uniform(0, 99);
        if (roll < 10) {
            return OrderType::Market;
        }
        if (roll < 16) {
            return OrderType::Stop;
        }
        return roll < 22 ? OrderType::StopLimit : OrderType::Limit;
    }

    // Stop prices inside the trading band, iceberg display sizes, and a few owners with random prevention modes.
    // Roughly one request in fifty carries an invalid combination to exercise validation.
    void add_advanced_fields(OrderRequest& order) {
        const bool stop = order.type == OrderType::Stop || order.type == OrderType::StopLimit;
        if (stop || uniform(0, 99) == 0) {
            order.stop_price = uniform(0, 49) == 0 ? 0 : 9990 + static_cast<Price>(uniform(0, 20));
        }
        if (uniform(0, 99) < 15 && order.quantity > 0U) {
            order.display_quantity =
                static_cast<Quantity>(uniform(1, order.quantity + (uniform(0, 19) == 0 ? 5U : 0U)));
        }
        if (uniform(0, 99) < 40) {
            order.owner = static_cast<OwnerId>(uniform(1, 3));
            order.self_trade_prevention = static_cast<SelfTradePrevention>(uniform(0, 3));
        }
    }

    TimeInForce pick_time_in_force() {
        const auto roll = uniform(0, 99);
        if (roll < 60) {
            return TimeInForce::GoodTillCancel;
        }
        if (roll < 75) {
            return TimeInForce::Day;
        }
        return roll < 90 ? TimeInForce::ImmediateOrCancel : TimeInForce::FillOrKill;
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
        op.order.type = pick_order_type();
        op.order.quantity = pick_quantity();
        op.order.price = pick_price(op.order.type);
        op.order.time_in_force = pick_time_in_force();
        op.order.post_only = uniform(0, 9) == 0;
        add_advanced_fields(op.order);
        remember(op.order);
        return op;
    }

    Operation make_cancel() {
        Operation op;
        op.kind = OpKind::Cancel;
        op.order.order_id = uniform(0, 9) == 0 ? next_id_ + 1000 : pick_recent_id();
        return op;
    }

    Operation make_replace(const ReferenceBook& book) {
        Operation op;
        op.kind = OpKind::Replace;
        op.order.order_id = uniform(0, 19) == 0 ? next_id_ + 1000 : pick_recent_id();

        const auto known = sides_.find(op.order.order_id);
        const Side side = known != sides_.end() ? known->second : Side::Buy;
        const bool flip_side = uniform(0, 19) == 0;
        op.order.side = flip_side ? opposite(side) : side;
        op.order.type = uniform(0, 19) == 0 ? OrderType::Market : OrderType::Limit;
        op.order.quantity = pick_quantity();
        op.order.price = pick_price(op.order.type);
        op.order.time_in_force = pick_replacement_time_in_force();
        op.order.post_only = uniform(0, 9) == 0;
        add_advanced_fields(op.order);

        // Half of the replacements keep the current price with the same or a smaller size: the in-place amend path.
        const auto current = book.get_order(op.order.order_id);
        if (current && op.order.type == OrderType::Limit && uniform(0, 1) == 0) {
            op.order.price = current->price;
            op.order.quantity = static_cast<Quantity>(uniform(1, current->remaining_quantity));
            op.order.display_quantity = current->display_quantity;
            op.order.stop_price = 0;
        }
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
    bool in_auction_{false};
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
        op.kind = decode_kind(selector);
        op.order.order_id = byte() % 32U;  // ID 0 is invalid and exercises validation.

        const auto flags = byte();
        op.order.side = (flags & 1U) != 0U ? Side::Sell : Side::Buy;
        op.order.type = decode_type(static_cast<std::uint8_t>((flags >> 1U) & 7U));
        op.order.quantity = static_cast<Quantity>(byte() % 64);

        const auto extras = byte();
        op.order.time_in_force = static_cast<TimeInForce>(extras & 3U);
        op.order.post_only = (extras & 0x1CU) == 0U;

        const auto price_byte = byte();
        if (op.order.type == OrderType::Market || op.order.type == OrderType::Stop) {
            op.order.price = (flags & 0x10U) != 0U && price_byte < 8 ? 10000 : 0;
        } else {
            op.order.price =
                price_byte < 4 ? -static_cast<Price>(price_byte) : 9984 + static_cast<Price>(price_byte % 32);
        }

        const auto advanced = byte();
        const auto stop_byte = byte();
        const bool stop = op.order.type == OrderType::Stop || op.order.type == OrderType::StopLimit;
        if (stop || stop_byte == 255) {
            op.order.stop_price = stop_byte < 2 ? 0 : 9984 + static_cast<Price>(stop_byte % 32);
        }
        op.order.display_quantity = static_cast<Quantity>(advanced & 7U) * 3U;
        op.order.owner = static_cast<OwnerId>((advanced >> 3U) & 3U);
        op.order.self_trade_prevention = static_cast<SelfTradePrevention>((advanced >> 5U) & 3U);
        return op;
    }

private:
    static OrderType decode_type(std::uint8_t bits) noexcept {
        if (bits == 0) {
            return OrderType::Market;
        }
        if (bits == 6) {
            return OrderType::Stop;
        }
        return bits == 7 ? OrderType::StopLimit : OrderType::Limit;
    }

    static OpKind decode_kind(std::uint8_t selector) noexcept {
        if (selector < 128) {
            return OpKind::Submit;
        }
        if (selector < 208) {
            return OpKind::Cancel;
        }
        if (selector < 250) {
            return OpKind::Replace;
        }
        if (selector < 253) {
            return OpKind::ExpireDay;
        }
        if (selector == 253) {
            return OpKind::BeginAuction;
        }
        return selector == 254 ? OpKind::Uncross : OpKind::Snapshot;
    }

    std::uint8_t byte() noexcept {
        return position_ < size_ ? data_[position_++] : std::uint8_t{0};
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_{0};
};

}  // namespace tradematch::testing

#endif
