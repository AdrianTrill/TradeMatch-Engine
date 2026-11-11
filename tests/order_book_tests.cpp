#include "tradematch/order_book.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected, const std::string& message) {
    if (!(actual == expected)) {
        std::ostringstream stream;
        stream << message << " (expected " << expected << ", got " << actual << ')';
        throw TestFailure(stream.str());
    }
}

template <typename T>
T expect_value(std::optional<T> value, const std::string& message) {
    if (!value.has_value()) {
        throw TestFailure(message);
    }
    return *std::move(value);
}

void expect_book_invariants(const tradematch::OrderBook& book) {
    const auto snapshot = book.snapshot();
    std::unordered_set<tradematch::OrderId> seen_order_ids;
    std::size_t visible_order_count = 0;

    const auto check_side = [&](const auto& levels, tradematch::Side side, bool descending_prices) {
        bool has_previous_price = false;
        tradematch::Price previous_price = 0;

        for (const auto& level : levels) {
            expect(level.order_count > 0U, "Visible levels should contain at least one order.");
            expect(level.total_quantity > 0U, "Visible levels should contain positive quantity.");
            expect_equal(level.order_count, level.order_ids.size(), "Level order count should match visible IDs.");

            if (has_previous_price) {
                expect(descending_prices ? previous_price > level.price : previous_price < level.price,
                       "Price levels should be strictly ordered.");
            }

            const auto resting_orders = book.get_orders_at_level(side, level.price);
            expect_equal(resting_orders.size(), level.order_count, "Level order count should match indexed orders.");

            tradematch::Volume level_quantity_sum = 0;
            tradematch::SequenceNumber previous_sequence = 0;
            bool has_previous_sequence = false;

            for (std::size_t index = 0; index < resting_orders.size(); ++index) {
                const auto& order = resting_orders[index];
                expect(order.side == side, "Orders returned by level lookup should belong to the requested side.");
                expect(order.remaining_quantity > 0U, "Resting orders should contain positive quantity.");
                expect_equal(order.price, level.price, "Order price should match the containing price level.");
                expect_equal(order.order_id, level.order_ids[index], "Level order IDs should preserve FIFO order.");
                expect(seen_order_ids.insert(order.order_id).second, "Order IDs should appear only once in the book.");
                expect(book.has_order(order.order_id), "Indexed orders should be discoverable by order ID.");

                const auto indexed_order = expect_value(book.get_order(order.order_id),
                                                        "Order lookup should return every visible resting order.");
                expect(indexed_order.side == order.side, "Order side should match the indexed view.");
                expect_equal(indexed_order.price, order.price, "Order price should match the indexed view.");
                expect_equal(indexed_order.remaining_quantity,
                             order.remaining_quantity,
                             "Order quantity should match the indexed view.");
                expect_equal(indexed_order.sequence, order.sequence, "Order sequence should match the indexed view.");
                expect(indexed_order.time_in_force == order.time_in_force,
                       "Order time in force should match the indexed view.");
                expect_equal(indexed_order.visible_quantity,
                             order.visible_quantity,
                             "Displayed quantity should match the indexed view.");

                if (has_previous_sequence) {
                    expect(previous_sequence < order.sequence, "Order sequence should increase within a price level.");
                }

                previous_sequence = order.sequence;
                has_previous_sequence = true;
                expect(order.visible_quantity <= order.remaining_quantity,
                       "Displayed quantity should never exceed the open quantity.");
                level_quantity_sum += order.visible_quantity;
            }

            expect_equal(level.total_quantity, level_quantity_sum, "Level quantity should match the sum of orders.");
            previous_price = level.price;
            has_previous_price = true;
            visible_order_count += resting_orders.size();
        }
    };

    check_side(snapshot.bids, tradematch::Side::Buy, true);
    check_side(snapshot.asks, tradematch::Side::Sell, false);

    if (!snapshot.bids.empty() && !snapshot.asks.empty()) {
        expect(snapshot.bids.front().price < snapshot.asks.front().price,
               "The visible book should never remain crossed after matching.");
    }

    expect_equal(visible_order_count, book.order_count(), "Visible orders should match the indexed order count.");
}

tradematch::SubmitResult submit_checked(tradematch::OrderBook& book, const tradematch::OrderRequest& order) {
    const auto result = book.submit(order);
    expect_book_invariants(book);
    return result;
}

tradematch::CancelResult cancel_checked(tradematch::OrderBook& book, tradematch::OrderId order_id) {
    const auto result = book.cancel(order_id);
    expect_book_invariants(book);
    return result;
}

tradematch::ReplaceResult replace_checked(tradematch::OrderBook& book, const tradematch::OrderRequest& order) {
    const auto result = book.replace(order);
    expect_book_invariants(book);
    return result;
}

void test_exact_match() {
    tradematch::OrderBook book;

    const auto resting = submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 50, 10000});
    expect(resting.rested, "Initial limit order should rest in the book.");

    const auto match = submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    expect(match.accepted, "Crossing order should be accepted.");
    expect_equal(match.trades.size(), std::size_t{1}, "Exact match should generate one trade.");
    expect_equal(match.trades.front().price, tradematch::Price{10000}, "Trade should execute at the resting price.");
    expect_equal(match.trades.front().quantity, tradematch::Quantity{50}, "Trade quantity should match both orders.");
    expect_equal(book.order_count(), std::size_t{0}, "No orders should remain after an exact match.");
}

void test_partial_fill_rests_remainder() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 80, 10100});
    const auto result = submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 120, 10100});

    expect(result.accepted, "Incoming order should be accepted.");
    expect(result.rested, "Unfilled remainder should rest for a limit order.");
    expect_equal(result.filled_quantity, tradematch::Quantity{80}, "Filled quantity should match resting liquidity.");
    expect_equal(result.remaining_quantity, tradematch::Quantity{40}, "Remaining quantity should rest in the book.");

    const auto remaining_order = expect_value(book.get_order(2), "Remaining quantity should be visible by order ID.");
    expect_equal(remaining_order.remaining_quantity, tradematch::Quantity{40}, "Remaining quantity should be indexed.");
}

