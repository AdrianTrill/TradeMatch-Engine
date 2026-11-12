// The WebAssembly side of the browser playground: the market simulator and the command line, driven from JavaScript.
// Views cross into JavaScript as JSON, which keeps the binding small.
#include "tradematch/book_listener.hpp"
#include "tradematch/cli.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/simulator.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using tradematch::Side;

void price_or_null(std::ostringstream& json, std::optional<tradematch::Price> price) {
    if (price) {
        json << *price;
    } else {
        json << "null";
    }
}

void levels(std::ostringstream& json, const std::vector<tradematch::DepthLevel>& depth) {
    json << '[';
    for (std::size_t index = 0; index < depth.size(); ++index) {
        json << (index == 0 ? "" : ",") << '[' << depth[index].price << ',' << depth[index].quantity << ','
             << depth[index].order_count << ']';
    }
    json << ']';
}

constexpr tradematch::Timestamp kSecond = 1'000'000'000;
constexpr std::size_t kMaxCandles = 20'000;  // one-second candles sent at most, the newest
constexpr std::size_t kSparkline = 60;       // closing prices of the last minute's candles, for the market watch
constexpr std::size_t kTape = 120;           // prints kept on each symbol's tape
constexpr std::size_t kTapeSent = 60;        // the newest of them sent with each snapshot

// A whole number from JavaScript, or 0, which the engine rejects, for anything that isn't one in range.
template <typename Integer>
Integer whole(const emscripten::val& value) {
    if (!value.isNumber()) {
        return 0;
    }
    const double number = value.as<double>();
    if (!(number >= 0.0 && number <= 1e12) || number != std::floor(number)) {
        return 0;
    }
    return static_cast<Integer>(std::min(number, static_cast<double>(std::numeric_limits<Integer>::max())));
}

tradematch::OrderType order_type(const std::string& name) {
    for (const auto type :
         {tradematch::OrderType::Market, tradematch::OrderType::Stop, tradematch::OrderType::StopLimit}) {
        if (name == tradematch::to_string(type)) {
            return type;
        }
    }
    return tradematch::OrderType::Limit;
}

tradematch::TimeInForce time_in_force(const std::string& name) {
    for (const auto tif : {tradematch::TimeInForce::Day,
                           tradematch::TimeInForce::ImmediateOrCancel,
                           tradematch::TimeInForce::FillOrKill}) {
        if (name == tradematch::to_string(tif)) {
            return tif;
        }
    }
    return tradematch::TimeInForce::GoodTillCancel;
}

void number_or_null(std::ostringstream& json, std::optional<double> value) {
    if (value) {
        json << *value;
    } else {
        json << "null";
    }
}

// The agent-based market of the terminal UI: three stocks trading through opening auctions, continuous trading, and
// closing auctions, day after day. It listens to the exchange's order-level events to keep each stock's tape, where an
// auction's uncross is one print, the day's high, low, volume, and VWAP, and every auction cross, and samples each
// stock's fair value and VWAP once a simulated second.
class LiveMarket final : public tradematch::OrderFeedListener {
public:
    explicit LiveMarket(unsigned seed) {
        config_.seed = seed;
        config_.symbols = {"ACME", "GLOBEX", "INITECH"};
        config_.measure_latency = false;
        simulator_ = std::make_unique<tradematch::MarketSimulator>(config_);
        instruments_.resize(simulator_->symbol_count());
        for (std::size_t index = 0; index < instruments_.size(); ++index) {
            const tradematch::SymbolId id = simulator_->symbol(index);
            if (index_by_symbol_.size() <= id) {
                index_by_symbol_.resize(std::size_t{id} + 1);
            }
            index_by_symbol_[id] = index;
        }
        day_ = simulator_->day();
        simulator_->set_order_listener(this);
    }
    LiveMarket(const LiveMarket&) = delete;
    LiveMarket& operator=(const LiveMarket&) = delete;
    LiveMarket(LiveMarket&&) = delete;
    LiveMarket& operator=(LiveMarket&&) = delete;
    ~LiveMarket() override {
        simulator_->set_order_listener(nullptr);
    }

