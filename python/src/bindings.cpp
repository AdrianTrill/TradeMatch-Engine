// Python bindings: the order book, the market simulator with complete trade and quote records, and ITCH replay.
// Results are copied into owning objects, since the engine's results only view buffers that its next request reuses.
#include "tradematch/detail/file_io.hpp"
#include "tradematch/itch.hpp"
#include "tradematch/itch_feed.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"
#include "tradematch/simulator.hpp"
#include "tradematch/snapshot.hpp"
#include "tradematch/types.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace py = pybind11;
namespace tme = tradematch;

namespace {

struct SubmitOutcome {
    tme::OrderId order_id{};
    bool accepted{false};
    bool rested{false};
    bool expired{false};
    bool pending_trigger{false};
    bool queued_for_auction{false};
    bool self_trade_prevented{false};
    tme::RejectReason reject_reason{tme::RejectReason::None};
    tme::Quantity filled_quantity{};
    tme::Quantity remaining_quantity{};
    std::vector<tme::Trade> trades;
    std::vector<tme::OrderId> triggered_stops;
    std::vector<tme::OrderId> self_trade_cancels;
    std::string message;
};

SubmitOutcome own(const tme::SubmitResult& result) {
    return SubmitOutcome{result.order_id,
                         result.accepted,
                         result.rested,
                         result.expired,
                         result.pending_trigger,
                         result.queued_for_auction,
                         result.self_trade_prevented,
                         result.reject_reason,
                         result.filled_quantity,
                         result.remaining_quantity,
                         {result.trades.begin(), result.trades.end()},
                         {result.triggered_stops.begin(), result.triggered_stops.end()},
                         {result.self_trade_cancels.begin(), result.self_trade_cancels.end()},
                         std::string{result.message}};
}

struct CancelOutcome {
    tme::OrderId order_id{};
    bool cancelled{false};
    tme::RejectReason reject_reason{tme::RejectReason::None};
    tme::Quantity cancelled_quantity{};
    std::string message;
};

struct ReplaceOutcome {
    tme::OrderId order_id{};
    bool replaced{false};
    bool priority_kept{false};
    tme::RejectReason reject_reason{tme::RejectReason::None};
    tme::Quantity previous_remaining_quantity{};
    SubmitOutcome submit_result;
    std::string message;
};

ReplaceOutcome own(const tme::ReplaceResult& result) {
    return ReplaceOutcome{result.order_id,
                          result.replaced,
                          result.priority_kept,
                          result.reject_reason,
                          result.previous_remaining_quantity,
                          own(result.submit_result),
                          std::string{result.message}};
}

struct AuctionOutcome {
    bool executed{false};
    tme::Price price{};
    tme::Volume matched_quantity{};
    std::vector<tme::Trade> trades;
    std::vector<tme::OrderId> expired_market_orders;
    std::vector<tme::OrderId> triggered_stops;
};

using Level = std::tuple<tme::Price, tme::Volume, std::size_t>;  // price, displayed quantity, order count

std::vector<Level> levels_of(const std::vector<tme::PriceLevelView>& side, std::size_t count) {
    std::vector<Level> rows;
    for (std::size_t index = 0; index < side.size() && index < count; ++index) {
        rows.emplace_back(side[index].price, side[index].total_quantity, side[index].order_count);
    }
    return rows;
}

std::vector<Level> levels_of(const std::vector<tme::DepthLevel>& depth) {
    std::vector<Level> rows;
    rows.reserve(depth.size());
    for (const tme::DepthLevel& level : depth) {
        rows.emplace_back(level.price, level.quantity, level.order_count);
    }
    return rows;
}

tme::OrderRequest make_request(tme::OrderId order_id,
                               tme::Side side,
                               tme::Quantity quantity,
                               tme::Price price,
                               tme::OrderType type,
                               tme::TimeInForce time_in_force,
                               bool post_only,
                               tme::Price stop_price,
                               tme::Quantity display_quantity,
                               tme::OwnerId owner,
                               tme::SelfTradePrevention self_trade_prevention) {
    tme::OrderRequest request{order_id, side, type, quantity, price};
    request.time_in_force = time_in_force;
    request.post_only = post_only;
    request.stop_price = stop_price;
    request.display_quantity = display_quantity;
    request.owner = owner;
    request.self_trade_prevention = self_trade_prevention;
    return request;
}

// Long calls release the GIL, so simulators and replays in different threads run in parallel. An object isn't thread
// safe, though, so such a call marks its object busy while holding the GIL, and every other use of the object, which
// holds the GIL too, raises an error rather than racing it.
class BusyFlag {
public:
    void check_idle() const {
        if (busy_) {
            throw std::runtime_error("this object is busy in another thread");
        }
    }

    // Runs work without the GIL, with the object marked busy.
    template <typename Work>
    auto run_released(Work&& work) {
        check_idle();
        const Busy busy(busy_);
        const py::gil_scoped_release release;
        return std::forward<Work>(work)();
    }

private:
    // Destroyed after the GIL is taken back, so busy_ only changes under the GIL.
    class Busy {
    public:
        explicit Busy(bool& busy) noexcept : busy_(busy) {
            busy_ = true;
        }
        Busy(const Busy&) = delete;
        Busy& operator=(const Busy&) = delete;
        Busy(Busy&&) = delete;
        Busy& operator=(Busy&&) = delete;
        ~Busy() {
            busy_ = false;
        }