void test_multiple_matches_across_price_levels() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 70, 10050});

    const auto result = submit_checked(book, {3, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10100});
    expect_equal(result.trades.size(), std::size_t{2}, "Incoming order should match multiple price levels.");
    expect_equal(result.trades[0].sell_order_id, tradematch::OrderId{1}, "Best ask should execute first.");
    expect_equal(result.trades[1].sell_order_id, tradematch::OrderId{2}, "Second-best ask should execute next.");
    expect_equal(result.trades[0].price, tradematch::Price{10000}, "First trade should use first resting level price.");
    expect_equal(result.trades[1].price, tradematch::Price{10050}, "Second trade should use next resting level price.");

    const auto remaining_order =
        expect_value(book.get_order(2), "Second resting order should have remaining quantity.");
    expect_equal(remaining_order.remaining_quantity,
                 tradematch::Quantity{20},
                 "Remaining quantity should be preserved.");
}

void test_price_time_priority() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});

    const auto result = submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 60, 10000});
    expect_equal(result.trades.size(), std::size_t{2}, "Sell order should match both resting bids.");
    expect_equal(result.trades[0].buy_order_id,
                 tradematch::OrderId{1},
                 "Older order should fill first at the same price.");
    expect_equal(result.trades[1].buy_order_id,
                 tradematch::OrderId{2},
                 "Newer order should fill second at the same price.");

    const auto remaining_order = expect_value(book.get_order(2), "Second order should still be resting.");
    expect_equal(remaining_order.remaining_quantity,
                 tradematch::Quantity{20},
                 "FIFO should leave the correct remainder.");
}

void test_cancel_order() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 10000});
    const auto cancel = cancel_checked(book, 1);

    expect(cancel.cancelled, "Active order should cancel successfully.");
    expect_equal(cancel.cancelled_quantity, tradematch::Quantity{25}, "Cancel should report remaining quantity.");
    expect(!book.has_order(1), "Cancelled order should be removed from the index.");

    const auto second_cancel = cancel_checked(book, 1);
    expect(!second_cancel.cancelled, "Cancelling a missing order should fail cleanly.");
}

void test_unmatched_order_rests_in_book() {
    tradematch::OrderBook book;

    const auto result = submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 35, 9950});
    expect(result.rested, "Non-crossing limit order should rest.");
    expect_equal(result.trades.size(), std::size_t{0}, "Non-crossing order should not trade.");

    const auto snapshot = book.snapshot();
    expect_equal(snapshot.bids.size(), std::size_t{1}, "One bid level should be present.");
    expect_equal(snapshot.bids.front().price, tradematch::Price{9950}, "Bid price level should match input.");
    expect_equal(snapshot.bids.front().total_quantity, tradematch::Quantity{35}, "Resting quantity should be visible.");
    expect_equal(snapshot.bids.front().order_ids.front(),
                 tradematch::OrderId{1},
                 "Order ID should be retained at the level.");
}

void test_market_order_expires_after_available_liquidity() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10050});

    const auto result = submit_checked(book, {3, tradematch::Side::Buy, tradematch::OrderType::Market, 80, 0});
    expect(result.accepted, "Market order should be accepted.");
    expect(result.expired, "Unfilled market remainder should expire instead of resting.");
    expect_equal(result.filled_quantity,
                 tradematch::Quantity{60},
                 "Market order should consume all available liquidity.");
    expect_equal(result.remaining_quantity, tradematch::Quantity{20}, "Remainder should be reported after expiry.");
    expect_equal(book.order_count(), std::size_t{0}, "Market order should leave no resting asks in this scenario.");
}

void test_invalid_and_duplicate_orders_are_rejected() {
    tradematch::OrderBook book;

    const auto invalid = submit_checked(book, {0, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 10000});
    expect(!invalid.accepted, "Invalid order ID should be rejected.");

    const auto first = submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 10000});
    expect(first.accepted, "Valid order should be accepted.");

    const auto duplicate = submit_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 10, 10000});
    expect(!duplicate.accepted, "Duplicate active order ID should be rejected.");

    const auto invalid_market =
        submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Market, 10, 10000});
    expect(!invalid_market.accepted, "Market order price should be rejected when non-zero.");
}

void test_replace_order_resets_time_priority() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});

    // Increasing the quantity is a new order for priority purposes.
    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 60, 10000});
    expect(replace.replaced, "Replace should succeed for an active order.");
    expect(!replace.priority_kept, "A larger quantity should lose time priority.");
    expect_equal(replace.previous_remaining_quantity,
                 tradematch::Quantity{40},
                 "Replace should report the previous resting quantity.");
    expect(replace.submit_result.rested, "The replacement order should rest in the book.");

    const auto result = submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    expect_equal(result.trades.size(), std::size_t{2}, "Sell order should match both bids after replacement.");
    expect_equal(result.trades[0].buy_order_id,
                 tradematch::OrderId{2},
                 "Unreplaced order should keep earlier priority.");
    expect_equal(result.trades[1].buy_order_id,
                 tradematch::OrderId{1},
                 "Replaced order should lose time priority and fill second.");

    const auto remaining_order =
        expect_value(book.get_order(1), "Replaced order should still be resting with remaining quantity.");
    expect_equal(remaining_order.remaining_quantity,
                 tradematch::Quantity{50},
                 "Replace scenario should leave the expected remainder.");
}

void test_amend_down_keeps_time_priority() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10000});
    const auto original_sequence = expect_value(book.get_order(1), "Order 1 should rest.").sequence;

    const auto amend = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 30, 10000});
    expect(amend.replaced && amend.priority_kept, "Reducing quantity at the same price should amend in place.");
    expect_equal(amend.previous_remaining_quantity, tradematch::Quantity{100}, "The old quantity should be reported.");
    expect(amend.submit_result.trades.empty(), "An in-place amend never trades.");

    const auto amended = expect_value(book.get_order(1), "The amended order should still rest.");
    expect_equal(amended.remaining_quantity, tradematch::Quantity{30}, "The quantity should be reduced.");
    expect_equal(amended.sequence, original_sequence, "The sequence number should be unchanged.");
    expect_equal(book.snapshot().bids.front().total_quantity,
                 tradematch::Volume{130},
                 "The level total should reflect the reduction.");

    const auto fill = submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 40, 10000});
    expect_equal(fill.trades.front().buy_order_id,
                 tradematch::OrderId{1},
                 "The amended order should still fill first.");
    expect_equal(fill.trades.front().quantity, tradematch::Quantity{30}, "It should fill its reduced quantity.");

    const auto same = replace_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 90, 10000});
    expect(same.priority_kept, "Re-stating the same quantity at the same price keeps priority.");

    const auto moved = replace_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 9990});
    expect(moved.replaced && !moved.priority_kept, "A price change should lose priority even when shrinking.");
}