    // Advances simulated time by `steps` steps of 10 ms each.
    void step(int steps) {
        for (int count = 0; count < steps; ++count) {
            simulator_->step();
            if (simulator_->day() != day_) {
                day_ = simulator_->day();
                for (Instrument& instrument : instruments_) {
                    instrument.day_value = 0.0;
                    instrument.day_volume = 0;
                    instrument.day_high.reset();
                    instrument.day_low.reset();
                }
            }
            // Each second that ends is sampled once: samples[s] holds the values at the end of second s.
            const tradematch::Timestamp second = simulator_->now() / kSecond;
            for (; seconds_sampled_ < second; ++seconds_sampled_) {
                for (std::size_t index = 0; index < instruments_.size(); ++index) {
                    instruments_[index].samples.push_back(Sample{simulator_->fair_value(index), vwap(index)});
                }
            }
        }
    }

    void on_order_events(tradematch::SymbolId symbol,
                         std::span<const tradematch::OrderEvent> /*events*/,
                         std::span<const tradematch::Trade> trades) override {
        Instrument& instrument = instruments_.at(index_by_symbol_.at(symbol));
        const tradematch::Timestamp now = simulator_->now();
        for (const tradematch::Trade& trade : trades) {
            instrument.day_value += static_cast<double>(trade.price) * static_cast<double>(trade.quantity);
            instrument.day_volume += trade.quantity;
            instrument.day_high = std::max(instrument.day_high.value_or(trade.price), trade.price);
            instrument.day_low = std::min(instrument.day_low.value_or(trade.price), trade.price);
            if (trade.auction) {
                if (instrument.crosses.empty() || instrument.crosses.back().time != now) {
                    instrument.crosses.push_back(Cross{now, trade.price, 0});
                }
                instrument.crosses.back().volume += trade.quantity;
            }
            // Both sides can be the visitor's in an auction, where orders trade at one price however they arrived.
            const bool bought = fill(trade, Side::Buy, symbol, now);
            const bool sold = fill(trade, Side::Sell, symbol, now);
            print(instrument, trade, now, bought ? 'B' : (sold ? 'S' : 0));
        }
    }

    // Sends the visitor's order, given as a JavaScript object {side: 'BUY' or 'SELL', type: 'LIMIT', 'MARKET', 'STOP',
    // or 'STOP_LIMIT', quantity, price, stop, tif: 'GTC', 'DAY', 'IOC', or 'FOK', postOnly, display} with prices in
    // ticks, and reports the outcome as JSON. Self-trade prevention cancels the visitor's older order.
    std::string submit(int symbol_index, const emscripten::val& request) {
        const std::size_t index = index_of(symbol_index);
        tradematch::OrderRequest order;
        order.side = request["side"].isString() && request["side"].as<std::string>() == "SELL" ? Side::Sell : Side::Buy;
        order.type = order_type(request["type"].isString() ? request["type"].as<std::string>() : "");
        order.quantity = whole<tradematch::Quantity>(request["quantity"]);
        order.price = whole<tradematch::Price>(request["price"]);
        order.stop_price = whole<tradematch::Price>(request["stop"]);
        order.time_in_force = time_in_force(request["tif"].isString() ? request["tif"].as<std::string>() : "");
        order.post_only = request["postOnly"].isTrue();
        order.display_quantity = whole<tradematch::Quantity>(request["display"]);
        order.owner = simulator_->participant_owner();
        order.self_trade_prevention = tradematch::SelfTradePrevention::CancelOldest;

        // Registered before it is sent, so that the events of its own request already count its fills.
        const tradematch::OrderId id = simulator_->next_order_id();
        orders_[id] = VisitorOrder{id,
                                   index,
                                   order.side,
                                   order.type,
                                   order.time_in_force,
                                   order.quantity,
                                   order.price,
                                   order.stop_price,
                                   order.display_quantity,
                                   simulator_->now(),
                                   0};
        const tradematch::SubmitResult result = simulator_->submit(index, order);
        for (const tradematch::OrderId cancelled : result.self_trade_cancels) {
            finish(cancelled, "self-trade prevention");
        }
        if (!result.accepted) {
            orders_.erase(id);
        }
        std::ostringstream json;
        json << "{\"accepted\":" << (result.accepted ? "true" : "false") << ",\"id\":" << id << ",\"reject\":\""
             << tradematch::to_string(result.reject_reason) << "\",\"message\":\"" << result.message
             << "\",\"filled\":" << result.filled_quantity << ",\"remaining\":" << result.remaining_quantity
             << ",\"rested\":" << (result.rested ? "true" : "false")
             << ",\"pending\":" << (result.pending_trigger ? "true" : "false")
             << ",\"queued\":" << (result.queued_for_auction ? "true" : "false") << '}';
        return json.str();
    }

