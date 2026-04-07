#include "tradematch/order_book.hpp"

#include <iostream>
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

void print_levels(const std::vector<PriceLevelView>& levels, const char* label) {
    std::cout << "  " << label << '\n';
    if (levels.empty()) {
        std::cout << "    <empty>\n";
        return;
    }

    for (const auto& level : levels) {
        std::cout << "    " << tradematch::format_price(level.price)
                  << " qty=" << level.total_quantity
                  << " orders=" << level.order_count
                  << '\n';
    }
}

void print_snapshot(const BookSnapshot& snapshot) {
    std::cout << "Book snapshot:\n";
    print_levels(snapshot.asks, "Asks");
    print_levels(snapshot.bids, "Bids");
}

void print_submit_result(const SubmitResult& result) {
    std::cout << "  accepted=" << (result.accepted ? "true" : "false")
              << " filled=" << result.filled_quantity
              << " remaining=" << result.remaining_quantity
              << " rested=" << (result.rested ? "true" : "false")
              << " expired=" << (result.expired ? "true" : "false")
              << '\n';
    std::cout << "  " << result.message << '\n';

    for (const auto& trade : result.trades) {
        std::cout << "  trade#" << trade.trade_id
                  << " buy=" << trade.buy_order_id
                  << " sell=" << trade.sell_order_id
                  << " qty=" << trade.quantity
                  << " price=" << tradematch::format_price(trade.price)
                  << " aggressor=" << tradematch::to_string(trade.aggressor_side)
                  << '\n';
    }
}

void print_cancel_result(const CancelResult& result) {
    std::cout << "Cancel order " << result.order_id << '\n';
    std::cout << "  cancelled=" << (result.cancelled ? "true" : "false")
              << " cancelled_qty=" << result.cancelled_quantity
              << '\n';
    std::cout << "  " << result.message << '\n';
}

void print_replace_result(const ReplaceResult& result) {
    std::cout << "Replace order " << result.order_id << '\n';
    std::cout << "  replaced=" << (result.replaced ? "true" : "false")
              << " previous_remaining=" << result.previous_remaining_quantity
              << '\n';
    std::cout << "  " << result.message << '\n';

    if (result.replaced) {
        print_submit_result(result.submit_result);
    }
}

void submit_and_print(OrderBook& book, const OrderRequest& order) {
    std::cout << "Submit order " << order.order_id
              << " side=" << tradematch::to_string(order.side)
              << " type=" << tradematch::to_string(order.type)
              << " qty=" << order.quantity;

    if (order.type == OrderType::Limit) {
        std::cout << " price=" << tradematch::format_price(order.price);
    }
    std::cout << '\n';

    const auto result = book.submit(order);
    print_submit_result(result);
    print_snapshot(book.snapshot());
    std::cout << '\n';
}

void replace_and_print(OrderBook& book, const OrderRequest& order) {
    std::cout << "Replace request " << order.order_id
              << " side=" << tradematch::to_string(order.side)
              << " type=" << tradematch::to_string(order.type)
              << " qty=" << order.quantity;

    if (order.type == OrderType::Limit) {
        std::cout << " price=" << tradematch::format_price(order.price);
    }
    std::cout << '\n';

    const auto result = book.replace(order);
    print_replace_result(result);
    print_snapshot(book.snapshot());
    std::cout << '\n';
}

}  // namespace

int main() {
    OrderBook book;

    submit_and_print(book, OrderRequest{1, Side::Buy, OrderType::Limit, 100, 10050});
    submit_and_print(book, OrderRequest{2, Side::Buy, OrderType::Limit, 60, 10050});
    replace_and_print(book, OrderRequest{1, Side::Buy, OrderType::Limit, 80, 10050});
    submit_and_print(book, OrderRequest{3, Side::Sell, OrderType::Limit, 120, 10050});
    submit_and_print(book, OrderRequest{4, Side::Sell, OrderType::Limit, 50, 10100});
    submit_and_print(book, OrderRequest{5, Side::Buy, OrderType::Market, 40, 0});

    print_cancel_result(book.cancel(4));
    print_snapshot(book.snapshot());

    return 0;
}