void test_immediate_or_cancel_never_rests() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Sell, OrderType::Limit, 30, 10000});
    submit_checked(book, {2, Side::Sell, OrderType::Limit, 30, 10100});

    const auto partial =
        submit_checked(book, {3, Side::Buy, OrderType::Limit, 50, 10000, TimeInForce::ImmediateOrCancel});
    expect(partial.accepted && partial.expired && !partial.rested, "IOC remainders should be cancelled.");
    expect_equal(partial.filled_quantity, tradematch::Quantity{30}, "IOC should trade what its limit allows.");
    expect_equal(partial.remaining_quantity, tradematch::Quantity{20}, "The cancelled remainder should be reported.");
    expect(!book.has_order(3), "IOC orders never rest.");

    const auto none = submit_checked(book, {4, Side::Buy, OrderType::Limit, 10, 10050, TimeInForce::ImmediateOrCancel});
    expect(none.expired && none.trades.empty(), "An IOC with nothing to trade should simply expire.");
    expect_equal(book.order_count(), std::size_t{1}, "Only the untouched ask should remain.");
}

void test_fill_or_kill_is_all_or_nothing() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Sell, OrderType::Limit, 30, 10000});
    submit_checked(book, {2, Side::Sell, OrderType::Limit, 30, 10050});
    submit_checked(book, {3, Side::Sell, OrderType::Limit, 30, 10100});

    const auto killed = submit_checked(book, {4, Side::Buy, OrderType::Limit, 70, 10050, TimeInForce::FillOrKill});
    expect(killed.accepted && killed.expired, "A FOK that cannot fill in full should be killed.");
    expect(killed.trades.empty() && killed.filled_quantity == 0U, "A killed FOK must not trade at all.");
    expect_equal(book.order_count(), std::size_t{3}, "A killed FOK must leave the book untouched.");

    const auto filled = submit_checked(book, {5, Side::Buy, OrderType::Limit, 60, 10050, TimeInForce::FillOrKill});
    expect(!filled.expired && filled.filled_quantity == 60U, "A FOK covered by its limit should fill completely.");
    expect_equal(filled.trades.size(), std::size_t{2}, "It may sweep several levels.");

    const auto market_kill = submit_checked(book, {6, Side::Buy, OrderType::Market, 31, 0, TimeInForce::FillOrKill});
    expect(market_kill.expired && market_kill.trades.empty(), "Market FOK orders are all-or-nothing too.");
    const auto market_fill = submit_checked(book, {7, Side::Buy, OrderType::Market, 30, 0, TimeInForce::FillOrKill});
    expect_equal(market_fill.filled_quantity, tradematch::Quantity{30}, "A coverable market FOK fills.");
    expect_equal(book.order_count(), std::size_t{0}, "All liquidity should be consumed.");
}

void test_post_only_never_takes_liquidity() {
    using tradematch::OrderType;
    using tradematch::RejectReason;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Sell, OrderType::Limit, 10, 10100});

    const auto rests =
        submit_checked(book, {2, Side::Buy, OrderType::Limit, 10, 10000, TimeInForce::GoodTillCancel, true});
    expect(rests.rested, "A non-crossing post-only order should rest.");

    const auto crossing =
        submit_checked(book, {3, Side::Buy, OrderType::Limit, 10, 10100, TimeInForce::GoodTillCancel, true});
    expect(!crossing.accepted && crossing.reject_reason == RejectReason::PostOnlyWouldCross,
           "A crossing post-only order should be rejected.");
    expect(crossing.trades.empty(), "A rejected post-only order must not trade.");

    const auto with_ioc =
        submit_checked(book, {4, Side::Buy, OrderType::Limit, 10, 9900, TimeInForce::ImmediateOrCancel, true});
    expect(with_ioc.reject_reason == RejectReason::InvalidPostOnly, "Post-only IOC orders are contradictory.");
    const auto as_market =
        submit_checked(book, {5, Side::Buy, OrderType::Market, 10, 0, TimeInForce::GoodTillCancel, true});
    expect(as_market.reject_reason == RejectReason::InvalidPostOnly, "Post-only market orders are contradictory.");

    const auto replace_cross =
        replace_checked(book, {2, Side::Buy, OrderType::Limit, 10, 10100, TimeInForce::GoodTillCancel, true});
    expect(replace_cross.reject_reason == RejectReason::PostOnlyWouldCross,
           "A post-only replacement that would cross is rejected.");
    expect_equal(expect_value(book.get_order(2), "The original must survive.").price,
                 tradematch::Price{10000},
                 "A rejected replacement leaves the original order untouched.");
}

void test_replace_requires_a_resting_time_in_force() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Buy, OrderType::Limit, 10, 10000});

    const auto ioc = replace_checked(book, {1, Side::Buy, OrderType::Limit, 10, 10010, TimeInForce::ImmediateOrCancel});
    expect(ioc.reject_reason == tradematch::RejectReason::InvalidTimeInForce, "Replacements must be able to rest.");

    const auto to_day = replace_checked(book, {1, Side::Buy, OrderType::Limit, 5, 10000, TimeInForce::Day});
    expect(to_day.priority_kept, "Switching to DAY while shrinking is still an in-place amend.");
    expect(expect_value(book.get_order(1), "Order 1 should rest.").time_in_force == TimeInForce::Day,
           "The amended time in force should be visible.");
}

void test_day_orders_expire_at_end_of_day() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, {5, Side::Buy, OrderType::Limit, 10, 10000, TimeInForce::Day});
    submit_checked(book, {2, Side::Buy, OrderType::Limit, 10, 10000});
    submit_checked(book, {9, Side::Sell, OrderType::Limit, 10, 10100, TimeInForce::Day});
    submit_checked(book, {4, Side::Sell, OrderType::Limit, 10, 10200});
    submit_checked(book, {7, Side::Buy, OrderType::Limit, 10, 9900, TimeInForce::Day});

    const auto expired = book.expire_day_orders();
    expect_book_invariants(book);
    expect(expired == std::vector<tradematch::OrderId>{5, 7, 9}, "DAY orders should expire in ascending ID order.");
    expect_equal(book.order_count(), std::size_t{2}, "GTC orders survive the end of day.");
    expect_equal(book.level_count(Side::Buy), std::size_t{1}, "Emptied levels should be removed.");
    expect(book.expire_day_orders().empty(), "A second expiry finds nothing.");
}