    // Cancels one of the visitor's orders; any other ID is refused.
    std::string cancel(double order_id) {
        const auto found = orders_.find(order_id >= 0.0 ? static_cast<tradematch::OrderId>(order_id) : 0);
        if (found == orders_.end()) {
            return R"({"cancelled":false,"message":"no working order of yours has this ID"})";
        }
        const tradematch::CancelResult result = simulator_->cancel(found->second.symbol, found->first);
        if (result.cancelled) {
            finish(result.order_id, "cancelled");
        }
        std::ostringstream json;
        json << "{\"cancelled\":" << (result.cancelled ? "true" : "false")
             << ",\"quantity\":" << result.cancelled_quantity << ",\"message\":\"" << result.message << "\"}";
        return json.str();
    }

    // Cancels every working order of the visitor's, and returns how many.
    int cancel_all() {
        std::vector<std::pair<std::size_t, tradematch::OrderId>> working;
        working.reserve(orders_.size());
        for (const auto& [id, order] : orders_) {
            working.emplace_back(order.symbol, id);
        }
        int cancelled = 0;
        for (const auto& [symbol, id] : working) {
            if (simulator_->cancel(symbol, id).cancelled) {
                finish(id, "cancelled");
                ++cancelled;
            }
        }
        return cancelled;
    }

    // Moves a resting limit order of the visitor's to a new price and quantity: a smaller quantity at the same price
    // keeps its place in the queue.
    std::string amend(double order_id, double price, double quantity) {
        const auto found = orders_.find(order_id >= 0.0 ? static_cast<tradematch::OrderId>(order_id) : 0);
        if (found == orders_.end() || found->second.type != tradematch::OrderType::Limit) {
            return R"({"replaced":false,"message":"no resting limit order of yours has this ID"})";
        }
        VisitorOrder& order = found->second;
        tradematch::OrderRequest replacement{order.id,
                                             order.side,
                                             tradematch::OrderType::Limit,
                                             whole<tradematch::Quantity>(emscripten::val(quantity)),
                                             whole<tradematch::Price>(emscripten::val(price)),
                                             order.time_in_force};
        replacement.display_quantity = order.display;
        replacement.owner = simulator_->participant_owner();
        replacement.self_trade_prevention = tradematch::SelfTradePrevention::CancelOldest;
        // The replacement's quantity is what stays open, on top of what has filled so far; its own request may fill
        // more, which the order events count.
        const tradematch::Quantity filled_before = order.filled;
        const tradematch::ReplaceResult result = simulator_->replace(order.symbol, replacement);
        if (result.replaced) {
            order.price = replacement.price;
            order.quantity = filled_before + replacement.quantity;
        }
        std::ostringstream json;
        json << "{\"replaced\":" << (result.replaced ? "true" : "false")
             << ",\"kept_priority\":" << (result.priority_kept ? "true" : "false") << ",\"message\":\""
             << result.message << "\"}";
        return json.str();
    }

