#ifndef TRADEMATCH_BOOK_STATE_HPP
#define TRADEMATCH_BOOK_STATE_HPP

#include "tradematch/types.hpp"

#include <optional>
#include <vector>

namespace tradematch {

// The complete logical state of an order book: everything that decides how the book handles future requests, and
// nothing about how it is stored. Two books with equal states behave identically from then on. OrderBook converts in
// both directions with export_state() and import_state(); snapshots, state hashes, and recovery build on that.

struct RestingOrderState {
    OrderId order_id{};
    SequenceNumber sequence{};
    Quantity visible_quantity{};  // displayed quantity; for icebergs, what is left of the current tranche
    Quantity hidden_quantity{};   // iceberg reserve that is not displayed yet
    Quantity display_quantity{};  // iceberg tranche size, 0 for regular orders
    OwnerId owner{};
    TimeInForce time_in_force{TimeInForce::GoodTillCancel};

    bool operator==(const RestingOrderState&) const = default;
};

struct PriceLevelState {
    Price price{};
    std::vector<RestingOrderState> orders;  // in time priority, which is ascending sequence

    bool operator==(const PriceLevelState&) const = default;
};

struct StopOrderState {
    OrderRequest request;
    SequenceNumber sequence{};  // arrival, the tie-breaker between stops with the same trigger price

    bool operator==(const StopOrderState&) const = default;
};

struct AuctionMarketOrderState {
    OrderId order_id{};
    Quantity quantity{};
    TimeInForce time_in_force{TimeInForce::GoodTillCancel};

    bool operator==(const AuctionMarketOrderState&) const = default;
};

struct BookState {
    TradingPhase phase{TradingPhase::Continuous};
    std::optional<Price> last_trade_price;
    SequenceNumber next_sequence{1};
    TradeId next_trade_id{1};
    std::vector<PriceLevelState> bids;                   // best (highest) price first
    std::vector<PriceLevelState> asks;                   // best (lowest) price first
    std::vector<StopOrderState> stops;                   // pending stop orders, in arrival order
    std::vector<AuctionMarketOrderState> auction_buys;   // market orders queued for the uncross, in arrival order
    std::vector<AuctionMarketOrderState> auction_sells;  // likewise

    bool operator==(const BookState&) const = default;
};

}  // namespace tradematch

#endif
