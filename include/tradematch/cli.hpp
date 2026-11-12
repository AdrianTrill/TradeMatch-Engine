#ifndef TRADEMATCH_CLI_HPP
#define TRADEMATCH_CLI_HPP

#include "tradematch/journal.hpp"
#include "tradematch/market_data.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tradematch {

// Runs text commands such as `buy ACME 100 @ 100.50 ioc` against a matching engine and prints what happened, for the
// interactive command line and for scripted scenarios. `help` lists the commands. Prices are decimals with at most
// two places (one tick is 0.01). Malformed commands print an error and change nothing.
class CommandInterpreter {
public:
    explicit CommandInterpreter(std::ostream& out);
    CommandInterpreter(const CommandInterpreter&) = delete;
    CommandInterpreter& operator=(const CommandInterpreter&) = delete;
    CommandInterpreter(CommandInterpreter&&) = delete;
    CommandInterpreter& operator=(CommandInterpreter&&) = delete;
    ~CommandInterpreter();

    // Runs one line. Returns false once the session should end (`quit` or `exit`).
    bool execute(std::string_view line);

    const MatchingEngine& engine() const noexcept {
        return *engine_;
    }

    // Commands that could not be parsed or run.
    std::size_t errors() const noexcept {
        return errors_;
    }

private:
    using Tokens = std::vector<std::string>;

    void reset_engine();
    void dispatch(const std::string& command, Tokens& args);

    void help();
    void add(const Tokens& args);
    void use(const Tokens& args);
    void symbols();
    void order(Side side, Tokens& args, bool stop);
    void cancel(Tokens& args);
    void replace(Tokens& args);
    void book(Tokens& args);
    void show_order(Tokens& args);
    void auction(Tokens& args);
    void expire();
    void trades(Tokens& args);
    void save(const Tokens& args);
    void load(const Tokens& args);
    void journal(const Tokens& args);

    SymbolId take_symbol(Tokens& args);
    SymbolId current_symbol() const;
    void print_submit(const OrderRequest& order, const SubmitResult& result);
    void print_events(std::span<const Trade> trades,
                      std::span<const OrderId> triggered_stops,
                      std::span<const OrderId> self_trade_cancels);

    std::ostream& out_;
    std::unique_ptr<MatchingEngine> engine_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<JournalWriter> journal_;
    std::unique_ptr<JournaledEngine> journaled_;
    std::optional<SymbolId> current_;
    OrderId next_order_id_{1};
    std::size_t errors_{0};
};

}  // namespace tradematch

#endif
