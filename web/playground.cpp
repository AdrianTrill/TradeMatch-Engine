// The WebAssembly side of the browser playground: the market simulator and the command line, driven from JavaScript.
// Views cross into JavaScript as JSON, which keeps the binding small.
#include "tradematch/book_listener.hpp"
#include "tradematch/cli.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/simulator.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

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

void number_or_null(std::ostringstream& json, std::optional<double> value) {
    if (value) {
        json << *value;
    } else {
        json << "null";
    }
}

// The agent-based market of the terminal UI: three stocks trading through opening auctions, continuous trading, and
// closing auctions, day after day. It listens to the exchange's order-level events to keep each day's high, low,
// volume, and VWAP, and every auction cross, and samples each stock's fair value and VWAP once a simulated second.
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
        }
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

        // The newest trades last, as [time, price, quantity, aggressor]; auction trades have no aggressor.
        const auto& tape = feed.recent_trades(symbol);
        json << ",\"trades\":[";
        const std::size_t first_trade = tape.size() > 40 ? tape.size() - 40 : 0;
        for (std::size_t position = first_trade; position < tape.size(); ++position) {
            const tradematch::TapeEntry& entry = tape[position];
            const char aggressor =
                entry.trade.auction ? 'X' : (entry.trade.aggressor_side == tradematch::Side::Buy ? 'B' : 'S');
            json << (position == first_trade ? "" : ",") << '[' << entry.timestamp << ',' << entry.trade.price << ','
                 << entry.trade.quantity << ",\"" << aggressor << "\"]";
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
    struct Instrument {
        double day_value{0.0};
        tradematch::Volume day_volume{0};
        std::optional<tradematch::Price> day_high;
        std::optional<tradematch::Price> day_low;
        std::vector<Sample> samples;  // by second
        std::vector<Cross> crosses;
    };

    // The day's volume-weighted average price so far, in ticks to two decimals, or nothing before its first trade.
    std::optional<double> vwap(std::size_t index) const {
        const Instrument& instrument = instruments_[index];
        if (instrument.day_volume == 0) {
            return std::nullopt;
        }
        return std::round(instrument.day_value / static_cast<double>(instrument.day_volume) * 100.0) / 100.0;
    }

    tradematch::SimulationConfig config_;
    std::unique_ptr<tradematch::MarketSimulator> simulator_;
    std::vector<Instrument> instruments_;
    std::vector<std::size_t> index_by_symbol_;
    int day_{0};
    tradematch::Timestamp seconds_sampled_{0};
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
        .function("snapshot", &LiveMarket::snapshot);
    emscripten::class_<Terminal>("Terminal").constructor<>().function("run", &Terminal::run);
    // The same transcript the native command line produces for a script, so the golden scenarios can be checked
    // against the WebAssembly build too.
    emscripten::function("runScript", &run_script);
}
