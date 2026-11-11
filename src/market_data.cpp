#include "tradematch/market_data.hpp"

#include <algorithm>
#include <stdexcept>

namespace tradematch {

void DepthBook::apply(const LevelUpdate& update) {
    const auto write = [&](auto& side_levels) {
        if (update.quantity == 0U) {
            side_levels.erase(update.price);
        } else {
            side_levels[update.price] = DepthLevel{update.price, update.quantity, update.order_count};
        }
    };
    if (update.side == Side::Buy) {
        write(bids_);
    } else {
        write(asks_);
    }
}

std::vector<DepthLevel> DepthBook::levels(Side side, std::size_t max_levels) const {
    std::vector<DepthLevel> result;
    const auto collect = [&](const auto& side_levels) {
        for (const auto& entry : side_levels) {
            if (result.size() >= max_levels) {
                break;
            }
            result.push_back(entry.second);
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return result;
}

TopOfBook DepthBook::top() const {
    TopOfBook top_of_book;
    if (!bids_.empty()) {
        top_of_book.bid_price = bids_.begin()->first;
        top_of_book.bid_quantity = bids_.begin()->second.quantity;
    }
    if (!asks_.empty()) {
        top_of_book.ask_price = asks_.begin()->first;
        top_of_book.ask_quantity = asks_.begin()->second.quantity;
    }
    return top_of_book;
}

CandleSeries::CandleSeries(Timestamp interval) : interval_(interval) {
    if (interval_ <= 0) {
        throw std::invalid_argument("candle interval must be positive");
    }
}

void CandleSeries::add_trade(Timestamp timestamp, Price price, Quantity quantity) {
    // Floor division, so bars align correctly for negative (pre-epoch) simulated times too.
    Timestamp start = timestamp / interval_ * interval_;
    if (start > timestamp) {
        start -= interval_;
    }

    if (candles_.empty() || candles_.back().start != start) {
        if (!candles_.empty() && start < candles_.back().start) {
            throw std::invalid_argument("trades must arrive in time order");
        }
        candles_.push_back(Candle{start, price, price, price, price, 0U, 0U});
    }
    Candle& candle = candles_.back();
    candle.high = std::max(candle.high, price);
    candle.low = std::min(candle.low, price);
    candle.close = price;
    candle.volume += quantity;
    ++candle.trade_count;
}

MarketDataFeed::MarketDataFeed(const FeedConfig& config) : config_(config) {}

MarketDataFeed::SymbolState& MarketDataFeed::state(SymbolId symbol) {
    while (symbols_.size() <= symbol) {
        symbols_.emplace_back(config_.candle_interval);
    }
    return symbols_[symbol];
}

const MarketDataFeed::SymbolState& MarketDataFeed::state(SymbolId symbol) const {
    static const SymbolState empty{1};
    return symbol < symbols_.size() ? symbols_[symbol] : empty;
}

void MarketDataFeed::record(FeedMessage message) {
    message.sequence = ++sequence_;
    message.timestamp = now_;
    if (config_.record_messages) {
        messages_.push_back(message);
    }
}

void MarketDataFeed::on_trade(SymbolId symbol, const Trade& trade) {
    SymbolState& symbol_state = state(symbol);
    symbol_state.last_price = trade.price;
    symbol_state.volume += trade.quantity;
    symbol_state.notional += static_cast<long double>(trade.price) * static_cast<long double>(trade.quantity);
    symbol_state.candles.add_trade(now_, trade.price, trade.quantity);
    symbol_state.tape.push_back(trade);
    while (symbol_state.tape.size() > config_.tape_length) {
        symbol_state.tape.pop_front();
    }

    FeedMessage message;
    message.symbol = symbol;
    message.kind = FeedMessage::Kind::Trade;
    message.trade = trade;
    record(message);
}

void MarketDataFeed::on_level_update(SymbolId symbol, const LevelUpdate& update) {
    state(symbol).depth.apply(update);

    FeedMessage message;
    message.symbol = symbol;
    message.kind = FeedMessage::Kind::Level;
    message.level = update;
    record(message);
}

const DepthBook& MarketDataFeed::depth(SymbolId symbol) const {
    return state(symbol).depth;
}

TopOfBook MarketDataFeed::top_of_book(SymbolId symbol) const {
    return state(symbol).depth.top();
}

const std::deque<Trade>& MarketDataFeed::recent_trades(SymbolId symbol) const {
    return state(symbol).tape;
}

const CandleSeries& MarketDataFeed::candles(SymbolId symbol) const {
    return state(symbol).candles;
}

std::optional<Price> MarketDataFeed::last_price(SymbolId symbol) const {
    return state(symbol).last_price;
}

Volume MarketDataFeed::traded_volume(SymbolId symbol) const {
    return state(symbol).volume;
}

std::optional<double> MarketDataFeed::vwap(SymbolId symbol) const {
    const SymbolState& symbol_state = state(symbol);
    if (symbol_state.volume == 0U) {
        return std::nullopt;
    }
    return static_cast<double>(symbol_state.notional / static_cast<long double>(symbol_state.volume));
}

std::vector<FeedMessage> MarketDataFeed::drain_messages() {
    std::vector<FeedMessage> drained;
    drained.swap(messages_);
    return drained;
}

}  // namespace tradematch