    private:
        bool& busy_;
    };

    bool busy_{false};
};

// Whole nanoseconds in a span of seconds, which must be a number from zero up to about 285 years.
tme::Timestamp nanoseconds_of(double seconds) {
    const double nanoseconds = seconds * 1e9;
    if (std::isnan(nanoseconds) || nanoseconds < 0.0 || nanoseconds >= 9e18) {
        throw py::value_error("a span of time must be from 0 to 9e9 seconds");
    }
    return static_cast<tme::Timestamp>(std::llround(nanoseconds));
}

char aggressor_code(const tme::Trade& trade) noexcept {
    if (trade.auction) {
        return 'X';
    }
    return trade.aggressor_side == tme::Side::Buy ? 'B' : 'S';
}

// A recorded trade: time, symbol, price, quantity, the aggressor ('B' or 'S', or 'X' for an auction trade), and the
// buy and sell orders' IDs.
using TradeRow = std::tuple<tme::Timestamp, std::string, tme::Price, tme::Quantity, char, tme::OrderId, tme::OrderId>;
// One symbol's top of book after a step: time, symbol, phase, bid, bid size, ask, ask size, fair value.
using QuoteRow = std::tuple<tme::Timestamp,
                            std::string,
                            std::string,
                            std::optional<tme::Price>,
                            tme::Volume,
                            std::optional<tme::Price>,
                            tme::Volume,
                            tme::Price>;

// The market simulator, recording every trade and, after every step, each symbol's top of book.
class Simulation final : public tme::OrderFeedListener {
public:
    Simulation(const tme::SimulationConfig& config, bool record_quotes)
        : simulator_(config), step_(config.step), record_quotes_(record_quotes) {
        for (std::size_t index = 0; index < simulator_.symbol_count(); ++index) {
            const tme::SymbolId id = simulator_.symbol(index);
            if (names_.size() <= id) {
                names_.resize(std::size_t{id} + 1);
            }
            names_[id] = std::string{simulator_.engine().symbol_name(id)};
        }
        simulator_.set_order_listener(this);
    }
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    Simulation(Simulation&&) = delete;
    Simulation& operator=(Simulation&&) = delete;
    ~Simulation() override {
        simulator_.set_order_listener(nullptr);
    }

    void on_order_events(tme::SymbolId symbol,
                         std::span<const tme::OrderEvent> /*events*/,
                         std::span<const tme::Trade> trades) override {
        for (const tme::Trade& trade : trades) {
            trades_.emplace_back(time_,
                                 names_.at(symbol),
                                 trade.price,
                                 trade.quantity,
                                 aggressor_code(trade),
                                 trade.buy_order_id,
                                 trade.sell_order_id);
        }
    }

    void step(std::size_t steps) {
        for (std::size_t count = 0; count < steps; ++count) {
            time_ = simulator_.now() + step_;  // everything in the step happens at its end time
            simulator_.step();
            record_quotes();
        }
    }

    // Runs for the number of whole steps nearest the span.
    void run(tme::Timestamp span) {
        step(static_cast<std::size_t>((span + (step_ / 2)) / step_));
    }

    // Runs to the end of the trading day: through the auctions and continuous trading to the close and the expiry of
    // day orders.
    void run_day() {
        while (simulator_.session() == tme::SessionPhase::Closed) {
            step(1);
        }
        while (simulator_.session() != tme::SessionPhase::Closed) {
            step(1);
        }
    }

    void check_idle() const {
        busy_.check_idle();
    }

    template <typename Work>
    auto run_released(Work&& work) {
        return busy_.run_released(std::forward<Work>(work));
    }

    std::vector<TradeRow> take_trades() {
        check_idle();
        return std::exchange(trades_, {});
    }

    std::vector<QuoteRow> take_quotes() {
        check_idle();
        return std::exchange(quotes_, {});
    }

    const tme::MarketSimulator& simulator() const {
        check_idle();
        return simulator_;
    }

    std::size_t index_of(const std::string& symbol) const {
        const tme::MarketSimulator& market = simulator();
        for (std::size_t index = 0; index < market.symbol_count(); ++index) {
            if (market.engine().symbol_name(market.symbol(index)) == symbol) {
                return index;
            }
        }
        throw py::key_error("no symbol " + symbol);
    }

    tme::SymbolId id_of(const std::string& symbol) const {
        return simulator().symbol(index_of(symbol));
    }

private:
    void record_quotes() {
        if (!record_quotes_) {
            return;
        }
        const std::string phase = tme::to_string(simulator_.session());
        for (std::size_t index = 0; index < simulator_.symbol_count(); ++index) {
            const tme::SymbolId id = simulator_.symbol(index);
            const tme::TopOfBook top = simulator_.feed().top_of_book(id);
            quotes_.emplace_back(simulator_.now(),
                                 names_.at(id),
                                 phase,
                                 top.bid_price,
                                 top.bid_quantity,
                                 top.ask_price,
                                 top.ask_quantity,
                                 simulator_.fair_value(index));
        }
    }