    // The visitor's working orders, and the fills and finished orders since the last call, as JSON.
    std::string account() {
        std::vector<std::pair<tradematch::OrderId, const char*>> gone;
        std::ostringstream json;
        json << "{\"owner\":" << simulator_->participant_owner() << ",\"orders\":[";
        bool first = true;
        for (const auto& [id, order] : orders_) {
            const tradematch::OrderBook& book = simulator_->engine().book(simulator_->symbol(order.symbol));
            const auto resting = book.get_order(id);
            const char* status = resting                             ? "resting"
                                 : book.has_stop_order(id)           ? "stop"
                                 : book.has_auction_market_order(id) ? "queued"
                                                                     : nullptr;
            if (status == nullptr) {
                gone.emplace_back(id, order.filled >= order.quantity ? "filled" : "expired");
                continue;
            }
            const tradematch::Quantity remaining =
                resting ? resting->remaining_quantity : order.quantity - std::min(order.filled, order.quantity);
            json << (first ? "" : ",") << '[' << id << ',' << order.symbol << ",\"" << tradematch::to_string(order.side)
                 << "\",\"" << tradematch::to_string(order.type) << "\",\""
                 << tradematch::to_string(order.time_in_force) << "\"," << order.quantity << ',' << order.price << ','
                 << order.stop << ',' << order.display << ',' << order.filled << ',' << remaining << ",\"" << status
                 << "\"," << order.time << ']';
            first = false;
        }
        for (const auto& [id, reason] : gone) {
            finish(id, reason);
        }
        json << "],\"fills\":[";
        for (std::size_t position = 0; position < fills_.size(); ++position) {
            const VisitorFill& fill = fills_[position];
            json << (position == 0 ? "" : ",") << '[' << fill.id << ',' << fill.time << ',' << fill.symbol << ','
                 << fill.order << ",\"" << tradematch::to_string(fill.side) << "\"," << fill.price << ','
                 << fill.quantity << ",\"" << fill.liquidity << "\"]";
        }
        json << "],\"done\":[";
        for (std::size_t position = 0; position < done_.size(); ++position) {
            const Finished& order = done_[position];
            json << (position == 0 ? "" : ",") << '[' << order.id << ',' << order.symbol << ",\""
                 << tradematch::to_string(order.side) << "\"," << order.quantity << ',' << order.filled << ",\""
                 << order.reason << "\"]";
        }
        json << "]}";
        fills_.clear();
        done_.clear();
        return json.str();
    }

    // The shape of the simulated day, as JSON.
    std::string config() const {
        std::ostringstream json;
        json << "{\"step\":" << config_.step << ",\"phases\":[[\"OPENING AUCTION\"," << config_.opening_auction
             << "],[\"CONTINUOUS\"," << config_.continuous << "],[\"CLOSING AUCTION\"," << config_.closing_auction
             << "],[\"CLOSED\"," << config_.closed << "]]}";
        return json.str();
    }

