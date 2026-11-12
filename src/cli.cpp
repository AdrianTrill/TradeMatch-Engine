#include "tradematch/cli.hpp"

#include "tradematch/snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace tradematch {

namespace {

// A command the interpreter could not make sense of; reported to the user, never to the engine.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr std::size_t kDefaultBookLevels = 10;
constexpr std::size_t kDefaultTrades = 10;
constexpr std::size_t kTapeLength = 100;

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string upper(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

// Splits on whitespace; `#` starts a comment.
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : line) {
        if (character == '#') {
            break;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

bool starts_with_letter(std::string_view token) {
    return !token.empty() && std::isalpha(static_cast<unsigned char>(token.front())) != 0;
}

std::string with_commas(std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), ",");
    }
    return digits;
}

std::uint64_t parse_unsigned(std::string_view token, std::string_view what) {
    const std::string text{token};
    std::uint64_t value = 0;
    const char* const last = text.c_str() + text.size();
    const auto [stop, error] = std::from_chars(text.c_str(), last, value);
    if (text.empty() || error != std::errc{} || stop != last) {
        throw UsageError("expected a whole number for the " + std::string{what} + ", got '" + std::string{token} + "'");
    }
    return value;
}

template <typename T>
T parse_bounded(std::string_view token, std::string_view what) {
    const std::uint64_t value = parse_unsigned(token, what);
    if (value > std::numeric_limits<T>::max()) {
        throw UsageError("the " + std::string{what} + " " + std::string{token} + " is too large");
    }
    return static_cast<T>(value);
}

// Decimal prices with at most two places: "100", "100.5", "100.50", "-1.25". One tick is 0.01.
Price parse_price(std::string_view token) {
    const bool negative = token.starts_with('-');
    const std::string_view digits = negative ? token.substr(1) : token;
    const auto point = digits.find('.');
    const std::string_view whole = digits.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view{} : digits.substr(point + 1);
    const auto all_digits = [](std::string_view part) {
        return std::ranges::all_of(part, [](unsigned char character) { return std::isdigit(character) != 0; });
    };
    if (whole.empty() || !all_digits(whole) || !all_digits(fraction) || fraction.size() > 2 ||
        (point != std::string_view::npos && fraction.empty())) {
        throw UsageError("expected a price such as 100.25, got '" + std::string{token} + "'");
    }
    constexpr auto kLargestWhole = (std::numeric_limits<Price>::max() - 99) / 100;
    const auto units = parse_unsigned(whole, "price");
    if (units > static_cast<std::uint64_t>(kLargestWhole)) {
        throw UsageError("the price " + std::string{token} + " is too large");
    }
    Price ticks = static_cast<Price>(units) * 100;
    if (!fraction.empty()) {
        const auto cents = static_cast<Price>(parse_unsigned(fraction, "price"));
        ticks += fraction.size() == 1 ? cents * 10 : cents;
    }
    return negative ? -ticks : ticks;
}

TimeInForce parse_time_in_force(std::string_view text) {
    const std::string value = lower(text);
    if (value == "gtc") {
        return TimeInForce::GoodTillCancel;
    }
    if (value == "day") {
        return TimeInForce::Day;
    }
    if (value == "ioc") {
        return TimeInForce::ImmediateOrCancel;
    }
    if (value == "fok") {
        return TimeInForce::FillOrKill;
    }
    throw UsageError("time in force must be gtc, day, ioc, or fok, not '" + std::string{text} + "'");
}

SelfTradePrevention parse_self_trade_prevention(std::string_view text) {
    const std::string value = lower(text);
    if (value == "none") {
        return SelfTradePrevention::None;
    }
    if (value == "newest") {
        return SelfTradePrevention::CancelNewest;
    }
    if (value == "oldest") {
        return SelfTradePrevention::CancelOldest;
    }
    if (value == "both") {
        return SelfTradePrevention::CancelBoth;
    }
    throw UsageError("stp must be none, newest, oldest, or both, not '" + std::string{text} + "'");
}

const char* short_name(TimeInForce time_in_force) noexcept {
    switch (time_in_force) {
        case TimeInForce::GoodTillCancel:
            return "GTC";
        case TimeInForce::Day:
            return "DAY";
        case TimeInForce::ImmediateOrCancel:
            return "IOC";
        case TimeInForce::FillOrKill:
            return "FOK";
    }
    return "?";
}