void test_replace_rejects_invalid_request_without_removing_resting_order() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 10000});
    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 0});

    expect(!replace.replaced, "Invalid replacement should fail.");
    expect(book.has_order(1), "Rejected replacement should leave the original resting order untouched.");

    const auto order =
        expect_value(book.get_order(1), "Original order should still be visible after rejected replacement.");
    expect_equal(order.price, tradematch::Price{10000}, "Rejected replacement should preserve the old price.");
    expect_equal(order.remaining_quantity,
                 tradematch::Quantity{25},
                 "Rejected replacement should preserve the old quantity.");
}

void test_replace_rejects_side_change() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 9900});

    const auto replace = replace_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 9900});
    expect(!replace.replaced, "Replace must not flip a resting buy into a sell.");
    expect_equal(replace.submit_result.trades.size(), std::size_t{0}, "Rejected replace must not trade.");

    const auto order = expect_value(book.get_order(1), "Original order should survive a rejected side change.");
    expect(order.side == tradematch::Side::Buy, "Original order should keep its side.");
    expect_equal(order.remaining_quantity, tradematch::Quantity{100}, "Original order should keep its quantity.");
    expect(book.has_order(2), "Other resting orders must be untouched.");
}

void test_replace_rejects_order_type_change() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 100, 10100});

    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Market, 50, 0});
    expect(!replace.replaced, "Replace must not turn a resting limit order into a market order.");
    expect_equal(replace.submit_result.trades.size(), std::size_t{0}, "Rejected replace must not trade.");

    const auto order = expect_value(book.get_order(1), "Original order should survive a rejected type change.");
    expect_equal(order.price, tradematch::Price{10000}, "Original order should keep its price.");
    expect_equal(book.order_count(), std::size_t{2}, "Book should be unchanged after a rejected replace.");
}

void test_level_quantity_does_not_overflow() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 3000000000U, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 3000000000U, 10000});

    const auto snapshot = book.snapshot();
    expect_equal(snapshot.bids.size(), std::size_t{1}, "Both orders should share one price level.");
    expect_equal(snapshot.bids.front().total_quantity,
                 tradematch::Volume{6000000000ULL},
                 "Level totals must not wrap around at 32 bits.");
}

void test_replace_that_crosses_the_book_fills_immediately() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 30, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10100});

    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 30, 10100});
    expect(replace.replaced, "A crossing replace should be accepted.");
    expect(!replace.submit_result.rested, "A fully filled replacement should not rest.");
    expect_equal(replace.submit_result.trades.size(), std::size_t{1}, "The replacement should trade once.");
    expect_equal(replace.submit_result.trades.front().price,
                 tradematch::Price{10100},
                 "The replacement should trade at the resting ask price.");
    expect(!book.has_order(1), "The fully filled replacement should leave the book.");
    expect_equal(expect_value(book.get_order(2), "The ask should keep its remainder.").remaining_quantity,
                 tradematch::Quantity{20},
                 "The ask should keep the unfilled remainder.");
}

void test_level_lookup_for_missing_price_is_empty() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 10, 10100});

    expect(book.get_orders_at_level(tradematch::Side::Buy, 9999).empty(), "Missing bid levels should be empty.");
    expect(book.get_orders_at_level(tradematch::Side::Sell, 10101).empty(), "Missing ask levels should be empty.");
    expect(!book.get_order(42).has_value(), "Unknown order IDs should not resolve.");
}

void test_top_of_book_tracks_best_prices() {
    tradematch::OrderBook book;
    expect(!book.best_bid().has_value() && !book.best_ask().has_value(), "An empty book has no top of book.");

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 9900});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 9950});
    submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 10, 10100});
    submit_checked(book, {4, tradematch::Side::Sell, tradematch::OrderType::Limit, 10, 10050});
    expect_equal(book.best_bid().value_or(0), tradematch::Price{9950}, "Best bid is the highest bid.");
    expect_equal(book.best_ask().value_or(0), tradematch::Price{10050}, "Best ask is the lowest ask.");

    submit_checked(book, {5, tradematch::Side::Buy, tradematch::OrderType::Market, 10, 0});
    expect_equal(book.best_ask().value_or(0), tradematch::Price{10100}, "Consuming the best ask exposes the next.");

    cancel_checked(book, 2);
    expect_equal(book.best_bid().value_or(0), tradematch::Price{9900}, "Cancelling the best bid exposes the next.");
}

void test_reject_reasons_are_reported() {
    using tradematch::OrderType;
    using tradematch::RejectReason;
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Buy, OrderType::Limit, 10, 10000});

    const auto reason_of = [&](const tradematch::OrderRequest& order) {
        return book.submit(order).reject_reason;
    };
    expect(reason_of({0, Side::Buy, OrderType::Limit, 10, 10000}) == RejectReason::InvalidOrderId, "ID 0.");
    expect(reason_of({2, Side::Buy, OrderType::Limit, 0, 10000}) == RejectReason::InvalidQuantity, "Zero quantity.");
    expect(reason_of({2, Side::Buy, OrderType::Limit, 10, -1}) == RejectReason::InvalidPrice, "Negative price.");
    expect(reason_of({2, Side::Buy, OrderType::Limit, 10, 0}) == RejectReason::InvalidPrice, "Zero limit price.");
    expect(reason_of({2, Side::Buy, OrderType::Market, 10, 5}) == RejectReason::InvalidPrice, "Priced market order.");
    expect(reason_of({1, Side::Sell, OrderType::Limit, 10, 10000}) == RejectReason::DuplicateOrderId, "Duplicate.");

    expect(book.cancel(99).reject_reason == RejectReason::UnknownOrder, "Unknown cancel.");
    expect(book.replace({99, Side::Buy, OrderType::Limit, 10, 10000}).reject_reason == RejectReason::UnknownOrder,
           "Unknown replace.");
    expect(book.replace({1, Side::Sell, OrderType::Limit, 10, 10000}).reject_reason == RejectReason::SideChange,
           "Side change.");
    expect(book.replace({1, Side::Buy, OrderType::Market, 10, 0}).reject_reason == RejectReason::TypeChange,
           "Type change.");
    expect(book.replace({1, Side::Buy, OrderType::Limit, 0, 10000}).reject_reason == RejectReason::InvalidQuantity,
           "Invalid replacement quantity.");

    const auto accepted = submit_checked(book, {2, Side::Sell, OrderType::Limit, 5, 10100});
    expect(accepted.reject_reason == RejectReason::None, "Accepted orders carry no reject reason.");
    expect(std::string{tradematch::to_string(RejectReason::DuplicateOrderId)} == "DUPLICATE_ORDER_ID",
           "Reject reasons have stable names.");
    expect_equal(book.order_count(), std::size_t{2}, "Rejected requests must leave the book unchanged.");
}