    BusyFlag busy_;
    tme::MarketSimulator simulator_;
    tme::Timestamp step_;
    bool record_quotes_;
    tme::Timestamp time_{0};
    std::vector<std::string> names_;
    std::vector<TradeRow> trades_;
    std::vector<QuoteRow> quotes_;
};

std::unique_ptr<Simulation> make_simulation(std::uint64_t seed,
                                            std::vector<std::string> symbols,
                                            tme::Price initial_price,
                                            double step_seconds,
                                            double opening_auction_seconds,
                                            double continuous_seconds,
                                            double closing_auction_seconds,
                                            double closed_seconds,
                                            std::size_t market_makers,
                                            std::size_t noise_traders,
                                            std::size_t momentum_traders,
                                            std::size_t execution_traders,
                                            double volatility,
                                            double news_probability,
                                            bool record_quotes) {
    const auto nanoseconds = [](double seconds) {
        const tme::Timestamp span = nanoseconds_of(seconds);
        if (span == 0) {
            throw py::value_error("durations must be at least a nanosecond");
        }
        return span;
    };
    if (symbols.empty()) {
        throw py::value_error("the simulator needs at least one symbol");
    }
    tme::SimulationConfig config;
    config.seed = seed;
    config.symbols = std::move(symbols);
    config.initial_price = initial_price;
    config.step = nanoseconds(step_seconds);
    config.opening_auction = nanoseconds(opening_auction_seconds);
    config.continuous = nanoseconds(continuous_seconds);
    config.closing_auction = nanoseconds(closing_auction_seconds);
    config.closed = nanoseconds(closed_seconds);
    config.market_makers = market_makers;
    config.noise_traders = noise_traders;
    config.momentum_traders = momentum_traders;
    config.execution_traders = execution_traders;
    config.volatility = volatility;
    config.news_probability = news_probability;
    config.measure_latency = false;
    return std::make_unique<Simulation>(config, record_quotes);
}

// A trade printed by an ITCH feed: time, symbol, price, shares, and the aggressor: 'B' or 'S', 'X' for a cross, or '?'
// for a trade with a non-displayed order, whose aggressor the feed doesn't show.
using FeedTradeRow = std::tuple<std::uint64_t, std::string, std::uint32_t, std::uint64_t, char>;

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        std::fclose(file);
    }
};

// Raises the OSError that fits, such as FileNotFoundError, if the file can't be opened.
std::unique_ptr<std::FILE, FileCloser> open_for_reading(const std::string& path) {
    std::unique_ptr<std::FILE, FileCloser> file(tme::detail::open_file(path, "rb"));
    if (!file) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path.c_str());
        throw py::error_already_set();
    }
    return file;
}

// Rebuilds every stock's order book from an ITCH 5.0 file, one stretch of the day at a time, and records the trades of
// the stocks it watches.
class ItchReplay final : public tme::itch::FeedTradeListener {
public:
    ItchReplay(const std::string& path, const std::vector<std::string>& watch)
        : file_(open_for_reading(path)), reader_(file_.get()), watch_(watch.begin(), watch.end()) {
        replica_.set_trade_listener(this);
    }
    ItchReplay(const ItchReplay&) = delete;
    ItchReplay& operator=(const ItchReplay&) = delete;
    ItchReplay(ItchReplay&&) = delete;
    ItchReplay& operator=(ItchReplay&&) = delete;
    ~ItchReplay() override {
        replica_.set_trade_listener(nullptr);
    }

    void on_trade(const tme::itch::FeedTrade& trade) override {
        if (trade.symbol >= watched_.size()) {
            watched_.resize(std::size_t{trade.symbol} + 1, Watched::Unknown);
        }
        Watched& watched = watched_[trade.symbol];
        if (watched == Watched::Unknown) {
            watched =
                watch_.contains(std::string{replica_.engine().symbol_name(trade.symbol)}) ? Watched::Yes : Watched::No;
        }
        if (watched == Watched::No) {
            return;
        }
        char aggressor = trade.type == 'Q' ? 'X' : '?';
        if (trade.aggressor) {
            aggressor = *trade.aggressor == tme::Side::Buy ? 'B' : 'S';
        }
        trades_.emplace_back(trade.timestamp,
                             std::string{replica_.engine().symbol_name(trade.symbol)},
                             trade.price,
                             trade.shares,
                             aggressor);
    }

    // Applies the file's messages up to the first one stamped after `until` nanoseconds since midnight, or to the end
    // of the file. Returns how many it applied.
    std::uint64_t advance(std::optional<std::uint64_t> until) {
        std::uint64_t applied = 0;
        if (pending_) {
            if (until && pending_time_ > *until) {
                return 0;
            }
            replica_.apply(std::span<const std::uint8_t>{pending_message_});
            pending_ = false;
            ++applied;
        }
        while (const auto message = reader_.next()) {
            const std::uint64_t time = tme::itch::timestamp_of(*message).value_or(0);
            if (until && time > *until) {
                pending_message_.assign(message->begin(), message->end());
                pending_time_ = time;
                pending_ = true;
                return applied;
            }
            replica_.apply(*message);
            ++applied;
        }
        finished_ = true;
        return applied;
    }

    bool finished() const {
        check_idle();
        return finished_ && !pending_;
    }