const char* short_name(SelfTradePrevention mode) noexcept {
    switch (mode) {
        case SelfTradePrevention::None:
            return "none";
        case SelfTradePrevention::CancelNewest:
            return "newest";
        case SelfTradePrevention::CancelOldest:
            return "oldest";
        case SelfTradePrevention::CancelBoth:
            return "both";
    }
    return "?";
}

struct OrderOptions {
    std::optional<OrderId> order_id;
    std::optional<TimeInForce> time_in_force;
    bool post_only{false};
    std::optional<Quantity> display_quantity;
    std::optional<OwnerId> owner;
    std::optional<SelfTradePrevention> self_trade_prevention;
};

OrderOptions parse_options(std::span<const std::string> tokens) {
    OrderOptions options;
    for (const std::string& token : tokens) {
        const std::string option = lower(token);
        if (option == "post-only" || option == "postonly") {
            options.post_only = true;
            continue;
        }
        if (option == "gtc" || option == "day" || option == "ioc" || option == "fok") {
            options.time_in_force = parse_time_in_force(option);
            continue;
        }
        const auto equals = option.find('=');
        if (equals == std::string::npos) {
            throw UsageError("unexpected '" + token + "'");
        }
        const std::string key = option.substr(0, equals);
        const std::string_view value = std::string_view{token}.substr(equals + 1);
        if (key == "id") {
            options.order_id = parse_unsigned(value, "order ID");
        } else if (key == "tif") {
            options.time_in_force = parse_time_in_force(value);
        } else if (key == "display") {
            options.display_quantity = parse_bounded<Quantity>(value, "display quantity");
        } else if (key == "owner") {
            options.owner = parse_bounded<OwnerId>(value, "owner");
        } else if (key == "stp") {
            options.self_trade_prevention = parse_self_trade_prevention(value);
        } else {
            throw UsageError("unknown option '" + key + "'");
        }
    }
    return options;
}

// "BUY 100 @ 100.50 IOC post-only", "STOP-LIMIT SELL 10 @ 99.00 limit 98.50 display 5", ...
std::string describe(const OrderRequest& order) {
    std::ostringstream text;
    switch (order.type) {
        case OrderType::Stop:
            text << "STOP ";
            break;
        case OrderType::StopLimit:
            text << "STOP-LIMIT ";
            break;
        case OrderType::Limit:
        case OrderType::Market:
            break;
    }
    text << (order.side == Side::Buy ? "BUY " : "SELL ") << with_commas(order.quantity) << " @ ";
    if (order.type == OrderType::Stop || order.type == OrderType::StopLimit) {
        text << format_price(order.stop_price);
        if (order.type == OrderType::StopLimit) {
            text << " limit " << format_price(order.price);
        }
    } else if (order.type == OrderType::Market) {
        text << "market";
    } else {
        text << format_price(order.price);
    }
    if (order.time_in_force != TimeInForce::GoodTillCancel) {
        text << ' ' << short_name(order.time_in_force);
    }
    if (order.post_only) {
        text << " post-only";
    }
    if (order.display_quantity != 0U) {
        text << " display " << with_commas(order.display_quantity);
    }
    if (order.owner != 0U) {
        text << " owner " << order.owner;
    }
    if (order.self_trade_prevention != SelfTradePrevention::None) {
        text << " stp " << short_name(order.self_trade_prevention);
    }
    return text.str();
}