void test_tiny_capacity_grows_transparently() {
    tradematch::OrderBook book(tradematch::BookCapacity{1, 1, 1});

    tradematch::OrderId next_id = 1;
    for (tradematch::Price price = 10000; price < 10100; ++price) {
        for (int order = 0; order < 10; ++order) {
            submit_checked(book, {next_id++, tradematch::Side::Sell, tradematch::OrderType::Limit, 5, price});
        }
    }
    expect_equal(book.order_count(), std::size_t{1000}, "All orders should rest after the pools grow.");
    expect_equal(book.level_count(tradematch::Side::Sell), std::size_t{100}, "Each price should get its own level.");

    const auto sweep = submit_checked(book, {next_id, tradematch::Side::Buy, tradematch::OrderType::Market, 5000, 0});
    expect_equal(sweep.trades.size(), std::size_t{1000}, "The sweep should trade with every resting order.");
    expect_equal(sweep.trades.back().price, tradematch::Price{10099}, "The last fill should be the worst level.");
    expect_equal(book.order_count(), std::size_t{0}, "The sweep should empty the book.");
}

tradematch::OrderRequest stop_order(tradematch::OrderId id,
                                    tradematch::Side side,
                                    tradematch::Quantity quantity,
                                    tradematch::Price stop_price,
                                    tradematch::Price limit_price = 0) {
    tradematch::OrderRequest order{id,
                                   side,
                                   limit_price == 0 ? tradematch::OrderType::Stop : tradematch::OrderType::StopLimit,
                                   quantity,
                                   limit_price};
    order.stop_price = stop_price;
    return order;
}

tradematch::OrderRequest iceberg(tradematch::OrderId id,
                                 tradematch::Side side,
                                 tradematch::Quantity quantity,
                                 tradematch::Price price,
                                 tradematch::Quantity display) {
    tradematch::OrderRequest order{id, side, tradematch::OrderType::Limit, quantity, price};
    order.display_quantity = display;
    return order;
}

tradematch::OrderRequest owned(tradematch::OrderRequest order,
                               tradematch::OwnerId owner,
                               tradematch::SelfTradePrevention mode = tradematch::SelfTradePrevention::None) {
    order.owner = owner;
    order.self_trade_prevention = mode;
    return order;
}

void test_stop_order_waits_then_triggers_on_last_trade() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Sell, OrderType::Limit, 10, 10000});
    submit_checked(book, {2, Side::Sell, OrderType::Limit, 10, 10100});

    const auto pending = submit_checked(book, stop_order(3, Side::Buy, 10, 10000));
    expect(pending.accepted && pending.pending_trigger, "Without a trade at 100.00 the stop should wait.");
    expect(book.has_stop_order(3) && !book.has_order(3), "Pending stops are not visible in the book.");
    expect_equal(book.stop_order_count(), std::size_t{1}, "One stop should be pending.");

    const auto trigger = submit_checked(book, {4, Side::Buy, OrderType::Limit, 10, 10000});
    expect(trigger.triggered_stops.size() == 1U && trigger.triggered_stops.front() == 3U,
           "The trade at 100.00 should trigger the stop.");
    expect_equal(trigger.trades.size(), std::size_t{2}, "The request reports its own trade and the stop's trade.");
    expect_equal(trigger.trades.back().buy_order_id, tradematch::OrderId{3}, "The stop should buy as a market order.");
    expect_equal(trigger.trades.back().price, tradematch::Price{10100}, "The stop should lift the next ask.");
    expect(!book.has_stop_order(3) && book.order_count() == 0U, "The stop and both asks should be gone.");
    expect_equal(book.last_trade_price().value_or(0), tradematch::Price{10100}, "Last trade price should update.");
}

void test_stop_limit_rests_after_triggering() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Buy, OrderType::Limit, 10, 9900});
    submit_checked(book, stop_order(2, Side::Sell, 20, 9900, 9950));

    const auto trigger = submit_checked(book, {3, Side::Sell, OrderType::Limit, 10, 9900});
    expect(trigger.triggered_stops.size() == 1U, "The trade at 99.00 should trigger the sell stop-limit.");
    const auto rested = expect_value(book.get_order(2), "The activated stop-limit should rest at its limit.");
    expect_equal(rested.price, tradematch::Price{9950}, "It should rest at its limit price, not the stop price.");
    expect_equal(book.best_ask().value_or(0), tradematch::Price{9950}, "It becomes the best ask.");
}

void test_stop_already_through_the_market_triggers_immediately() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, {1, Side::Sell, OrderType::Limit, 10, 10000});
    submit_checked(book, {2, Side::Buy, OrderType::Limit, 5, 10000});
    expect_equal(book.last_trade_price().value_or(0), tradematch::Price{10000}, "A trade sets the last price.");

    const auto immediate = submit_checked(book, stop_order(3, Side::Buy, 5, 9900));
    expect(!immediate.pending_trigger && immediate.triggered_stops.size() == 1U,
           "A buy stop below the last trade should trigger on entry.");
    expect_equal(immediate.filled_quantity, tradematch::Quantity{5}, "It executes like a market order.");
}

void test_stop_cascade_triggers_in_price_then_time_order() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    for (tradematch::OrderId id = 1; id <= 4; ++id) {
        submit_checked(book, {id, Side::Sell, OrderType::Limit, 10, 10000 + static_cast<tradematch::Price>(id) * 100});
    }
    submit_checked(book, stop_order(11, Side::Buy, 10, 10200));  // arrives first, higher trigger
    submit_checked(book, stop_order(12, Side::Buy, 10, 10100));
    submit_checked(book, stop_order(13, Side::Buy, 10, 10100));  // same trigger as 12, later arrival

    const auto cascade = submit_checked(book, {20, Side::Buy, OrderType::Market, 10, 0});
    const std::vector<tradematch::OrderId> expected{12, 13, 11};
    expect(std::ranges::equal(cascade.triggered_stops, expected),
           "Stops should trigger by trigger price, then arrival, each one moving the price further.");
    expect_equal(cascade.trades.size(), std::size_t{4}, "Every ask should be lifted by the cascade.");
    expect_equal(book.stop_order_count(), std::size_t{0}, "No stop should remain.");
}

