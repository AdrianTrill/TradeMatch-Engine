#include "tradematch/order_book.hpp"

#include <iostream>
#include <span>
#include <vector>

namespace {

using tradematch::BookSnapshot;
using tradematch::CancelResult;
using tradematch::OrderBook;
using tradematch::OrderRequest;
using tradematch::OrderType;
using tradematch::PriceLevelView;
using tradematch::ReplaceResult;
using tradematch::Side;
using tradematch::SubmitResult;
using tradematch::TimeInForce;
using tradematch::Trade;

void print_levels(const std::vector<PriceLevelView>& levels, const char* label) {
    std::cout << "  " << label << '\n';
    if (levels.empty()) {
        std::cout << "    <empty>\n";
        return;
    }

    for (const auto& level : levels) {
        std::cout << "    " << tradematch::format_price(level.price) << " qty=" << level.total_quantity
                  << " orders=" << level.order_count << '\n';
    }
}

void print_snapshot(const BookSnapshot& snapshot) {
    std::cout << "Book snapshot:\n";
    print_levels(snapshot.asks, "Asks");
    print_levels(snapshot.bids, "Bids");
}

void print_trades(std::span<const Trade> trades) {
    for (const auto& trade : trades) {
        std::cout << "  trade#" << trade.trade_id << " buy=" << trade.buy_order_id << " sell=" << trade.sell_order_id
                  << " qty=" << trade.quantity << " price=" << tradematch::format_price(trade.price)
                  << " aggressor=" << tradematch::to_string(trade.aggressor_side) << '\n';
    }
}

void print_submit_result(const SubmitResult& result) {
    std::cout << "  accepted=" << (result.accepted ? "true" : "false") << " filled=" << result.filled_quantity
              << " remaining=" << result.remaining_quantity << " rested=" << (result.rested ? "true" : "false")
              << " expired=" << (result.expired ? "true" : "false");
    if (!result.accepted) {
        std::cout << " reject=" << tradematch::to_string(result.reject_reason);
    }
    std::cout << '\n';
    std::cout << "  " << result.message << '\n';
    print_trades(result.trades);
}

void print_cancel_result(const CancelResult& result) {
    std::cout << "Cancel order " << result.order_id << '\n';
    std::cout << "  cancelled=" << (result.cancelled ? "true" : "false")
              << " cancelled_qty=" << result.cancelled_quantity << '\n';
    std::cout << "  " << result.message << '\n';
}

void print_replace_result(const ReplaceResult& result) {
    std::cout << "  replaced=" << (result.replaced ? "true" : "false")
              << " priority_kept=" << (result.priority_kept ? "true" : "false")
              << " previous_remaining=" << result.previous_remaining_quantity << '\n';
    std::cout << "  " << result.message << '\n';
    print_trades(result.submit_result.trades);
}

void describe(const char* action, const OrderRequest& order) {
    std::cout << action << ' ' << order.order_id << " side=" << tradematch::to_string(order.side)
              << " type=" << tradematch::to_string(order.type) << " qty=" << order.quantity;
    if (order.type == OrderType::Limit) {
        std::cout << " price=" << tradematch::format_price(order.price);
    }
    if (order.time_in_force != TimeInForce::GoodTillCancel) {
        std::cout << " tif=" << tradematch::to_string(order.time_in_force);
    }
    if (order.post_only) {
        std::cout << " post_only";
    }
    std::cout << '\n';
}

void submit_and_print(OrderBook& book, const OrderRequest& order) {
    describe("Submit order", order);
    print_submit_result(book.submit(order));
    print_snapshot(book.snapshot());
    std::cout << '\n';
}

void replace_and_print(OrderBook& book, const OrderRequest& order) {
    describe("Replace order", order);
    print_replace_result(book.replace(order));
    print_snapshot(book.snapshot());
    std::cout << '\n';
}

}  // namespace

int main() {
    OrderBook book;

    // Two bids at the same price; reducing the first one in place keeps it at the front of the queue.
    submit_and_print(book, OrderRequest{1, Side::Buy, OrderType::Limit, 100, 10050});
    submit_and_print(book, OrderRequest{2, Side::Buy, OrderType::Limit, 60, 10050});
    replace_and_print(book, OrderRequest{1, Side::Buy, OrderType::Limit, 80, 10050});
    submit_and_print(book, OrderRequest{3, Side::Sell, OrderType::Limit, 120, 10050});

    // A post-only ask rests; a post-only bid that would cross it is rejected instead of trading.
    submit_and_print(book, OrderRequest{4, Side::Sell, OrderType::Limit, 50, 10100, TimeInForce::GoodTillCancel, true});
    submit_and_print(book, OrderRequest{5, Side::Buy, OrderType::Limit, 10, 10100, TimeInForce::GoodTillCancel, true});

    // Fill-or-kill needs 80 but only 50 is offered, so nothing trades. Then a market order and an IOC take the rest.
    submit_and_print(book, OrderRequest{6, Side::Buy, OrderType::Limit, 80, 10100, TimeInForce::FillOrKill});
    submit_and_print(book, OrderRequest{7, Side::Buy, OrderType::Market, 40, 0});
    submit_and_print(book, OrderRequest{8, Side::Buy, OrderType::Limit, 30, 10100, TimeInForce::ImmediateOrCancel});

    print_cancel_result(book.cancel(2));
    print_snapshot(book.snapshot());

    return 0;
}