    const tme::itch::BookReplica& replica() const {
        check_idle();
        return replica_;
    }

    const tme::OrderBook& book(const std::string& symbol) const {
        const auto id = replica().find(symbol);
        if (!id) {
            throw py::key_error("no stock " + symbol + " in the feed so far");
        }
        return replica_.engine().book(*id);
    }

    const tme::itch::StockStats& stock_stats(const std::string& symbol) const {
        const auto id = replica().find(symbol);
        if (!id) {
            throw py::key_error("no stock " + symbol + " in the feed so far");
        }
        return replica_.stock_stats(*id);
    }

    std::vector<FeedTradeRow> take_trades() {
        check_idle();
        return std::exchange(trades_, {});
    }

    void check_idle() const {
        busy_.check_idle();
    }

    template <typename Work>
    auto run_released(Work&& work) {
        return busy_.run_released(std::forward<Work>(work));
    }

private:
    enum class Watched : std::uint8_t {
        Unknown,
        Yes,
        No
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    tme::itch::FileReader reader_;
    tme::itch::BookReplica replica_;
    std::unordered_set<std::string> watch_;
    std::vector<Watched> watched_;  // by symbol
    std::vector<FeedTradeRow> trades_;
    std::vector<std::uint8_t> pending_message_;  // the first message past the last limit, applied by the next advance
    std::uint64_t pending_time_{0};
    bool pending_{false};
    bool finished_{false};
    BusyFlag busy_;
};

// The best bid and offer with their displayed sizes, read from the best level of each side alone.
std::tuple<std::optional<tme::Price>, tme::Volume, std::optional<tme::Price>, tme::Volume> top_of(
    const tme::OrderBook& book) {
    const auto displayed = [&](tme::Side side, std::optional<tme::Price> price) {
        tme::Volume quantity = 0;
        if (price) {
            for (const tme::RestingOrderView& order : book.get_orders_at_level(side, *price)) {
                quantity += order.visible_quantity;
            }
        }
        return quantity;
    };
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    return {bid, displayed(tme::Side::Buy, bid), ask, displayed(tme::Side::Sell, ask)};
}

std::pair<std::vector<Level>, std::vector<Level>> depth_of(const tme::OrderBook& book, std::size_t levels) {
    const tme::BookSnapshot snapshot = book.snapshot();
    return {levels_of(snapshot.bids, levels), levels_of(snapshot.asks, levels)};
}

}  // namespace

