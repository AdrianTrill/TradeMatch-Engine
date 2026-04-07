#include "tradematch/order_book.hpp"

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
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

            tradematch::Quantity level_quantity_sum = 0;
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

                const auto indexed_order = book.get_order(order.order_id);
                expect(indexed_order.has_value(), "Order lookup should return every visible resting order.");
                expect(indexed_order->side == order.side, "Order side should match the indexed view.");
                expect_equal(indexed_order->price, order.price, "Order price should match the indexed view.");
                expect_equal(indexed_order->remaining_quantity,
                             order.remaining_quantity,
                             "Order quantity should match the indexed view.");
                expect_equal(indexed_order->sequence, order.sequence, "Order sequence should match the indexed view.");

                if (has_previous_sequence) {
                    expect(previous_sequence < order.sequence,
                           "Order sequence should increase within a price level.");
                }

                previous_sequence = order.sequence;
                has_previous_sequence = true;
                level_quantity_sum += order.remaining_quantity;
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

    const auto remaining_order = book.get_order(2);
    expect(remaining_order.has_value(), "Remaining quantity should be visible by order ID.");
    expect_equal(remaining_order->remaining_quantity, tradematch::Quantity{40}, "Remaining quantity should be indexed.");
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

    const auto remaining_order = book.get_order(2);
    expect(remaining_order.has_value(), "Second resting order should have remaining quantity.");
    expect_equal(remaining_order->remaining_quantity, tradematch::Quantity{20}, "Remaining quantity should be preserved.");
}

void test_price_time_priority() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});
    submit_checked(book, {2, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});

    const auto result = submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 60, 10000});
    expect_equal(result.trades.size(), std::size_t{2}, "Sell order should match both resting bids.");
    expect_equal(result.trades[0].buy_order_id, tradematch::OrderId{1}, "Older order should fill first at the same price.");
    expect_equal(result.trades[1].buy_order_id, tradematch::OrderId{2}, "Newer order should fill second at the same price.");

    const auto remaining_order = book.get_order(2);
    expect(remaining_order.has_value(), "Second order should still be resting.");
    expect_equal(remaining_order->remaining_quantity, tradematch::Quantity{20}, "FIFO should leave the correct remainder.");
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
    expect_equal(snapshot.bids.front().order_ids.front(), tradematch::OrderId{1}, "Order ID should be retained at the level.");
}

void test_market_order_expires_after_available_liquidity() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10000});
    submit_checked(book, {2, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10050});

    const auto result = submit_checked(book, {3, tradematch::Side::Buy, tradematch::OrderType::Market, 80, 0});
    expect(result.accepted, "Market order should be accepted.");
    expect(result.expired, "Unfilled market remainder should expire instead of resting.");
    expect_equal(result.filled_quantity, tradematch::Quantity{60}, "Market order should consume all available liquidity.");
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

    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});
    expect(replace.replaced, "Replace should succeed for an active order.");
    expect_equal(replace.previous_remaining_quantity,
                 tradematch::Quantity{40},
                 "Replace should report the previous resting quantity.");
    expect(replace.submit_result.rested, "The replacement order should rest in the book.");

    const auto result = submit_checked(book, {3, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    expect_equal(result.trades.size(), std::size_t{2}, "Sell order should match both bids after replacement.");
    expect_equal(result.trades[0].buy_order_id, tradematch::OrderId{2}, "Unreplaced order should keep earlier priority.");
    expect_equal(result.trades[1].buy_order_id,
                 tradematch::OrderId{1},
                 "Replaced order should lose time priority and fill second.");

    const auto remaining_order = book.get_order(1);
    expect(remaining_order.has_value(), "Replaced order should still be resting with remaining quantity.");
    expect_equal(remaining_order->remaining_quantity, tradematch::Quantity{30}, "Replace scenario should leave the expected remainder.");
}

void test_replace_rejects_invalid_request_without_removing_resting_order() {
    tradematch::OrderBook book;

    submit_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 10000});
    const auto replace = replace_checked(book, {1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 0});

    expect(!replace.replaced, "Invalid replacement should fail.");
    expect(book.has_order(1), "Rejected replacement should leave the original resting order untouched.");

    const auto order = book.get_order(1);
    expect(order.has_value(), "Original order should still be visible after rejected replacement.");
    expect_equal(order->price, tradematch::Price{10000}, "Rejected replacement should preserve the old price.");
    expect_equal(order->remaining_quantity,
                 tradematch::Quantity{25},
                 "Rejected replacement should preserve the old quantity.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

}  // namespace

int main() {
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
        {"replace_rejects_invalid_request_without_removing_resting_order",
         test_replace_rejects_invalid_request_without_removing_resting_order},
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