    // One symbol's view as JSON: the session, every symbol's summary, the top `depth` levels of each side, the
    // indicative uncross during an auction, recent trades, the auction crosses, and its one-second candles from the one
    // starting at `since` on: all of them if `since` is negative, and none if it is later than now.
    std::string snapshot(int symbol_index, int depth, double since) const {
        const std::size_t index =
            std::min(static_cast<std::size_t>(std::max(symbol_index, 0)), simulator_->symbol_count() - 1);
        const tradematch::SymbolId symbol = simulator_->symbol(index);
        const tradematch::MarketDataFeed& feed = simulator_->feed();
        const Instrument& instrument = instruments_[index];
        const auto rows = static_cast<std::size_t>(std::max(depth, 1));
        const tradematch::SimulationStats& stats = simulator_->stats();

        std::ostringstream json;
        json << "{\"time\":" << simulator_->now() << ",\"day\":" << simulator_->day() << ",\"phase\":\""
             << tradematch::to_string(simulator_->session())
             << "\",\"phase_left\":" << simulator_->phase_time_remaining() << ",\"symbol\":\""
             << simulator_->engine().symbol_name(symbol) << "\",\"symbols\":[";
        for (std::size_t other = 0; other < simulator_->symbol_count(); ++other) {
            json << (other == 0 ? "" : ",") << '"' << simulator_->engine().symbol_name(simulator_->symbol(other))
                 << '"';
        }
        json << "],\"watch\":[";
        for (std::size_t other = 0; other < simulator_->symbol_count(); ++other) {
            watch(json, other);
            json << (other + 1 < simulator_->symbol_count() ? "," : "");
        }
        json << "],\"auction\":";
        if (const auto indicative = simulator_->session() == tradematch::SessionPhase::OpeningAuction ||
                                            simulator_->session() == tradematch::SessionPhase::ClosingAuction
                                        ? simulator_->engine().book(symbol).indicative_auction()
                                        : std::nullopt) {
            json << "{\"price\":" << indicative->price << ",\"matched\":" << indicative->matched_quantity
                 << ",\"buy_surplus\":" << indicative->buy_surplus << ",\"sell_surplus\":" << indicative->sell_surplus
                 << '}';
        } else {
            json << "null";
        }
        json << ",\"last\":";
        price_or_null(json, feed.last_price(symbol));
        json << ",\"fair\":" << simulator_->fair_value(index) << ",\"vwap\":";
        number_or_null(json, vwap(index));
        json << ",\"volume\":" << feed.traded_volume(symbol)
             << ",\"orders\":" << simulator_->engine().book(symbol).order_count() << ",\"bids\":";
        levels(json, feed.depth(symbol).levels(tradematch::Side::Buy, rows));
        json << ",\"asks\":";
        levels(json, feed.depth(symbol).levels(tradematch::Side::Sell, rows));

        // The newest prints last, as [id, time, price, quantity, aggressor, fills, mine]: the aggressor is 'B' or 'S'
        // for the side that took liquidity, or 'X' for an auction's uncross, one print however many orders it filled;
        // mine is 'B' or 'S' when the visitor bought or sold in it.
        const auto& tape = instrument.tape;
        json << ",\"trades\":[";
        const std::size_t first_print = tape.size() > kTapeSent ? tape.size() - kTapeSent : 0;
        for (std::size_t position = first_print; position < tape.size(); ++position) {
            const Print& entry = tape[position];
            json << (position == first_print ? "" : ",") << '[' << entry.id << ',' << entry.time << ',' << entry.price
                 << ',' << entry.quantity << ",\"" << entry.aggressor << "\"," << entry.fills << ",\""
                 << (entry.mine == 0 ? "" : std::string(1, entry.mine)) << "\"]";
        }

        // Every auction cross, as [time, price, volume].
        json << "],\"crosses\":[";
        for (std::size_t position = 0; position < instrument.crosses.size(); ++position) {
            const Cross& cross = instrument.crosses[position];
            json << (position == 0 ? "" : ",") << '[' << cross.time << ',' << cross.price << ',' << cross.volume << ']';
        }

        // One-second candles as [start, open, high, low, close, volume, fair value, day VWAP], the fair value and
        // VWAP as of the candle's end.
        const auto& candles = feed.candles(symbol).candles();
        auto first =
            candles.size() > kMaxCandles ? candles.end() - static_cast<std::ptrdiff_t>(kMaxCandles) : candles.begin();
        if (since > static_cast<double>(simulator_->now())) {
            first = candles.end();
        } else if (since >= 0.0) {
            first = std::max(first,
                             std::lower_bound(candles.begin(),
                                              candles.end(),
                                              static_cast<tradematch::Timestamp>(since),
                                              [](const tradematch::Candle& candle, tradematch::Timestamp start) {
                                                  return candle.start < start;
                                              }));
        }
        json << "],\"candles\":[";
        for (auto candle = first; candle != candles.end(); ++candle) {
            const auto second = static_cast<std::size_t>(candle->start / kSecond);
            const bool sampled = second < instrument.samples.size();
            json << (candle == first ? "" : ",") << '[' << candle->start << ',' << candle->open << ',' << candle->high
                 << ',' << candle->low << ',' << candle->close << ',' << candle->volume << ','
                 << (sampled ? instrument.samples[second].fair : simulator_->fair_value(index)) << ',';
            number_or_null(json, sampled ? instrument.samples[second].vwap : vwap(index));
            json << ']';
        }
        json << "],\"requests\":" << stats.requests << ",\"trade_count\":" << stats.trades
             << ",\"auctions\":" << stats.auctions << ",\"stops\":" << stats.stops_triggered << '}';
        return json.str();
    }

private:
    // A symbol's summary for the market watch: its name, last price, day open, previous close, the day's high, low,
    // and volume, and the closes of its last minute of candles.
    void watch(std::ostringstream& json, std::size_t index) const {
        const tradematch::SymbolId symbol = simulator_->symbol(index);
        const Instrument& instrument = instruments_[index];
        json << "{\"symbol\":\"" << simulator_->engine().symbol_name(symbol) << "\",\"last\":";
        price_or_null(json, simulator_->feed().last_price(symbol));
        json << ",\"open\":";
        price_or_null(json, simulator_->day_open(index));
        json << ",\"previous_close\":";
        price_or_null(json, simulator_->previous_close(index));
        json << ",\"high\":";
        price_or_null(json, instrument.day_high);
        json << ",\"low\":";
        price_or_null(json, instrument.day_low);
        json << ",\"volume\":" << instrument.day_volume << ",\"spark\":[";
        const auto& candles = simulator_->feed().candles(symbol).candles();
        const std::size_t first = candles.size() > kSparkline ? candles.size() - kSparkline : 0;
        for (std::size_t position = first; position < candles.size(); ++position) {
            json << (position == first ? "" : ",") << candles[position].close;
        }
        json << "]}";
    }