PYBIND11_MODULE(_tradematch, module) {
    module.doc() = "TradeMatch Engine: a price-time-priority matching engine, its market simulator, and ITCH replay.";

    py::enum_<tme::Side>(module, "Side").value("BUY", tme::Side::Buy).value("SELL", tme::Side::Sell);
    py::enum_<tme::OrderType>(module, "OrderType")
        .value("LIMIT", tme::OrderType::Limit)
        .value("MARKET", tme::OrderType::Market)
        .value("STOP", tme::OrderType::Stop)
        .value("STOP_LIMIT", tme::OrderType::StopLimit);
    py::enum_<tme::TimeInForce>(module, "TimeInForce")
        .value("GTC", tme::TimeInForce::GoodTillCancel)
        .value("DAY", tme::TimeInForce::Day)
        .value("IOC", tme::TimeInForce::ImmediateOrCancel)
        .value("FOK", tme::TimeInForce::FillOrKill);
    py::enum_<tme::SelfTradePrevention>(module, "SelfTradePrevention")
        .value("NONE", tme::SelfTradePrevention::None)
        .value("CANCEL_NEWEST", tme::SelfTradePrevention::CancelNewest)
        .value("CANCEL_OLDEST", tme::SelfTradePrevention::CancelOldest)
        .value("CANCEL_BOTH", tme::SelfTradePrevention::CancelBoth);
    py::enum_<tme::TradingPhase>(module, "TradingPhase")
        .value("CONTINUOUS", tme::TradingPhase::Continuous)
        .value("AUCTION", tme::TradingPhase::Auction);
    {
        py::enum_<tme::RejectReason> reasons(module, "RejectReason");
        for (auto reason = static_cast<std::uint8_t>(tme::RejectReason::None);
             reason <= static_cast<std::uint8_t>(tme::RejectReason::NotAllowedInAuction);
             ++reason) {
            const auto value = static_cast<tme::RejectReason>(reason);
            reasons.value(tme::to_string(value), value);
        }
    }

    py::class_<tme::OrderRequest>(module, "OrderRequest", "A new order, or the replacement for a resting one.")
        .def(py::init(&make_request),
             py::arg("order_id"),
             py::arg("side"),
             py::arg("quantity"),
             py::arg("price") = 0,
             py::kw_only(),
             py::arg("type") = tme::OrderType::Limit,
             py::arg("time_in_force") = tme::TimeInForce::GoodTillCancel,
             py::arg("post_only") = false,
             py::arg("stop_price") = 0,
             py::arg("display_quantity") = 0,
             py::arg("owner") = 0,
             py::arg("self_trade_prevention") = tme::SelfTradePrevention::None)
        .def_readwrite("order_id", &tme::OrderRequest::order_id)
        .def_readwrite("side", &tme::OrderRequest::side)
        .def_readwrite("type", &tme::OrderRequest::type)
        .def_readwrite("quantity", &tme::OrderRequest::quantity)
        .def_readwrite("price", &tme::OrderRequest::price)
        .def_readwrite("time_in_force", &tme::OrderRequest::time_in_force)
        .def_readwrite("post_only", &tme::OrderRequest::post_only)
        .def_readwrite("stop_price", &tme::OrderRequest::stop_price)
        .def_readwrite("display_quantity", &tme::OrderRequest::display_quantity)
        .def_readwrite("owner", &tme::OrderRequest::owner)
        .def_readwrite("self_trade_prevention", &tme::OrderRequest::self_trade_prevention)
        .def("__repr__", [](const tme::OrderRequest& request) {
            return "OrderRequest(order_id=" + std::to_string(request.order_id) +
                   ", side=" + tme::to_string(request.side) + ", quantity=" + std::to_string(request.quantity) +
                   ", price=" + std::to_string(request.price) + ", type=" + tme::to_string(request.type) + ")";
        });

    py::class_<tme::Trade>(module, "Trade")
        .def_readonly("trade_id", &tme::Trade::trade_id)
        .def_readonly("buy_order_id", &tme::Trade::buy_order_id)
        .def_readonly("sell_order_id", &tme::Trade::sell_order_id)
        .def_readonly("price", &tme::Trade::price)
        .def_readonly("quantity", &tme::Trade::quantity)
        .def_readonly("aggressor_side", &tme::Trade::aggressor_side)
        .def_readonly("auction", &tme::Trade::auction)
        .def("__repr__", [](const tme::Trade& trade) {
            return "Trade(" + std::to_string(trade.quantity) + " @ " + std::to_string(trade.price) +
                   ", buy=" + std::to_string(trade.buy_order_id) + ", sell=" + std::to_string(trade.sell_order_id) +
                   (trade.auction ? ", auction)" : ")");
        });

    py::class_<SubmitOutcome>(module, "SubmitResult")
        .def_readonly("order_id", &SubmitOutcome::order_id)
        .def_readonly("accepted", &SubmitOutcome::accepted)
        .def_readonly("rested", &SubmitOutcome::rested)
        .def_readonly("expired", &SubmitOutcome::expired)
        .def_readonly("pending_trigger", &SubmitOutcome::pending_trigger, "A stop order waiting in the stop book.")
        .def_readonly("queued_for_auction",
                      &SubmitOutcome::queued_for_auction,
                      "A market order waiting for the auction uncross.")
        .def_readonly("self_trade_prevented",
                      &SubmitOutcome::self_trade_prevented,
                      "Self-trade prevention cancelled the incoming order's remainder.")
        .def_readonly("reject_reason", &SubmitOutcome::reject_reason)
        .def_readonly("filled_quantity", &SubmitOutcome::filled_quantity)
        .def_readonly("remaining_quantity", &SubmitOutcome::remaining_quantity)
        .def_readonly("trades", &SubmitOutcome::trades, "Every trade the request caused, including triggered stops'.")
        .def_readonly("triggered_stops",
                      &SubmitOutcome::triggered_stops,
                      "Stop orders the request activated, in order.")
        .def_readonly("self_trade_cancels",
                      &SubmitOutcome::self_trade_cancels,
                      "Resting orders that self-trade prevention cancelled.")
        .def_readonly("message", &SubmitOutcome::message)
        .def("__repr__", [](const SubmitOutcome& result) {
            return "SubmitResult(order_id=" + std::to_string(result.order_id) +
                   ", accepted=" + (result.accepted ? "True" : "False") +
                   ", filled=" + std::to_string(result.filled_quantity) +
                   ", remaining=" + std::to_string(result.remaining_quantity) +
                   ", trades=" + std::to_string(result.trades.size()) + ")";
        });

    py::class_<CancelOutcome>(module, "CancelResult")
        .def_readonly("order_id", &CancelOutcome::order_id)
        .def_readonly("cancelled", &CancelOutcome::cancelled)
        .def_readonly("reject_reason", &CancelOutcome::reject_reason)
        .def_readonly("cancelled_quantity", &CancelOutcome::cancelled_quantity)
        .def_readonly("message", &CancelOutcome::message);

    py::class_<ReplaceOutcome>(module, "ReplaceResult")
        .def_readonly("order_id", &ReplaceOutcome::order_id)
        .def_readonly("replaced", &ReplaceOutcome::replaced)
        .def_readonly("priority_kept", &ReplaceOutcome::priority_kept)
        .def_readonly("reject_reason", &ReplaceOutcome::reject_reason)
        .def_readonly("previous_remaining_quantity", &ReplaceOutcome::previous_remaining_quantity)
        .def_readonly("submit_result", &ReplaceOutcome::submit_result)
        .def_readonly("message", &ReplaceOutcome::message);

    py::class_<AuctionOutcome>(module, "AuctionResult")
        .def_readonly("executed", &AuctionOutcome::executed)
        .def_readonly("price", &AuctionOutcome::price)
        .def_readonly("matched_quantity", &AuctionOutcome::matched_quantity)
        .def_readonly("trades", &AuctionOutcome::trades)
        .def_readonly("expired_market_orders", &AuctionOutcome::expired_market_orders)
        .def_readonly("triggered_stops", &AuctionOutcome::triggered_stops);

    py::class_<tme::AuctionIndicative>(module, "AuctionIndicative")
        .def_readonly("price", &tme::AuctionIndicative::price)
        .def_readonly("matched_quantity", &tme::AuctionIndicative::matched_quantity)
        .def_readonly("buy_surplus", &tme::AuctionIndicative::buy_surplus)
        .def_readonly("sell_surplus", &tme::AuctionIndicative::sell_surplus);

    py::class_<tme::RestingOrderView>(module, "RestingOrder")
        .def_readonly("order_id", &tme::RestingOrderView::order_id)
        .def_readonly("side", &tme::RestingOrderView::side)
        .def_readonly("price", &tme::RestingOrderView::price)
        .def_readonly("remaining_quantity", &tme::RestingOrderView::remaining_quantity)
        .def_readonly("visible_quantity", &tme::RestingOrderView::visible_quantity)
        .def_readonly("display_quantity", &tme::RestingOrderView::display_quantity)
        .def_readonly("time_in_force", &tme::RestingOrderView::time_in_force)
        .def_readonly("owner", &tme::RestingOrderView::owner)
        .def("__repr__", [](const tme::RestingOrderView& order) {
            return "RestingOrder(" + std::to_string(order.order_id) + ", " + tme::to_string(order.side) + " " +
                   std::to_string(order.remaining_quantity) + " @ " + std::to_string(order.price) + ")";
        });

    py::class_<tme::PriceLevelView>(module, "PriceLevel")
        .def_readonly("price", &tme::PriceLevelView::price)
        .def_readonly("total_quantity", &tme::PriceLevelView::total_quantity)
        .def_readonly("order_count", &tme::PriceLevelView::order_count)
        .def_readonly("order_ids", &tme::PriceLevelView::order_ids);

    py::class_<tme::BookSnapshot>(module, "BookSnapshot")
        .def_readonly("bids", &tme::BookSnapshot::bids)
        .def_readonly("asks", &tme::BookSnapshot::asks);

    py::class_<tme::OrderBook>(module, "OrderBook", "A price-time-priority limit order book for one instrument.")
        .def(py::init([](std::size_t orders, std::size_t price_levels) {
                 tme::BookCapacity capacity;
                 capacity.orders = orders;
                 capacity.price_levels = price_levels;
                 return tme::OrderBook(capacity);
             }),
             py::arg("orders") = 1'024,
             py::arg("price_levels") = 256)
        .def(
            "submit",
            [](tme::OrderBook& book, const tme::OrderRequest& request) { return own(book.submit(request)); },
            py::arg("request"))
        .def(
            "submit",
            [](tme::OrderBook& book,
               tme::OrderId order_id,
               tme::Side side,
               tme::Quantity quantity,
               tme::Price price,
               tme::OrderType type,
               tme::TimeInForce time_in_force,
               bool post_only,
               tme::Price stop_price,
               tme::Quantity display_quantity,
               tme::OwnerId owner,
               tme::SelfTradePrevention self_trade_prevention) {
                return own(book.submit(make_request(order_id,
                                                    side,
                                                    quantity,
                                                    price,
                                                    type,
                                                    time_in_force,
                                                    post_only,
                                                    stop_price,
                                                    display_quantity,
                                                    owner,
                                                    self_trade_prevention)));
            },
            py::arg("order_id"),
            py::arg("side"),
            py::arg("quantity"),
            py::arg("price") = 0,
            py::kw_only(),
            py::arg("type") = tme::OrderType::Limit,
            py::arg("time_in_force") = tme::TimeInForce::GoodTillCancel,
            py::arg("post_only") = false,
            py::arg("stop_price") = 0,
            py::arg("display_quantity") = 0,
            py::arg("owner") = 0,
            py::arg("self_trade_prevention") = tme::SelfTradePrevention::None,
            "Submits an order. Prices are integer ticks.")
        .def(
            "cancel",
            [](tme::OrderBook& book, tme::OrderId order_id) {
                const tme::CancelResult result = book.cancel(order_id);
                return CancelOutcome{result.order_id,
                                     result.cancelled,
                                     result.reject_reason,
                                     result.cancelled_quantity,
                                     std::string{result.message}};
            },
            py::arg("order_id"))
        .def(
            "replace",
            [](tme::OrderBook& book, const tme::OrderRequest& request) { return own(book.replace(request)); },
            py::arg("request"))
        .def(
            "replace",
            [](tme::OrderBook& book,
               tme::OrderId order_id,
               tme::Side side,
               tme::Quantity quantity,
               tme::Price price,
               tme::TimeInForce time_in_force,
               bool post_only,
               tme::Quantity display_quantity) {
                return own(book.replace(make_request(order_id,
                                                     side,
                                                     quantity,
                                                     price,
                                                     tme::OrderType::Limit,
                                                     time_in_force,
                                                     post_only,
                                                     0,
                                                     display_quantity,
                                                     0,
                                                     tme::SelfTradePrevention::None)));
            },
            py::arg("order_id"),
            py::arg("side"),
            py::arg("quantity"),
            py::arg("price"),
            py::kw_only(),
            py::arg("time_in_force") = tme::TimeInForce::GoodTillCancel,
            py::arg("post_only") = false,
            py::arg("display_quantity") = 0,
            "Replaces a resting order. A smaller quantity at the same price keeps its place in the queue.")
        .def("expire_day_orders", &tme::OrderBook::expire_day_orders)
        .def("begin_auction", &tme::OrderBook::begin_auction)
        .def("uncross",
             [](tme::OrderBook& book) {
                 const tme::AuctionResult result = book.uncross();
                 return AuctionOutcome{result.executed,
                                       result.price,
                                       result.matched_quantity,
                                       {result.trades.begin(), result.trades.end()},
                                       {result.expired_market_orders.begin(), result.expired_market_orders.end()},
                                       {result.triggered_stops.begin(), result.triggered_stops.end()}};
             })
        .def("indicative_auction", &tme::OrderBook::indicative_auction)
        .def("get_order", &tme::OrderBook::get_order, py::arg("order_id"))
        .def("orders_at", &tme::OrderBook::get_orders_at_level, py::arg("side"), py::arg("price"))
        .def("snapshot", &tme::OrderBook::snapshot)
        .def("depth",
             &depth_of,
             py::arg("levels") = 10,
             "The best levels of each side as (bids, asks), each a list of (price, quantity, order count).")
        .def("state_hash", [](const tme::OrderBook& book) { return tme::state_hash(book); })
        .def_property_readonly("best_bid", &tme::OrderBook::best_bid)
        .def_property_readonly("best_ask", &tme::OrderBook::best_ask)
        .def_property_readonly("last_trade_price", &tme::OrderBook::last_trade_price)
        .def_property_readonly("phase", &tme::OrderBook::phase)
        .def_property_readonly("order_count", &tme::OrderBook::order_count)
        .def_property_readonly("stop_order_count", &tme::OrderBook::stop_order_count);

    py::class_<Simulation>(module,
                           "MarketSimulator",
                           "An agent-based market of market makers, noise traders, momentum traders, execution "
                           "traders that slice large orders into small ones, and a whale, trading through opening "
                           "auctions, continuous sessions, and closing auctions. It records every trade and, after "
                           "every step, each symbol's top of book.")
        .def(py::init(&make_simulation),
             py::kw_only(),
             py::arg("seed") = 7,
             py::arg("symbols") = std::vector<std::string>{"ACME"},
             py::arg("initial_price") = 10'000,
             py::arg("step_seconds") = 0.01,
             py::arg("opening_auction_seconds") = 4.0,
             py::arg("continuous_seconds") = 90.0,
             py::arg("closing_auction_seconds") = 4.0,
             py::arg("closed_seconds") = 2.0,
             py::arg("market_makers") = 4,
             py::arg("noise_traders") = 16,
             py::arg("momentum_traders") = 3,
             py::arg("execution_traders") = 2,
             py::arg("volatility") = 0.35,
             py::arg("news_probability") = 4e-4,
             py::arg("record_quotes") = true)
        .def(
            "step",
            [](Simulation& simulation, std::size_t steps) { simulation.run_released([&] { simulation.step(steps); }); },
            py::arg("steps") = 1,
            "Advances the market by a number of steps, 10 ms each by default.")
        .def(
            "run",
            [](Simulation& simulation, double seconds) {
                const tme::Timestamp span = nanoseconds_of(seconds);
                simulation.run_released([&] { simulation.run(span); });
            },
            py::arg("seconds"),
            "Advances the market by a span of simulated time.")
        .def(
            "run_day",
            [](Simulation& simulation) { simulation.run_released([&] { simulation.run_day(); }); },
            "Runs to the end of the trading day, past the closing auction and the expiry of day orders.")
        .def("take_trades",
             &Simulation::take_trades,
             "The trades since the last call, as (time, symbol, price, quantity, aggressor, buy order ID, sell order "
             "ID), where the aggressor is 'B', 'S', or 'X' for an auction.")
        .def("take_quotes",
             &Simulation::take_quotes,
             "Each symbol's top of book after each step since the last call, as (time, symbol, phase, bid, bid size, "
             "ask, ask size, fair value).")
        .def(
            "top_of_book",
            [](const Simulation& simulation, const std::string& symbol) {
                const tme::TopOfBook top = simulation.simulator().feed().top_of_book(simulation.id_of(symbol));
                return std::tuple{top.bid_price, top.bid_quantity, top.ask_price, top.ask_quantity};
            },
            py::arg("symbol"))
        .def(
            "depth",
            [](const Simulation& simulation, const std::string& symbol, std::size_t levels) {
                const tme::DepthBook& depth = simulation.simulator().feed().depth(simulation.id_of(symbol));
                return std::pair{levels_of(depth.levels(tme::Side::Buy, levels)),
                                 levels_of(depth.levels(tme::Side::Sell, levels))};
            },
            py::arg("symbol"),
            py::arg("levels") = 10)
        .def(
            "candles",
            [](const Simulation& simulation, const std::string& symbol) {
                std::vector<std::tuple<tme::Timestamp, tme::Price, tme::Price, tme::Price, tme::Price, tme::Volume>>
                    rows;
                for (const tme::Candle& candle :
                     simulation.simulator().feed().candles(simulation.id_of(symbol)).candles()) {
                    rows.emplace_back(candle.start, candle.open, candle.high, candle.low, candle.close, candle.volume);
                }
                return rows;
            },
            py::arg("symbol"),
            "One-second candles as (start, open, high, low, close, volume).")
        .def(
            "fair_value",
            [](const Simulation& simulation, const std::string& symbol) {
                return simulation.simulator().fair_value(simulation.index_of(symbol));
            },
            py::arg("symbol"))
        .def("state_hash",
             [](const Simulation& simulation) { return tme::state_hash(simulation.simulator().engine()); })
        .def_property_readonly("now", [](const Simulation& simulation) { return simulation.simulator().now(); })
        .def_property_readonly("day", [](const Simulation& simulation) { return simulation.simulator().day(); })
        .def_property_readonly(
            "session",
            [](const Simulation& simulation) { return std::string{tme::to_string(simulation.simulator().session())}; })
        .def_property_readonly("symbols",
                               [](const Simulation& simulation) {
                                   std::vector<std::string> names;
                                   const tme::MarketSimulator& simulator = simulation.simulator();
                                   names.reserve(simulator.symbol_count());
                                   for (std::size_t index = 0; index < simulator.symbol_count(); ++index) {
                                       names.emplace_back(simulator.engine().symbol_name(simulator.symbol(index)));
                                   }
                                   return names;
                               })
        .def_property_readonly("stats", [](const Simulation& simulation) {
            const tme::SimulationStats& stats = simulation.simulator().stats();
            py::dict result;
            result["requests"] = stats.requests;
            result["rejects"] = stats.rejects;
            result["trades"] = stats.trades;
            result["auctions"] = stats.auctions;
            result["stops_triggered"] = stats.stops_triggered;
            result["self_trade_cancels"] = stats.self_trade_cancels;
            return result;
        });

    py::class_<ItchReplay>(module,
                           "ItchReplica",
                           "Every stock's order book, rebuilt from a NASDAQ TotalView-ITCH 5.0 file one stretch of the "
                           "day at a time, with the trades of the watched stocks. Prices are in ITCH units of 1/10,000 "
                           "of a dollar, and times in nanoseconds since midnight.")
        .def(py::init<const std::string&, const std::vector<std::string>&>(),
             py::arg("path"),
             py::arg("watch") = std::vector<std::string>{})
        .def(
            "advance",
            [](ItchReplay& replay, std::optional<std::uint64_t> until) {
                return replay.run_released([&] { return replay.advance(until); });
            },
            py::arg("until") = std::optional<std::uint64_t>{},
            "Applies the file's messages up to the first one stamped after `until`, or to the end of the file. "
            "Returns how many it applied.")
        .def_property_readonly("finished", &ItchReplay::finished, "Whether every message in the file has been applied.")
        .def_property_readonly("stats",
                               [](const ItchReplay& replay) {
                                   const tme::itch::ReplicaStats& stats = replay.replica().stats();
                                   py::dict by_type;
                                   for (std::size_t type = 0; type < stats.messages.size(); ++type) {
                                       if (stats.messages.at(type) > 0U) {
                                           by_type[py::str(std::string(1, static_cast<char>(type)))] =
                                               stats.messages.at(type);
                                       }
                                   }
                                   py::dict result;
                                   result["messages"] = stats.total_messages;
                                   result["invalid_messages"] = stats.invalid_messages;
                                   result["anomalies"] = stats.anomalies;
                                   result["live_orders"] = stats.live_orders;
                                   result["peak_orders"] = stats.peak_orders;
                                   result["last_timestamp"] = stats.last_timestamp;
                                   result["by_type"] = by_type;
                                   return result;
                               })
        .def_property_readonly("symbols",
                               [](const ItchReplay& replay) {
                                   std::vector<std::string> names;
                                   const tme::MatchingEngine& engine = replay.replica().engine();
                                   names.reserve(engine.symbol_count());
                                   for (tme::SymbolId id = 0; id < engine.symbol_count(); ++id) {
                                       names.emplace_back(engine.symbol_name(id));
                                   }
                                   return names;
                               })
        .def("take_trades",
             &ItchReplay::take_trades,
             "The watched stocks' trades since the last call, as (time, symbol, price, shares, aggressor), where the "
             "aggressor is 'B' or 'S', 'X' for a cross, or '?' for a trade with a non-displayed order.")
        .def(
            "top_of_book",
            [](const ItchReplay& replay, const std::string& symbol) { return top_of(replay.book(symbol)); },
            py::arg("symbol"),
            "(bid, bid size, ask, ask size), with None for an empty side.")
        .def(
            "depth",
            [](const ItchReplay& replay, const std::string& symbol, std::size_t levels) {
                return depth_of(replay.book(symbol), levels);
            },
            py::arg("symbol"),
            py::arg("levels") = 10)
        .def(
            "stock_stats",
            [](const ItchReplay& replay, const std::string& symbol) {
                const tme::itch::StockStats& stats = replay.stock_stats(symbol);
                py::dict result;
                result["volume"] = stats.volume;
                result["value"] = stats.value;
                result["executions"] = stats.executions;
                result["last_price"] = stats.last_price;
                return result;
            },
            py::arg("symbol"));
}