void test_first_trade_triggers_both_sides_by_arrival() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    // Before any trade, stops on both sides wait. The first trade at 100.00 triggers both at once, and the earlier
    // arrival (the buy stop) goes first. The sell stop-limit then rests at 105.00 without trading, so it is still
    // triggered after the buy stop moves nothing.
    submit_checked(book, stop_order(1, Side::Buy, 5, 9900));
    submit_checked(book, stop_order(2, Side::Sell, 5, 10100, 10500));
    submit_checked(book, {3, Side::Sell, OrderType::Limit, 20, 10000});

    const auto first = submit_checked(book, {4, Side::Buy, OrderType::Limit, 5, 10000});
    const std::vector<tradematch::OrderId> expected{1, 2};
    expect(std::ranges::equal(first.triggered_stops, expected),
           "When both sides trigger together, the earlier arrival goes first.");
    expect_equal(expect_value(book.get_order(2), "The sell stop-limit should rest.").price,
                 tradematch::Price{10500},
                 "It rests at its limit price.");
}

void test_stop_orders_can_be_cancelled_and_expire() {
    using tradematch::OrderType;
    using tradematch::RejectReason;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, stop_order(1, Side::Buy, 10, 10500));
    auto day_stop = stop_order(2, Side::Sell, 10, 9500);
    day_stop.time_in_force = TimeInForce::Day;
    submit_checked(book, day_stop);

    expect(submit_checked(book, {1, Side::Sell, OrderType::Limit, 10, 10000}).reject_reason ==
               RejectReason::DuplicateOrderId,
           "A pending stop's ID is in use.");
    expect(
        replace_checked(book, {1, Side::Buy, OrderType::Limit, 10, 10000}).reject_reason == RejectReason::UnknownOrder,
        "Replace applies to resting orders, not pending stops.");

    const auto cancel = cancel_checked(book, 1);
    expect(cancel.cancelled && cancel.cancelled_quantity == 10U, "Pending stops can be cancelled.");
    expect(book.expire_day_orders() == std::vector<tradematch::OrderId>{2}, "DAY stops expire at end of day.");
    expect_equal(book.stop_order_count(), std::size_t{0}, "No stop should remain.");
}

void test_stop_validation() {
    using tradematch::OrderType;
    using tradematch::RejectReason;
    using tradematch::Side;
    tradematch::OrderBook book;
    auto priced_stop = stop_order(1, Side::Buy, 10, 10100);
    priced_stop.price = 10200;
    expect(book.submit(priced_stop).reject_reason == RejectReason::InvalidPrice, "Stop orders become market orders.");
    expect(book.submit(stop_order(1, Side::Buy, 10, 0)).reject_reason == RejectReason::InvalidStopPrice,
           "Stops need a stop price.");
    tradematch::OrderRequest limit{1, Side::Buy, OrderType::Limit, 10, 10000};
    limit.stop_price = 10100;
    expect(book.submit(limit).reject_reason == RejectReason::InvalidStopPrice, "Plain orders cannot have one.");
    expect(book.submit(stop_order(1, Side::Buy, 10, 10100, 10150)).accepted, "A valid stop-limit is accepted.");
}

void test_iceberg_displays_one_tranche() {
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, iceberg(1, Side::Sell, 100, 10000, 20));

    const auto order = expect_value(book.get_order(1), "The iceberg should rest.");
    expect_equal(order.remaining_quantity, tradematch::Quantity{100}, "The full quantity remains open.");
    expect_equal(order.visible_quantity, tradematch::Quantity{20}, "Only one tranche is displayed.");
    expect_equal(book.snapshot().asks.front().total_quantity, tradematch::Volume{20}, "Depth shows the tranche only.");

    const auto cancel = cancel_checked(book, 1);
    expect_equal(cancel.cancelled_quantity, tradematch::Quantity{100}, "Cancel removes hidden quantity too.");
}

void test_iceberg_refresh_goes_to_the_back_of_the_queue() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::OrderBook book;
    submit_checked(book, iceberg(1, Side::Sell, 30, 10000, 10));
    submit_checked(book, {2, Side::Sell, OrderType::Limit, 10, 10000});

    const auto sweep = submit_checked(book, {3, Side::Buy, OrderType::Limit, 25, 10000});
    expect_equal(sweep.trades.size(), std::size_t{3}, "The sweep should take three fills.");
    expect_equal(sweep.trades[0].sell_order_id, tradematch::OrderId{1}, "The first tranche fills first.");
    expect_equal(sweep.trades[1].sell_order_id, tradematch::OrderId{2}, "The refresh queues behind order 2.");
    expect_equal(sweep.trades[2].sell_order_id, tradematch::OrderId{1}, "Then the refreshed tranche fills.");
    expect_equal(sweep.trades[2].quantity, tradematch::Quantity{5}, "Only what is needed is taken.");

    const auto rest = expect_value(book.get_order(1), "The iceberg keeps its last tranche and reserve.");
    expect_equal(rest.remaining_quantity, tradematch::Quantity{15}, "15 of 30 should remain open.");
    expect_equal(rest.visible_quantity, tradematch::Quantity{5}, "5 of the second tranche is still displayed.");
}

void test_iceberg_reserve_counts_for_fill_or_kill() {
    using tradematch::OrderType;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, iceberg(1, Side::Sell, 100, 10000, 10));

    const auto filled = submit_checked(book, {2, Side::Buy, OrderType::Limit, 60, 10000, TimeInForce::FillOrKill});
    expect(!filled.expired && filled.filled_quantity == 60U, "Hidden quantity is executable for a FOK.");
    expect_equal(filled.trades.size(), std::size_t{6}, "It fills tranche by tranche.");
    const auto killed = submit_checked(book, {3, Side::Buy, OrderType::Limit, 41, 10000, TimeInForce::FillOrKill});
    expect(killed.expired && killed.trades.empty(), "Asking for more than the reserve still kills the FOK.");
}

