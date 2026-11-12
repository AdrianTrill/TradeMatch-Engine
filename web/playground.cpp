// The WebAssembly side of the browser playground: the market simulator and the command line, driven from JavaScript.
// Views cross into JavaScript as JSON, which keeps the binding small.
#include "tradematch/cli.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/simulator.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
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

// The agent-based market of the terminal UI: three stocks trading through opening auctions, continuous trading, and
// closing auctions, day after day.
class LiveMarket {
public:
    explicit LiveMarket(unsigned seed) {
        tradematch::SimulationConfig config;
        config.seed = seed;
        config.symbols = {"ACME", "GLOBEX", "INITECH"};
        config.measure_latency = false;
        simulator_ = std::make_unique<tradematch::MarketSimulator>(config);
    }

    // Advances simulated time by `steps` steps of 10 ms each.
    void step(int steps) {
        for (int count = 0; count < steps; ++count) {
            simulator_->step();
        }
    }

    // One symbol's view as JSON: the session, the top `depth` levels of each side, recent trades, and candles.
    std::string snapshot(int symbol_index, int depth) const {
        const std::size_t index =
            std::min(static_cast<std::size_t>(std::max(symbol_index, 0)), simulator_->symbol_count() - 1);
        const tradematch::SymbolId symbol = simulator_->symbol(index);
        const tradematch::MarketDataFeed& feed = simulator_->feed();
        const auto rows = static_cast<std::size_t>(std::max(depth, 1));
        const tradematch::SimulationStats& stats = simulator_->stats();

        std::ostringstream json;
        json << "{\"time\":" << simulator_->now() << ",\"day\":" << simulator_->day() << ",\"phase\":\""
             << tradematch::to_string(simulator_->session()) << "\",\"symbol\":\""
             << simulator_->engine().symbol_name(symbol) << "\",\"symbols\":[";
        for (std::size_t other = 0; other < simulator_->symbol_count(); ++other) {
            json << (other == 0 ? "" : ",") << '"' << simulator_->engine().symbol_name(simulator_->symbol(other))
                 << '"';
        }
        json << "],\"last\":";
        price_or_null(json, feed.last_price(symbol));
        json << ",\"fair\":" << simulator_->fair_value(index) << ",\"volume\":" << feed.traded_volume(symbol)
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

        // The last 90 one-second candles as [start, open, high, low, close, volume].
        const auto& candles = feed.candles(symbol).candles();
        json << "],\"candles\":[";
        const std::size_t first_candle = candles.size() > 90 ? candles.size() - 90 : 0;
        for (std::size_t position = first_candle; position < candles.size(); ++position) {
            const tradematch::Candle& candle = candles[position];
            json << (position == first_candle ? "" : ",") << '[' << candle.start << ',' << candle.open << ','
                 << candle.high << ',' << candle.low << ',' << candle.close << ',' << candle.volume << ']';
        }
        json << "],\"requests\":" << stats.requests << ",\"trade_count\":" << stats.trades
             << ",\"auctions\":" << stats.auctions << ",\"stops\":" << stats.stops_triggered << '}';
        return json.str();
    }

private:
    std::unique_ptr<tradematch::MarketSimulator> simulator_;
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
        .function("snapshot", &LiveMarket::snapshot);
    emscripten::class_<Terminal>("Terminal").constructor<>().function("run", &Terminal::run);
    // The same transcript the native command line produces for a script, so the golden scenarios can be checked
    // against the WebAssembly build too.
    emscripten::function("runScript", &run_script);
}