std::string outcome(const OrderRequest& order, const SubmitResult& result) {
    if (!result.accepted) {
        return std::string{"rejected: "} + to_string(result.reject_reason);
    }
    if (result.pending_trigger) {
        return "waiting for a trade at " + format_price(order.stop_price);
    }
    if (result.queued_for_auction) {
        return "queued for the auction";
    }
    std::vector<std::string> parts;
    if (result.filled_quantity != 0U) {
        parts.push_back("filled " + with_commas(result.filled_quantity));
    }
    if (result.rested) {
        parts.push_back("resting " + with_commas(result.remaining_quantity));
    }
    if (result.expired) {
        const std::string remaining = with_commas(result.remaining_quantity);
        if (result.self_trade_prevented) {
            parts.push_back("self-trade prevention cancelled the remaining " + remaining);
        } else if (order.time_in_force == TimeInForce::FillOrKill && result.filled_quantity == 0U) {
            parts.push_back("killed: there wasn't enough to fill all " + remaining);
        } else {
            parts.push_back("cancelled the unfilled " + remaining);
        }
    }
    if (parts.empty()) {
        return "done";
    }
    std::string text = parts.front();
    for (std::size_t index = 1; index < parts.size(); ++index) {
        text += ", " + parts[index];
    }
    return text;
}

OrderId highest_order_id(const EngineSnapshot& snapshot) {
    OrderId highest = 0;
    for (const auto& symbol : snapshot.symbols) {
        for (const auto* levels : {&symbol.book.bids, &symbol.book.asks}) {
            for (const auto& level : *levels) {
                for (const auto& order : level.orders) {
                    highest = std::max(highest, order.order_id);
                }
            }
        }
        for (const auto& stop : symbol.book.stops) {
            highest = std::max(highest, stop.request.order_id);
        }
        for (const auto* queue : {&symbol.book.auction_buys, &symbol.book.auction_sells}) {
            for (const auto& queued : *queue) {
                highest = std::max(highest, queued.order_id);
            }
        }
    }
    return highest;
}

}  // namespace

CommandInterpreter::CommandInterpreter(std::ostream& out) : out_(out) {
    reset_engine();
}

CommandInterpreter::~CommandInterpreter() = default;

