#include "tradematch/order_book.hpp"

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

void test_exact_match() {
    tradematch::OrderBook book;

    const auto resting = book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 50, 10000});
    expect(resting.rested, "Initial limit order should rest in the book.");

    const auto match = book.submit({2, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    expect(match.accepted, "Crossing order should be accepted.");
    expect_equal(match.trades.size(), std::size_t{1}, "Exact match should generate one trade.");
    expect_equal(match.trades.front().price, tradematch::Price{10000}, "Trade should execute at the resting price.");
    expect_equal(match.trades.front().quantity, tradematch::Quantity{50}, "Trade quantity should match both orders.");
    expect_equal(book.order_count(), std::size_t{0}, "No orders should remain after an exact match.");
}

void test_partial_fill_rests_remainder() {
    tradematch::OrderBook book;

    book.submit({1, tradematch::Side::Sell, tradematch::OrderType::Limit, 80, 10100});
    const auto result = book.submit({2, tradematch::Side::Buy, tradematch::OrderType::Limit, 120, 10100});

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

    book.submit({1, tradematch::Side::Sell, tradematch::OrderType::Limit, 50, 10000});
    book.submit({2, tradematch::Side::Sell, tradematch::OrderType::Limit, 70, 10050});

    const auto result = book.submit({3, tradematch::Side::Buy, tradematch::OrderType::Limit, 100, 10100});
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

    book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});
    book.submit({2, tradematch::Side::Buy, tradematch::OrderType::Limit, 40, 10000});

    const auto result = book.submit({3, tradematch::Side::Sell, tradematch::OrderType::Limit, 60, 10000});
    expect_equal(result.trades.size(), std::size_t{2}, "Sell order should match both resting bids.");
    expect_equal(result.trades[0].buy_order_id, tradematch::OrderId{1}, "Older order should fill first at the same price.");
    expect_equal(result.trades[1].buy_order_id, tradematch::OrderId{2}, "Newer order should fill second at the same price.");

    const auto remaining_order = book.get_order(2);
    expect(remaining_order.has_value(), "Second order should still be resting.");
    expect_equal(remaining_order->remaining_quantity, tradematch::Quantity{20}, "FIFO should leave the correct remainder.");
}

void test_cancel_order() {
    tradematch::OrderBook book;

    book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 25, 10000});
    const auto cancel = book.cancel(1);

    expect(cancel.cancelled, "Active order should cancel successfully.");
    expect_equal(cancel.cancelled_quantity, tradematch::Quantity{25}, "Cancel should report remaining quantity.");
    expect(!book.has_order(1), "Cancelled order should be removed from the index.");

    const auto second_cancel = book.cancel(1);
    expect(!second_cancel.cancelled, "Cancelling a missing order should fail cleanly.");
}

void test_unmatched_order_rests_in_book() {
    tradematch::OrderBook book;

    const auto result = book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 35, 9950});
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

    book.submit({1, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10000});
    book.submit({2, tradematch::Side::Sell, tradematch::OrderType::Limit, 30, 10050});

    const auto result = book.submit({3, tradematch::Side::Buy, tradematch::OrderType::Market, 80, 0});
    expect(result.accepted, "Market order should be accepted.");
    expect(result.expired, "Unfilled market remainder should expire instead of resting.");
    expect_equal(result.filled_quantity, tradematch::Quantity{60}, "Market order should consume all available liquidity.");
    expect_equal(result.remaining_quantity, tradematch::Quantity{20}, "Remainder should be reported after expiry.");
    expect_equal(book.order_count(), std::size_t{0}, "Market order should leave no resting asks in this scenario.");
}

void test_invalid_and_duplicate_orders_are_rejected() {
    tradematch::OrderBook book;

    const auto invalid = book.submit({0, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 10000});
    expect(!invalid.accepted, "Invalid order ID should be rejected.");

    const auto first = book.submit({1, tradematch::Side::Buy, tradematch::OrderType::Limit, 10, 10000});
    expect(first.accepted, "Valid order should be accepted.");

    const auto duplicate = book.submit({1, tradematch::Side::Sell, tradematch::OrderType::Limit, 10, 10000});
    expect(!duplicate.accepted, "Duplicate active order ID should be rejected.");
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