    struct Sample {
        tradematch::Price fair{};
        std::optional<double> vwap;
    };
    struct Cross {
        tradematch::Timestamp time{};
        tradematch::Price price{};
        tradematch::Volume volume{};
    };
    struct Print {
        std::uint64_t id{};
        tradematch::Timestamp time{};
        tradematch::Price price{};
        tradematch::Volume quantity{};
        char aggressor{};
        std::uint32_t fills{};
        char mine{};  // 'B' or 'S' if the visitor bought or sold in the print
    };
    struct VisitorOrder {
        tradematch::OrderId id{};
        std::size_t symbol{};
        Side side{Side::Buy};
        tradematch::OrderType type{tradematch::OrderType::Limit};
        tradematch::TimeInForce time_in_force{tradematch::TimeInForce::GoodTillCancel};
        tradematch::Quantity quantity{};
        tradematch::Price price{};
        tradematch::Price stop{};
        tradematch::Quantity display{};
        tradematch::Timestamp time{};
        tradematch::Quantity filled{};
    };
    struct VisitorFill {
        std::uint64_t id{};
        tradematch::Timestamp time{};
        std::size_t symbol{};
        tradematch::OrderId order{};
        Side side{Side::Buy};
        tradematch::Price price{};
        tradematch::Quantity quantity{};
        char liquidity{};  // 'M' made liquidity, 'T' took it, 'A' traded in an auction's uncross
    };
    struct Finished {
        tradematch::OrderId id{};
        std::size_t symbol{};
        Side side{Side::Buy};
        tradematch::Quantity quantity{};
        tradematch::Quantity filled{};
        const char* reason{};
    };
    struct Instrument {
        double day_value{0.0};
        tradematch::Volume day_volume{0};
        std::optional<tradematch::Price> day_high;
        std::optional<tradematch::Price> day_low;
        std::vector<Sample> samples;  // by second
        std::vector<Cross> crosses;
        std::deque<Print> tape;
    };

    // Adds a trade to its symbol's tape. The fills of one uncross, all at one time and price, become one print.
    void print(Instrument& instrument, const tradematch::Trade& trade, tradematch::Timestamp now, char mine) {
        if (trade.auction && !instrument.tape.empty() && instrument.tape.back().aggressor == 'X' &&
            instrument.tape.back().time == now && instrument.tape.back().price == trade.price) {
            instrument.tape.back().quantity += trade.quantity;
            ++instrument.tape.back().fills;
            instrument.tape.back().mine = mine != 0 ? mine : instrument.tape.back().mine;
            return;
        }
        const char aggressor = trade.auction ? 'X' : (trade.aggressor_side == Side::Buy ? 'B' : 'S');
        instrument.tape.push_back(Print{++prints_, now, trade.price, trade.quantity, aggressor, 1, mine});
        if (instrument.tape.size() > kTape) {
            instrument.tape.pop_front();
        }
    }