void CommandInterpreter::reset_engine() {
    journaled_.reset();
    engine_ = std::make_unique<MatchingEngine>();
    feed_ = std::make_unique<MarketDataFeed>(FeedConfig{1'000'000'000, kTapeLength, false});
    engine_->set_listener(feed_.get());
    journaled_ = std::make_unique<JournaledEngine>(*engine_, journal_.get());
    current_.reset();
}

bool CommandInterpreter::execute(std::string_view line) {
    Tokens tokens = tokenize(line);
    if (tokens.empty()) {
        return true;
    }
    const std::string command = lower(tokens.front());
    tokens.erase(tokens.begin());
    if (command == "quit" || command == "exit") {
        return false;
    }
    try {
        dispatch(command, tokens);
    } catch (const std::exception& error) {
        ++errors_;
        out_ << "error: " << error.what() << '\n';
    }
    return true;
}

void CommandInterpreter::dispatch(const std::string& command, Tokens& args) {
    if (command == "help") {
        help();
    } else if (command == "add") {
        add(args);
    } else if (command == "use") {
        use(args);
    } else if (command == "symbols") {
        symbols();
    } else if (command == "buy" || command == "sell") {
        order(command == "buy" ? Side::Buy : Side::Sell, args, false);
    } else if (command == "stop") {
        const std::string side = args.empty() ? std::string{} : lower(args.front());
        if (side != "buy" && side != "sell") {
            throw UsageError("usage: stop buy|sell [SYMBOL] QTY @ STOP_PRICE [limit PRICE] [options]");
        }
        args.erase(args.begin());
        order(side == "buy" ? Side::Buy : Side::Sell, args, true);
    } else if (command == "cancel") {
        cancel(args);
    } else if (command == "replace") {
        replace(args);
    } else if (command == "book") {
        book(args);
    } else if (command == "order") {
        show_order(args);
    } else if (command == "auction") {
        auction(args);
    } else if (command == "expire") {
        expire();
    } else if (command == "trades") {
        trades(args);
    } else if (command == "hash") {
        std::ostringstream hash;
        hash << "0x" << std::hex << std::setw(16) << std::setfill('0') << state_hash(*engine_);
        out_ << "state hash " << hash.str() << '\n';
    } else if (command == "save") {
        save(args);
    } else if (command == "load") {
        load(args);
    } else if (command == "journal") {
        journal(args);
    } else {
        throw UsageError("unknown command '" + command + "' (try help)");
    }
}

void CommandInterpreter::help() {
    out_ << "Orders (SYMBOL can be left out once one is selected; prices like 100.25):\n"
            "  buy|sell [SYMBOL] QTY [@ PRICE | market] [options]\n"
            "  stop buy|sell [SYMBOL] QTY @ STOP_PRICE [limit PRICE] [options]\n"
            "  cancel [SYMBOL] ID\n"
            "  replace [SYMBOL] ID QTY @ PRICE [options]\n"
            "  options: gtc day ioc fok  post-only  display=N  owner=N  stp=none|newest|oldest|both  id=N\n"
            "Market:\n"
            "  add SYMBOL | use SYMBOL | symbols\n"
            "  book [SYMBOL] [LEVELS]     order [SYMBOL] ID     trades [SYMBOL] [N]\n"
            "  auction [SYMBOL] begin|show|uncross     expire     hash\n"
            "  save FILE | load FILE      snapshots of every book\n"
            "  journal FILE               record every command that follows (start before adding symbols)\n"
            "  quit\n";
}

SymbolId CommandInterpreter::current_symbol() const {
    if (current_) {
        return *current_;
    }
    if (engine_->symbol_count() == 1U) {
        return 0;
    }
    throw UsageError(engine_->symbol_count() == 0U ? "there are no symbols yet: try add ACME"
                                                   : "several symbols exist: name one, or select it with use");
}

// Consumes a leading symbol name if there is one; otherwise uses the current symbol.
SymbolId CommandInterpreter::take_symbol(Tokens& args) {
    if (args.empty() || !starts_with_letter(args.front())) {
        return current_symbol();
    }
    const std::string name = upper(args.front());
    const auto symbol = engine_->find_symbol(name);
    if (!symbol) {
        throw UsageError("unknown symbol " + name + " (add it first)");
    }
    args.erase(args.begin());
    current_ = symbol;
    return *symbol;
}

void CommandInterpreter::add(const Tokens& args) {
    if (args.size() != 1U || !starts_with_letter(args.front())) {
        throw UsageError("usage: add SYMBOL");
    }
    const std::string name = upper(args.front());
    const bool exists = engine_->find_symbol(name).has_value();
    current_ = journaled_->add_symbol(name);
    out_ << (exists ? "selected " : "added ") << name << '\n';
}

void CommandInterpreter::use(const Tokens& args) {
    if (args.size() != 1U) {
        throw UsageError("usage: use SYMBOL");
    }
    Tokens symbol = args;
    out_ << "using " << engine_->symbol_name(take_symbol(symbol)) << '\n';
}

void CommandInterpreter::symbols() {
    if (engine_->symbol_count() == 0U) {
        out_ << "no symbols yet\n";
        return;
    }
    for (SymbolId symbol = 0; symbol < engine_->symbol_count(); ++symbol) {
        const auto& book = engine_->book(symbol);
        out_ << (current_ == symbol ? "* " : "  ") << engine_->symbol_name(symbol) << "  resting "
             << with_commas(book.order_count()) << '\n';
    }
}

void CommandInterpreter::order(Side side, Tokens& args, bool stop) {
    const SymbolId symbol = take_symbol(args);
    if (args.empty()) {
        throw UsageError("missing the quantity");
    }
    OrderRequest request;
    request.side = side;
    request.quantity = parse_bounded<Quantity>(args.front(), "quantity");
    std::size_t next = 1;

    const auto price_token = [&]() -> const std::string& {
        if (next >= args.size()) {
            throw UsageError("missing a price after '@'");
        }
        return args[next++];
    };
    const bool has_at = next < args.size() && args[next] == "@";
    if (has_at) {
        ++next;
    }
    if (stop) {
        if (!has_at) {
            throw UsageError("stop orders need '@ STOP_PRICE'");
        }
        request.type = OrderType::Stop;
        request.stop_price = parse_price(price_token());
        if (next < args.size() && lower(args[next]) == "limit") {
            ++next;
            request.type = OrderType::StopLimit;
            request.price = parse_price(price_token());
        }
    } else if (has_at) {
        const std::string& token = price_token();
        request.type = lower(token) == "market" ? OrderType::Market : OrderType::Limit;
        request.price = request.type == OrderType::Market ? 0 : parse_price(token);
    } else {
        request.type = OrderType::Market;  // no price, or the word market
        if (next < args.size() && lower(args[next]) == "market") {
            ++next;
        }
    }

    const OrderOptions options = parse_options(std::span(args).subspan(next));
    request.order_id = options.order_id.value_or(next_order_id_);
    next_order_id_ = std::max(next_order_id_, request.order_id + 1);
    request.time_in_force = options.time_in_force.value_or(TimeInForce::GoodTillCancel);
    request.post_only = options.post_only;
    request.display_quantity = options.display_quantity.value_or(0);
    request.owner = options.owner.value_or(0);
    request.self_trade_prevention = options.self_trade_prevention.value_or(SelfTradePrevention::None);

    const SubmitResult result = journaled_->submit(symbol, request);
    out_ << engine_->symbol_name(symbol) << " #" << request.order_id << ' ' << describe(request) << " -> "
         << outcome(request, result) << '\n';
    print_events(result.trades, result.triggered_stops, result.self_trade_cancels);
}

void CommandInterpreter::cancel(Tokens& args) {
    const SymbolId symbol = take_symbol(args);
    if (args.size() != 1U) {
        throw UsageError("usage: cancel [SYMBOL] ID");
    }
    const OrderId order_id = parse_unsigned(args.front(), "order ID");
    const CancelResult result = journaled_->cancel(symbol, order_id);
    out_ << engine_->symbol_name(symbol) << " #" << order_id << ' '
         << (result.cancelled ? "cancelled " + with_commas(result.cancelled_quantity)
                              : std::string{"cancel rejected: "} + to_string(result.reject_reason))
         << '\n';
}

void CommandInterpreter::replace(Tokens& args) {
    const SymbolId symbol = take_symbol(args);
    if (args.size() < 4U || args[2] != "@") {
        throw UsageError("usage: replace [SYMBOL] ID QTY @ PRICE [options]");
    }
    // The side, time in force, display size, and owner default to the resting order's, so a plain size cut amends in
    // place. An unknown ID is passed through for the engine to reject.
    const auto existing = engine_->book(symbol).get_order(parse_unsigned(args[0], "order ID"));
    const OrderOptions options = parse_options(std::span(args).subspan(4));
    OrderRequest request;
    request.order_id = parse_unsigned(args[0], "order ID");
    request.side = existing ? existing->side : Side::Buy;
    request.type = OrderType::Limit;
    request.quantity = parse_bounded<Quantity>(args[1], "quantity");
    request.price = parse_price(args[3]);
    request.time_in_force =
        options.time_in_force.value_or(existing ? existing->time_in_force : TimeInForce::GoodTillCancel);
    request.post_only = options.post_only;
    request.display_quantity = options.display_quantity.value_or(existing ? existing->display_quantity : 0);
    request.owner = options.owner.value_or(existing ? existing->owner : 0);
    request.self_trade_prevention = options.self_trade_prevention.value_or(SelfTradePrevention::None);

    const ReplaceResult result = journaled_->replace(symbol, request);
    out_ << engine_->symbol_name(symbol) << " #" << request.order_id << " replace with "
         << with_commas(request.quantity) << " @ " << format_price(request.price) << " -> ";
    if (!result.replaced) {
        out_ << "rejected: " << to_string(result.reject_reason) << '\n';
        return;
    }
    out_ << (result.priority_kept ? "amended in place, priority kept: " : "priority reset: ")
         << outcome(request, result.submit_result) << '\n';
    print_events(result.submit_result.trades,
                 result.submit_result.triggered_stops,
                 result.submit_result.self_trade_cancels);
}

void CommandInterpreter::book(Tokens& args) {
    const SymbolId symbol = take_symbol(args);
    const std::size_t levels = args.empty() ? kDefaultBookLevels : parse_unsigned(args.front(), "number of levels");
    const OrderBook& book = engine_->book(symbol);
    const auto last = book.last_trade_price();
    out_ << engine_->symbol_name(symbol) << "  " << (book.phase() == TradingPhase::Auction ? "AUCTION" : "CONTINUOUS")
         << "  last " << (last ? format_price(*last) : "-") << "  resting " << with_commas(book.order_count());
    if (book.stop_order_count() != 0U) {
        out_ << "  stops " << with_commas(book.stop_order_count());
    }
    if (const auto indicative = book.indicative_auction()) {
        out_ << "  indicative " << format_price(indicative->price) << " x "
             << with_commas(indicative->matched_quantity);
    }
    out_ << '\n';

    const BookSnapshot snapshot = book.snapshot();
    if (snapshot.bids.empty() && snapshot.asks.empty()) {
        out_ << "  (empty)\n";
        return;
    }
    const auto cell = [](const PriceLevelView& level) {
        std::ostringstream text;
        text << std::setw(9) << format_price(level.price) << " x " << std::left << std::setw(7)
             << with_commas(level.total_quantity) << std::right << " (" << level.order_count << ')';
        return text.str();
    };
    out_ << "              BID                       ASK\n";
    const std::size_t rows = std::min(levels, std::max(snapshot.bids.size(), snapshot.asks.size()));
    for (std::size_t row = 0; row < rows; ++row) {
        const std::string bid = row < snapshot.bids.size() ? cell(snapshot.bids[row]) : std::string{};
        const std::string ask = row < snapshot.asks.size() ? cell(snapshot.asks[row]) : std::string{};
        std::string line = "  " + bid;
        line.resize(std::max<std::size_t>(line.size(), 28), ' ');
        line += ask;
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out_ << line << '\n';
    }
}

void CommandInterpreter::show_order(Tokens& args) {
    const SymbolId symbol = take_symbol(args);
    if (args.size() != 1U) {
        throw UsageError("usage: order [SYMBOL] ID");
    }
    const OrderId order_id = parse_unsigned(args.front(), "order ID");
    const OrderBook& book = engine_->book(symbol);
    out_ << engine_->symbol_name(symbol) << " #" << order_id << ' ';
    if (const auto view = book.get_order(order_id)) {
        out_ << (view->side == Side::Buy ? "BUY " : "SELL ") << with_commas(view->remaining_quantity) << " @ "
             << format_price(view->price) << ' ' << short_name(view->time_in_force) << ", sequence " << view->sequence;
        if (view->display_quantity != 0U) {
            out_ << ", iceberg showing " << with_commas(view->visible_quantity) << " (tranches of "
                 << with_commas(view->display_quantity) << ')';
        }
        if (view->owner != 0U) {
            out_ << ", owner " << view->owner;
        }
        out_ << '\n';
    } else if (book.has_stop_order(order_id)) {
        out_ << "is a stop order waiting for its trigger\n";
    } else {
        out_ << "is not in the book\n";
    }
}

void CommandInterpreter::auction(Tokens& args) {
    const bool named = !args.empty() && lower(args.front()) != "begin" && lower(args.front()) != "show" &&
                       lower(args.front()) != "uncross";
    const SymbolId symbol = named ? take_symbol(args) : current_symbol();
    const std::string action = args.size() == 1U ? lower(args.front()) : std::string{};
    const std::string_view name = engine_->symbol_name(symbol);
    if (action == "begin") {
        journaled_->begin_auction(symbol);
        out_ << name << " call auction started: orders collect without matching\n";
    } else if (action == "show") {
        const OrderBook& book = engine_->book(symbol);
        if (book.phase() != TradingPhase::Auction) {
            out_ << name << " is trading continuously\n";
        } else if (const auto indicative = book.indicative_auction()) {
            out_ << name << " would uncross at " << format_price(indicative->price) << ": "
                 << with_commas(indicative->matched_quantity) << " matched, buy surplus "
                 << with_commas(indicative->buy_surplus) << ", sell surplus " << with_commas(indicative->sell_surplus)
                 << '\n';
        } else {
            out_ << name << " has no crossing interest yet\n";
        }
    } else if (action == "uncross") {
        const AuctionResult result = journaled_->uncross(symbol);
        if (result.executed) {
            out_ << name << " uncrossed at " << format_price(result.price) << ": "
                 << with_commas(result.matched_quantity) << " matched\n";
        } else {
            out_ << name << " back to continuous trading with nothing to uncross\n";
        }
        for (const OrderId order_id : result.expired_market_orders) {
            out_ << "  market order #" << order_id << " expired unfilled\n";
        }
        print_events(result.trades, result.triggered_stops, {});
    } else {
        throw UsageError("usage: auction [SYMBOL] begin|show|uncross");
    }
}

void CommandInterpreter::expire() {
    const auto expired = journaled_->expire_day_orders();
    if (expired.empty()) {
        out_ << "no day orders to expire\n";
        return;
    }
    out_ << "expired " << expired.size() << " day order" << (expired.size() == 1U ? "" : "s") << ':';
    for (const auto& [symbol, order_id] : expired) {
        out_ << ' ' << engine_->symbol_name(symbol) << " #" << order_id;
    }
    out_ << '\n';
}

void CommandInterpreter::trades(Tokens& args) {
    const SymbolId symbol = take_symbol(args);
    const std::size_t count = args.empty() ? kDefaultTrades : parse_unsigned(args.front(), "number of trades");
    const auto& tape = feed_->recent_trades(symbol);
    if (tape.empty()) {
        out_ << engine_->symbol_name(symbol) << " has no trades yet\n";
        return;
    }
    std::size_t shown = 0;
    for (auto entry = tape.rbegin(); entry != tape.rend() && shown < count; ++entry, ++shown) {
        const Trade& trade = entry->trade;
        out_ << "  trade #" << trade.trade_id << "  " << with_commas(trade.quantity) << " @ "
             << format_price(trade.price) << "  buy #" << trade.buy_order_id << "  sell #" << trade.sell_order_id
             << (trade.auction ? "  (auction)" : "") << '\n';
    }
}

void CommandInterpreter::save(const Tokens& args) {
    if (args.size() != 1U) {
        throw UsageError("usage: save FILE");
    }
    const std::uint64_t sequence = journal_ ? journal_->last_sequence() : 0;
    save_snapshot(args.front(), capture(*engine_, sequence));
    out_ << "saved " << engine_->symbol_count() << " symbol" << (engine_->symbol_count() == 1U ? "" : "s") << " to "
         << args.front() << '\n';
}

void CommandInterpreter::load(const Tokens& args) {
    if (args.size() != 1U) {
        throw UsageError("usage: load FILE");
    }
    if (journal_) {
        throw UsageError("can't load a snapshot while journaling: the journal would no longer replay");
    }
    const EngineSnapshot snapshot = load_snapshot(args.front());
    reset_engine();
    restore(*engine_, snapshot);
    next_order_id_ = highest_order_id(snapshot) + 1;
    std::size_t resting = 0;
    for (SymbolId symbol = 0; symbol < engine_->symbol_count(); ++symbol) {
        resting += engine_->book(symbol).order_count();
    }
    out_ << "loaded " << engine_->symbol_count() << " symbol" << (engine_->symbol_count() == 1U ? "" : "s") << " with "
         << with_commas(resting) << " resting orders from " << args.front() << '\n';
}

void CommandInterpreter::journal(const Tokens& args) {
    if (args.size() != 1U) {
        throw UsageError("usage: journal FILE");
    }
    if (journal_) {
        throw UsageError("already journaling");
    }
    if (engine_->symbol_count() != 0U) {
        throw UsageError("start the journal before adding symbols, so that it records everything");
    }
    // Each command reaches the operating system before the next prompt.
    journal_ = std::make_unique<JournalWriter>(args.front(), JournalOptions{Durability::Flush, true, 1});
    journaled_->set_journal(journal_.get());
    out_ << "journaling to " << args.front() << '\n';
}

void CommandInterpreter::print_events(std::span<const Trade> trades,
                                      std::span<const OrderId> triggered_stops,
                                      std::span<const OrderId> self_trade_cancels) {
    for (const Trade& trade : trades) {
        out_ << "  trade #" << trade.trade_id << "  " << with_commas(trade.quantity) << " @ "
             << format_price(trade.price) << "  buy #" << trade.buy_order_id << "  sell #" << trade.sell_order_id
             << (trade.auction ? "  (auction)" : "") << '\n';
    }
    for (const OrderId order_id : triggered_stops) {
        out_ << "  stop #" << order_id << " triggered\n";
    }
    for (const OrderId order_id : self_trade_cancels) {
        out_ << "  self-trade prevention cancelled resting #" << order_id << '\n';
    }
}

}  // namespace tradematch
