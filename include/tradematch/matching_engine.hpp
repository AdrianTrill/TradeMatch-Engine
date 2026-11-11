#ifndef TRADEMATCH_MATCHING_ENGINE_HPP
#define TRADEMATCH_MATCHING_ENGINE_HPP

#include "tradematch/book_listener.hpp"
#include "tradematch/order_book.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tradematch {

using SymbolId = std::uint32_t;

// Market data from every book in a matching engine, tagged with the symbol it belongs to.
class MarketDataListener {
public:
    MarketDataListener() = default;
    MarketDataListener(const MarketDataListener&) = default;
    MarketDataListener& operator=(const MarketDataListener&) = default;
    MarketDataListener(MarketDataListener&&) = default;
    MarketDataListener& operator=(MarketDataListener&&) = default;
    virtual ~MarketDataListener() = default;

    virtual void on_trade(SymbolId symbol, const Trade& trade) = 0;
    virtual void on_level_update(SymbolId symbol, const LevelUpdate& update) = 0;
};

// Routes requests to one order book per instrument. Order IDs are scoped to their symbol. Books are independent and
// deterministic, so the engine as a whole is deterministic for a given request sequence.
class MatchingEngine {
public:
    MatchingEngine() = default;
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;
    ~MatchingEngine();

    // Adds an instrument and returns its ID. Names must be unique; adding an existing name returns its ID.
    SymbolId add_symbol(std::string_view name, const BookCapacity& capacity = {});
    std::optional<SymbolId> find_symbol(std::string_view name) const;
    std::string_view symbol_name(SymbolId symbol) const;

    std::size_t symbol_count() const noexcept {
        return instruments_.size();
    }

    bool has_symbol(SymbolId symbol) const noexcept {
        return symbol < instruments_.size();
    }

    OrderBook& book(SymbolId symbol);
    const OrderBook& book(SymbolId symbol) const;

    // Requests for an unknown symbol are rejected with RejectReason::UnknownSymbol.
    SubmitResult submit(SymbolId symbol, const OrderRequest& order);
    CancelResult cancel(SymbolId symbol, OrderId order_id);
    ReplaceResult replace(SymbolId symbol, const OrderRequest& replacement);

    // End-of-day processing across every instrument, returning (symbol, order ID) pairs in symbol order.
    std::vector<std::pair<SymbolId, OrderId>> expire_day_orders();

    // Attaches one listener to every current and future book, or detaches it with nullptr.
    void set_listener(MarketDataListener* listener);

private:
    class Forwarder final : public BookListener {
    public:
        Forwarder(SymbolId symbol, MarketDataListener* target) : symbol_(symbol), target_(target) {}
        void on_trade(const Trade& trade) override {
            target_->on_trade(symbol_, trade);
        }
        void on_level_update(const LevelUpdate& update) override {
            target_->on_level_update(symbol_, update);
        }

    private:
        SymbolId symbol_;
        MarketDataListener* target_;
    };

    struct Instrument {
        std::string name;
        std::unique_ptr<OrderBook> book;
        std::unique_ptr<Forwarder> forwarder;
    };

    void attach(Instrument& instrument, SymbolId symbol);

    std::vector<Instrument> instruments_;
    MarketDataListener* listener_{nullptr};
};

}  // namespace tradematch

#endif