void test_iceberg_validation_and_amend() {
    using tradematch::OrderType;
    using tradematch::RejectReason;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    expect(book.submit(iceberg(1, Side::Buy, 10, 10000, 20)).reject_reason == RejectReason::InvalidDisplayQuantity,
           "The display cannot exceed the order.");
    auto market_iceberg = iceberg(1, Side::Buy, 10, 0, 5);
    market_iceberg.type = OrderType::Market;
    expect(book.submit(market_iceberg).reject_reason == RejectReason::InvalidDisplayQuantity,
           "Market orders cannot be icebergs.");
    auto ioc_iceberg = iceberg(1, Side::Buy, 10, 10000, 5);
    ioc_iceberg.time_in_force = TimeInForce::ImmediateOrCancel;
    expect(book.submit(ioc_iceberg).reject_reason == RejectReason::InvalidDisplayQuantity,
           "Icebergs must be able to rest.");

    submit_checked(book, iceberg(1, Side::Buy, 100, 10000, 20));
    submit_checked(book, {2, Side::Buy, OrderType::Limit, 10, 10000});
    const auto amend = replace_checked(book, iceberg(1, Side::Buy, 30, 10000, 20));
    expect(amend.priority_kept, "Shrinking an iceberg at the same price keeps its priority.");
    const auto amended = expect_value(book.get_order(1), "The iceberg should still rest.");
    expect_equal(amended.visible_quantity, tradematch::Quantity{20}, "The reduction comes out of the reserve.");
    expect_equal(amended.remaining_quantity, tradematch::Quantity{30}, "The new open quantity applies.");

    const auto redisplay = replace_checked(book, iceberg(1, Side::Buy, 30, 10000, 10));
    expect(redisplay.replaced && !redisplay.priority_kept, "Changing the display size is a new order.");
}

void test_self_trade_prevention_modes() {
    using tradematch::OrderType;
    using tradematch::SelfTradePrevention;
    using tradematch::Side;

    const auto setup = [](tradematch::OrderBook& book) {
        submit_checked(book, owned({1, Side::Sell, OrderType::Limit, 10, 10000}, 7));
        submit_checked(book, owned({2, Side::Sell, OrderType::Limit, 10, 10000}, 8));
    };

    {
        tradematch::OrderBook book;
        setup(book);
        const auto allowed = submit_checked(book, owned({3, Side::Buy, OrderType::Limit, 10, 10000}, 7));
        expect(allowed.trades.size() == 1U && allowed.trades.front().sell_order_id == 1U,
               "Without prevention, a self-trade is allowed.");
    }
    {
        tradematch::OrderBook book;
        setup(book);
        const auto newest =
            submit_checked(book,
                           owned({3, Side::Buy, OrderType::Limit, 15, 10000}, 7, SelfTradePrevention::CancelNewest));
        expect(newest.self_trade_prevented && newest.expired && newest.trades.empty(),
               "Cancel-newest kills the incoming order at the first self-match.");
        expect(book.has_order(1) && book.has_order(2), "Resting orders are untouched.");
    }
    {
        tradematch::OrderBook book;
        setup(book);
        const auto oldest =
            submit_checked(book,
                           owned({3, Side::Buy, OrderType::Limit, 15, 10000}, 7, SelfTradePrevention::CancelOldest));
        expect(oldest.self_trade_cancels.size() == 1U && oldest.self_trade_cancels.front() == 1U,
               "Cancel-oldest removes the resting self order.");
        expect(oldest.trades.size() == 1U && oldest.trades.front().sell_order_id == 2U, "Then keeps matching.");
        expect(oldest.rested && !oldest.self_trade_prevented, "The remainder rests normally.");
    }
    {
        tradematch::OrderBook book;
        setup(book);
        const auto both =
            submit_checked(book,
                           owned({3, Side::Buy, OrderType::Limit, 15, 10000}, 7, SelfTradePrevention::CancelBoth));
        expect(both.self_trade_cancels.size() == 1U && both.self_trade_prevented && both.trades.empty(),
               "Cancel-both removes both orders.");
        expect(!book.has_order(1) && book.has_order(2) && !book.has_order(3), "Only the other owner's order remains.");
    }
}

void test_fill_or_kill_respects_self_trade_prevention() {
    using tradematch::OrderType;
    using tradematch::SelfTradePrevention;
    using tradematch::Side;
    using tradematch::TimeInForce;
    tradematch::OrderBook book;
    submit_checked(book, owned({1, Side::Sell, OrderType::Limit, 10, 10000}, 7));
    submit_checked(book, owned({2, Side::Sell, OrderType::Limit, 10, 10000}, 8));

    auto newest = owned({3, Side::Buy, OrderType::Limit, 10, 10000, TimeInForce::FillOrKill},
                        7,
                        SelfTradePrevention::CancelNewest);
    const auto killed = submit_checked(book, newest);
    expect(killed.expired && killed.trades.empty() && book.order_count() == 2U,
           "A FOK that would be stopped by its own order is killed before touching the book.");

    auto oldest = owned({4, Side::Buy, OrderType::Limit, 10, 10000, TimeInForce::FillOrKill},
                        7,
                        SelfTradePrevention::CancelOldest);
    const auto filled = submit_checked(book, oldest);
    expect(!filled.expired && filled.filled_quantity == 10U, "Cancel-oldest FOKs fill from other owners.");
    expect(filled.self_trade_cancels.size() == 1U && !book.has_order(1), "Its own resting order is cancelled.");
}

