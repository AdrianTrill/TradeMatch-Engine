#include "tradematch/matching_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace tradematch {

MatchingEngine::~MatchingEngine() {
    // Detach first so no book ever calls into a destroyed forwarder.
    for (auto& instrument : instruments_) {
        instrument.book->set_listener(nullptr);
        instrument.book->set_order_listener(nullptr);
    }
}

SymbolId MatchingEngine::add_symbol(std::string_view name, const BookCapacity& capacity) {
    if (const auto existing = find_symbol(name)) {
        return *existing;
    }
    const auto symbol = static_cast<SymbolId>(instruments_.size());
    instruments_.push_back(
        Instrument{std::string{name}, capacity, std::make_unique<OrderBook>(capacity), nullptr, nullptr});
    attach(instruments_.back(), symbol);
    return symbol;
}

std::optional<SymbolId> MatchingEngine::find_symbol(std::string_view name) const {
    const auto it =
        std::ranges::find_if(instruments_, [&](const Instrument& instrument) { return instrument.name == name; });
    if (it == instruments_.end()) {
        return std::nullopt;
    }
    return static_cast<SymbolId>(it - instruments_.begin());
}

std::string_view MatchingEngine::symbol_name(SymbolId symbol) const {
    return instruments_.at(symbol).name;
}

OrderBook& MatchingEngine::book(SymbolId symbol) {
    return *instruments_.at(symbol).book;
}

const OrderBook& MatchingEngine::book(SymbolId symbol) const {
    return *instruments_.at(symbol).book;
}

const BookCapacity& MatchingEngine::capacity(SymbolId symbol) const {
    return instruments_.at(symbol).capacity;
}

SubmitResult MatchingEngine::submit(SymbolId symbol, const OrderRequest& order) {
    if (!has_symbol(symbol)) {
        SubmitResult result;
        result.order_id = order.order_id;
        result.remaining_quantity = order.quantity;
        result.reject_reason = RejectReason::UnknownSymbol;
        result.message = "Unknown symbol.";
        return result;
    }
    return instruments_[symbol].book->submit(order);
}

CancelResult MatchingEngine::cancel(SymbolId symbol, OrderId order_id) {
    if (!has_symbol(symbol)) {
        CancelResult result;
        result.order_id = order_id;
        result.reject_reason = RejectReason::UnknownSymbol;
        result.message = "Unknown symbol.";
        return result;
    }
    return instruments_[symbol].book->cancel(order_id);
}

ReplaceResult MatchingEngine::replace(SymbolId symbol, const OrderRequest& replacement) {
    if (!has_symbol(symbol)) {
        ReplaceResult result;
        result.order_id = replacement.order_id;
        result.reject_reason = RejectReason::UnknownSymbol;
        result.message = "Unknown symbol.";
        return result;
    }
    return instruments_[symbol].book->replace(replacement);
}

std::vector<std::pair<SymbolId, OrderId>> MatchingEngine::expire_day_orders() {
    std::vector<std::pair<SymbolId, OrderId>> expired;
    for (SymbolId symbol = 0; symbol < instruments_.size(); ++symbol) {
        for (const OrderId order_id : instruments_[symbol].book->expire_day_orders()) {
            expired.emplace_back(symbol, order_id);
        }
    }
    return expired;
}

void MatchingEngine::set_listener(MarketDataListener* listener) {
    listener_ = listener;
    for (SymbolId symbol = 0; symbol < instruments_.size(); ++symbol) {
        attach(instruments_[symbol], symbol);
    }
}

void MatchingEngine::set_order_listener(OrderFeedListener* listener) {
    order_listener_ = listener;
    for (SymbolId symbol = 0; symbol < instruments_.size(); ++symbol) {
        attach(instruments_[symbol], symbol);
    }
}

void MatchingEngine::attach(Instrument& instrument, SymbolId symbol) {
    if (listener_ == nullptr) {
        instrument.book->set_listener(nullptr);
        instrument.forwarder.reset();
    } else {
        instrument.forwarder = std::make_unique<Forwarder>(symbol, listener_);
        instrument.book->set_listener(instrument.forwarder.get());
    }
    if (order_listener_ == nullptr) {
        instrument.book->set_order_listener(nullptr);
        instrument.order_forwarder.reset();
    } else {
        instrument.order_forwarder = std::make_unique<OrderForwarder>(symbol, order_listener_);
        instrument.book->set_order_listener(instrument.order_forwarder.get());
    }
}

}  // namespace tradematch
