#ifndef TRADEMATCH_TESTS_FEED_TAPE_HPP
#define TRADEMATCH_TESTS_FEED_TAPE_HPP

#include "tradematch/itch_feed.hpp"
#include "tradematch/matching_engine.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tradematch::testing {

// Keeps the trades an ITCH replica reports.
class TradeTape final : public itch::FeedTradeListener {
public:
    void on_trade(const itch::FeedTrade& trade) override {
        trades_.push_back(trade);
    }

    const std::vector<itch::FeedTrade>& trades() const noexcept {
        return trades_;
    }

private:
    std::vector<itch::FeedTrade> trades_;
};

// Whether two trades agree, apart from their match numbers, which only the feed assigns.
inline bool same_trade(const itch::FeedTrade& left, const itch::FeedTrade& right) {
    return left.symbol == right.symbol && left.timestamp == right.timestamp && left.price == right.price &&
           left.shares == right.shares && left.type == right.type && left.aggressor == right.aggressor;
}

// Passes an engine's order events on to its publisher, and writes down the tape the feed should print: each continuous
// trade as an execution, and each auction as a single cross.
class ExpectedTape final : public OrderFeedListener {
public:
    explicit ExpectedTape(itch::FeedPublisher& publisher) : publisher_(publisher) {}

    // Stamps the publisher's messages, and the trades they should print, with the time.
    void set_time(std::uint64_t time) noexcept {
        publisher_.set_time(time);
        time_ = time;
    }

    void on_order_events(SymbolId symbol, std::span<const OrderEvent> events, std::span<const Trade> trades) override {
        publisher_.on_order_events(symbol, events, trades);
        std::uint64_t crossed = 0;
        std::uint32_t cross_price = 0;
        for (const Trade& trade : trades) {
            const auto price = static_cast<std::uint32_t>(trade.price * publisher_.price_multiplier());
            if (trade.auction) {
                crossed += trade.quantity;
                cross_price = price;
            } else {
                trades_.push_back(itch::FeedTrade{symbol, time_, price, trade.quantity, 0, 'E', trade.aggressor_side});
            }
        }
        if (crossed > 0U) {
            trades_.push_back(itch::FeedTrade{symbol, time_, cross_price, crossed, 0, 'Q', std::nullopt});
        }
    }

    const std::vector<itch::FeedTrade>& trades() const noexcept {
        return trades_;
    }

private:
    itch::FeedPublisher& publisher_;
    std::uint64_t time_{0};
    std::vector<itch::FeedTrade> trades_;
};

}  // namespace tradematch::testing

#endif