void test_formatting_helpers() {
    expect_equal(std::string{tradematch::to_string(tradematch::Side::Buy)}, std::string{"BUY"}, "Buy side label.");
    expect_equal(std::string{tradematch::to_string(tradematch::Side::Sell)}, std::string{"SELL"}, "Sell side label.");
    expect_equal(std::string{tradematch::to_string(tradematch::OrderType::Limit)},
                 std::string{"LIMIT"},
                 "Limit order label.");
    expect_equal(std::string{tradematch::to_string(tradematch::OrderType::Market)},
                 std::string{"MARKET"},
                 "Market order label.");

    using tradematch::RejectReason;
    const std::vector<std::pair<RejectReason, std::string>> reasons = {
        {RejectReason::None, "NONE"},
        {RejectReason::InvalidOrderId, "INVALID_ORDER_ID"},
        {RejectReason::InvalidQuantity, "INVALID_QUANTITY"},
        {RejectReason::InvalidPrice, "INVALID_PRICE"},
        {RejectReason::DuplicateOrderId, "DUPLICATE_ORDER_ID"},
        {RejectReason::UnknownOrder, "UNKNOWN_ORDER"},
        {RejectReason::SideChange, "SIDE_CHANGE"},
        {RejectReason::TypeChange, "TYPE_CHANGE"},
        {RejectReason::InvalidTimeInForce, "INVALID_TIME_IN_FORCE"},
        {RejectReason::InvalidPostOnly, "INVALID_POST_ONLY"},
        {RejectReason::PostOnlyWouldCross, "POST_ONLY_WOULD_CROSS"},
        {RejectReason::InvalidStopPrice, "INVALID_STOP_PRICE"},
        {RejectReason::InvalidDisplayQuantity, "INVALID_DISPLAY_QUANTITY"},
        {RejectReason::UnknownSymbol, "UNKNOWN_SYMBOL"},
    };
    for (const auto& [mode, name] : std::vector<std::pair<tradematch::SelfTradePrevention, std::string>>{
             {tradematch::SelfTradePrevention::None, "NONE"},
             {tradematch::SelfTradePrevention::CancelNewest, "CANCEL_NEWEST"},
             {tradematch::SelfTradePrevention::CancelOldest, "CANCEL_OLDEST"},
             {tradematch::SelfTradePrevention::CancelBoth, "CANCEL_BOTH"},
         }) {
        expect_equal(std::string{tradematch::to_string(mode)}, name, "Self-trade prevention names should be stable.");
    }
    expect_equal(std::string{tradematch::to_string(tradematch::OrderType::Stop)}, std::string{"STOP"}, "Stop label.");
    expect_equal(std::string{tradematch::to_string(tradematch::OrderType::StopLimit)},
                 std::string{"STOP_LIMIT"},
                 "Stop-limit label.");
    for (const auto& [tif, name] : std::vector<std::pair<tradematch::TimeInForce, std::string>>{
             {tradematch::TimeInForce::GoodTillCancel, "GTC"},
             {tradematch::TimeInForce::Day, "DAY"},
             {tradematch::TimeInForce::ImmediateOrCancel, "IOC"},
             {tradematch::TimeInForce::FillOrKill, "FOK"},
         }) {
        expect_equal(std::string{tradematch::to_string(tif)}, name, "Time-in-force names should be stable.");
    }
    for (const auto& [reason, name] : reasons) {
        expect_equal(std::string{tradematch::to_string(reason)}, name, "Reject reason names should be stable.");
    }

    expect_equal(tradematch::format_price(10050), std::string{"100.50"}, "Ticks should format as cents.");
    expect_equal(tradematch::format_price(5), std::string{"0.05"}, "Sub-dollar prices should be zero padded.");
    expect_equal(tradematch::format_price(0), std::string{"0.00"}, "Zero should format with two decimals.");
    expect_equal(tradematch::format_price(-5), std::string{"-0.05"}, "Negative prices should keep their sign.");
    expect_equal(tradematch::format_price(std::numeric_limits<tradematch::Price>::max()),
                 std::string{"92233720368547758.07"},
                 "The largest price should format exactly.");
    expect_equal(tradematch::format_price(std::numeric_limits<tradematch::Price>::min()),
                 std::string{"-92233720368547758.08"},
                 "The most negative price should format without overflow.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"exact_match", test_exact_match},
        {"partial_fill_rests_remainder", test_partial_fill_rests_remainder},
        {"multiple_matches_across_price_levels", test_multiple_matches_across_price_levels},
        {"price_time_priority", test_price_time_priority},
        {"cancel_order", test_cancel_order},
        {"unmatched_order_rests_in_book", test_unmatched_order_rests_in_book},
        {"market_order_expires_after_available_liquidity", test_market_order_expires_after_available_liquidity},
        {"invalid_and_duplicate_orders_are_rejected", test_invalid_and_duplicate_orders_are_rejected},
        {"replace_order_resets_time_priority", test_replace_order_resets_time_priority},
        {"amend_down_keeps_time_priority", test_amend_down_keeps_time_priority},
        {"immediate_or_cancel_never_rests", test_immediate_or_cancel_never_rests},
        {"fill_or_kill_is_all_or_nothing", test_fill_or_kill_is_all_or_nothing},
        {"post_only_never_takes_liquidity", test_post_only_never_takes_liquidity},
        {"replace_requires_a_resting_time_in_force", test_replace_requires_a_resting_time_in_force},
        {"day_orders_expire_at_end_of_day", test_day_orders_expire_at_end_of_day},
        {"stop_order_waits_then_triggers_on_last_trade", test_stop_order_waits_then_triggers_on_last_trade},
        {"stop_limit_rests_after_triggering", test_stop_limit_rests_after_triggering},
        {"stop_already_through_the_market_triggers_immediately",
         test_stop_already_through_the_market_triggers_immediately},
        {"stop_cascade_triggers_in_price_then_time_order", test_stop_cascade_triggers_in_price_then_time_order},
        {"first_trade_triggers_both_sides_by_arrival", test_first_trade_triggers_both_sides_by_arrival},
        {"stop_orders_can_be_cancelled_and_expire", test_stop_orders_can_be_cancelled_and_expire},
        {"stop_validation", test_stop_validation},
        {"iceberg_displays_one_tranche", test_iceberg_displays_one_tranche},
        {"iceberg_refresh_goes_to_the_back_of_the_queue", test_iceberg_refresh_goes_to_the_back_of_the_queue},
        {"iceberg_reserve_counts_for_fill_or_kill", test_iceberg_reserve_counts_for_fill_or_kill},
        {"iceberg_validation_and_amend", test_iceberg_validation_and_amend},
        {"self_trade_prevention_modes", test_self_trade_prevention_modes},
        {"fill_or_kill_respects_self_trade_prevention", test_fill_or_kill_respects_self_trade_prevention},
        {"replace_rejects_invalid_request_without_removing_resting_order",
         test_replace_rejects_invalid_request_without_removing_resting_order},
        {"replace_rejects_side_change", test_replace_rejects_side_change},
        {"replace_rejects_order_type_change", test_replace_rejects_order_type_change},
        {"level_quantity_does_not_overflow", test_level_quantity_does_not_overflow},
        {"replace_that_crosses_the_book_fills_immediately", test_replace_that_crosses_the_book_fills_immediately},
        {"level_lookup_for_missing_price_is_empty", test_level_lookup_for_missing_price_is_empty},
        {"top_of_book_tracks_best_prices", test_top_of_book_tracks_best_prices},
        {"reject_reasons_are_reported", test_reject_reasons_are_reported},
        {"tiny_capacity_grows_transparently", test_tiny_capacity_grows_transparently},
        {"formatting_helpers", test_formatting_helpers},
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
        std::cout << "All tests passed (" << tests.size() << ").\n";
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