    // The day's volume-weighted average price so far, in ticks to two decimals, or nothing before its first trade.
    std::optional<double> vwap(std::size_t index) const {
        const Instrument& instrument = instruments_[index];
        if (instrument.day_volume == 0) {
            return std::nullopt;
        }
        return std::round(instrument.day_value / static_cast<double>(instrument.day_volume) * 100.0) / 100.0;
    }

    // Records a trade's fill of one of the visitor's orders on `side`; returns whether there was one.
    bool fill(const tradematch::Trade& trade, Side side, tradematch::SymbolId symbol, tradematch::Timestamp now) {
        const auto found = orders_.find(side == Side::Buy ? trade.buy_order_id : trade.sell_order_id);
        if (found == orders_.end()) {
            return false;
        }
        found->second.filled += trade.quantity;
        const char liquidity = trade.auction ? 'A' : (trade.aggressor_side == side ? 'T' : 'M');
        fills_.push_back(VisitorFill{++fill_count_,
                                     now,
                                     index_by_symbol_.at(symbol),
                                     found->first,
                                     side,
                                     trade.price,
                                     trade.quantity,
                                     liquidity});
        return true;
    }

    // Moves one of the visitor's orders from the working ones to the finished ones.
    void finish(tradematch::OrderId id, const char* reason) {
        const auto found = orders_.find(id);
        if (found != orders_.end()) {
            const VisitorOrder& order = found->second;
            done_.push_back(Finished{id, order.symbol, order.side, order.quantity, order.filled, reason});
            orders_.erase(found);
        }
    }

    std::size_t index_of(int symbol_index) const {
        return std::min(static_cast<std::size_t>(std::max(symbol_index, 0)), simulator_->symbol_count() - 1);
    }

    tradematch::SimulationConfig config_;
    std::unique_ptr<tradematch::MarketSimulator> simulator_;
    std::vector<Instrument> instruments_;
    std::vector<std::size_t> index_by_symbol_;
    int day_{0};
    tradematch::Timestamp seconds_sampled_{0};
    std::uint64_t prints_{0};
    std::unordered_map<tradematch::OrderId, VisitorOrder> orders_;  // the visitor's working orders
    std::vector<VisitorFill> fills_;                                // their fills since the last account()
    std::vector<Finished> done_;                                    // orders finished since then
    std::uint64_t fill_count_{0};
};

// The interactive command line: one interpreter that keeps its state from command to command.
class Terminal {
public:
    Terminal() : interpreter_(std::make_unique<tradematch::CommandInterpreter>(output_)) {}

    // Runs one command and returns what it printed. After quit, the next command starts a fresh session.
    std::string run(const std::string& line) {
        output_.str({});
        output_.clear();
        if (!interpreter_->execute(line)) {
            interpreter_ = std::make_unique<tradematch::CommandInterpreter>(output_);
            output_ << "(a new session starts)\n";
        }
        return output_.str();
    }

private:
    std::ostringstream output_;
    std::unique_ptr<tradematch::CommandInterpreter> interpreter_;
};

std::string run_script(const std::string& script) {
    return tradematch::run_script(script);
}

}  // namespace

EMSCRIPTEN_BINDINGS(tradematch) {
    emscripten::class_<LiveMarket>("LiveMarket")
        .constructor<unsigned>()
        .function("step", &LiveMarket::step)
        .function("config", &LiveMarket::config)
        .function("snapshot", &LiveMarket::snapshot)
        .function("submit", &LiveMarket::submit)
        .function("cancel", &LiveMarket::cancel)
        .function("cancelAll", &LiveMarket::cancel_all)
        .function("amend", &LiveMarket::amend)
        .function("account", &LiveMarket::account);
    emscripten::class_<Terminal>("Terminal").constructor<>().function("run", &Terminal::run);
    // The same transcript the native command line produces for a script, so the golden scenarios can be checked
    // against the WebAssembly build too.
    emscripten::function("runScript", &run_script);
}
