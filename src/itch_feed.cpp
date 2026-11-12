#include "tradematch/itch_feed.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace tradematch::itch {

namespace {

constexpr char side_code(Side side) noexcept {
    return side == Side::Buy ? 'B' : 'S';
}

constexpr SymbolId kMaxSymbols = 0xFFFE;  // stock locates are 16 bits, and 0 means the whole market

}  // namespace

FeedPublisher::FeedPublisher(const MatchingEngine& engine, std::uint32_t price_multiplier)
    : engine_(engine), price_multiplier_(price_multiplier) {
    if (price_multiplier == 0U) {
        throw std::invalid_argument("the price multiplier must be positive");
    }
}

Header FeedPublisher::header(SymbolId symbol) const noexcept {
    return Header{static_cast<std::uint16_t>(symbol + 1), 0, static_cast<std::uint64_t>(time_)};
}

std::uint32_t FeedPublisher::feed_price(Price price) const {
    if (price <= 0 || std::cmp_greater(price, std::numeric_limits<std::uint32_t>::max() / price_multiplier_)) {
        throw std::out_of_range("the price doesn't fit an ITCH price field");
    }
    return static_cast<std::uint32_t>(price) * price_multiplier_;
}

void FeedPublisher::publish(const Message& message) {
    encode_framed(message, bytes_);
    ++counts_[static_cast<unsigned char>(type_of(message))];
    ++total_messages_;
}

void FeedPublisher::system_event(char event_code) {
    publish(SystemEvent{Header{0, 0, static_cast<std::uint64_t>(time_)}, event_code});
}

void FeedPublisher::announce(SymbolId symbol) {
    if (symbol < announced_.size() && announced_[symbol]) {
        return;
    }
    if (symbol >= kMaxSymbols) {
        throw std::out_of_range("an ITCH feed has room for 65,534 stocks");
    }
    StockDirectory directory{header(symbol)};
    directory.stock = make_stock(engine_.symbol_name(symbol));
    directory.market_category = 'Q';  // NASDAQ Global Select Market
    directory.financial_status = 'N';
    directory.round_lot_size = 100;
    directory.round_lots_only = 'N';
    directory.issue_classification = 'C';  // common stock
    directory.issue_sub_type = {'Z', ' '};
    directory.authenticity = 'T';  // test data, rather than 'P' for production
    directory.short_sale_threshold = 'N';
    directory.ipo_flag = ' ';
    directory.luld_reference_price_tier = ' ';
    directory.etp_flag = 'N';
    directory.inverse_indicator = 'N';
    publish(directory);

    if (announced_.size() <= symbol) {
        announced_.resize(symbol + 1, false);
        displayed_.resize(symbol + 1);
    }
    announced_[symbol] = true;
}

std::unordered_map<OrderId, FeedPublisher::Displayed>& FeedPublisher::orders_of(SymbolId symbol) {
    announce(symbol);
    return displayed_[symbol];
}

void FeedPublisher::on_order_events(SymbolId symbol,
                                    std::span<const OrderEvent> events,
                                    std::span<const Trade> trades) {
    auto& orders = orders_of(symbol);
    const Stock stock = make_stock(engine_.symbol_name(symbol));
    const std::uint64_t first_match = next_match_;
    next_match_ += trades.size();

    const auto displayed = [&](OrderId order_id) -> Displayed& {
        const auto found = orders.find(order_id);
        if (found == orders.end()) {
            throw std::logic_error("an order event names an order that isn't displayed");
        }
        return found->second;
    };
    const auto trade_index = [&](TradeId trade_id) {
        const std::size_t index = trades.empty() ? 0 : static_cast<std::size_t>(trade_id - trades.front().trade_id);
        if (index >= trades.size()) {
            throw std::logic_error("an execution names a trade its request didn't make");
        }
        return index;
    };

    for (std::size_t position = 0; position < events.size(); ++position) {
        const OrderEvent& event = events[position];
        switch (event.type) {
            case OrderEvent::Type::Added: {
                AddOrder add{header(symbol)};
                add.order_reference = next_reference_++;
                add.side = side_code(event.side);
                add.shares = event.quantity;
                add.stock = stock;
                add.price = feed_price(event.price);
                orders[event.order_id] = Displayed{add.order_reference, event.quantity};
                publish(add);
                break;
            }
            case OrderEvent::Type::Executed: {
                Displayed& order = displayed(event.order_id);
                const std::size_t index = trade_index(event.trade_id);
                if (trades[index].auction) {
                    OrderExecutedWithPrice executed{header(symbol)};
                    executed.order_reference = order.reference;
                    executed.executed_shares = event.quantity;
                    executed.match_number = first_match + index;
                    executed.printable = 'N';  // the cross trade message reports the volume
                    executed.execution_price = feed_price(event.price);
                    publish(executed);
                } else {
                    publish(OrderExecuted{header(symbol), order.reference, event.quantity, first_match + index});
                }
                order.shown -= event.quantity;
                if (order.shown == 0U) {
                    orders.erase(event.order_id);
                }
                break;
            }
            case OrderEvent::Type::Reduced: {
                Displayed& order = displayed(event.order_id);
                publish(OrderCancel{header(symbol), order.reference, event.quantity});
                order.shown -= event.quantity;
                break;
            }
            case OrderEvent::Type::Deleted: {
                const std::uint64_t reference = displayed(event.order_id).reference;
                // A replace that re-enters the book without trading is a delete followed at once by an add of the
                // same order: one replace message in ITCH.
                const bool reentered = position + 1 < events.size() &&
                                       events[position + 1].type == OrderEvent::Type::Added &&
                                       events[position + 1].order_id == event.order_id;
                if (!reentered) {
                    orders.erase(event.order_id);
                    publish(OrderDelete{header(symbol), reference});
                    break;
                }
                const OrderEvent& added = events[++position];
                OrderReplace replace{header(symbol)};
                replace.original_order_reference = reference;
                replace.new_order_reference = next_reference_++;
                replace.shares = added.quantity;
                replace.price = feed_price(added.price);
                orders[event.order_id] = Displayed{replace.new_order_reference, added.quantity};
                publish(replace);
                break;
            }
        }
    }

    // An auction's executions are non-printable, and a single cross trade message reports its volume.
    std::uint64_t crossed = 0;
    Price cross_price = 0;
    for (const Trade& trade : trades) {
        if (trade.auction) {
            crossed += trade.quantity;
            cross_price = trade.price;
        }
    }
    if (crossed > 0U) {
        CrossTrade cross{header(symbol)};
        cross.shares = crossed;
        cross.stock = stock;
        cross.cross_price = feed_price(cross_price);
        cross.match_number = next_match_++;
        cross.cross_type = cross_type_;
        publish(cross);
    }
}

std::vector<std::uint8_t> FeedPublisher::take_bytes() {
    std::vector<std::uint8_t> taken;
    taken.swap(bytes_);
    bytes_.reserve(taken.capacity());
    return taken;
}

std::optional<std::uint64_t> FeedPublisher::reference_of(SymbolId symbol, OrderId order_id) const {
    if (symbol >= displayed_.size()) {
        return std::nullopt;
    }
    const auto found = displayed_[symbol].find(order_id);
    return found == displayed_[symbol].end() ? std::nullopt : std::optional<std::uint64_t>{found->second.reference};
}

BookCapacity BookReplica::default_capacity() noexcept {
    BookCapacity capacity;
    capacity.orders = 64;
    capacity.price_levels = 16;
    capacity.trades_per_call = 1;
    capacity.stop_orders = 1;
    return capacity;
}

BookReplica::BookReplica(const BookCapacity& capacity) : capacity_(capacity) {}

std::optional<SymbolId> BookReplica::symbol_at(std::uint16_t locate) const noexcept {
    return locate < symbols_.size() ? symbols_[locate] : std::nullopt;
}

SymbolId BookReplica::add_stock(std::uint16_t locate, std::string_view name) {
    const SymbolId symbol =
        engine_.add_symbol(name.empty() ? "LOCATE" + std::to_string(locate) : std::string{name}, capacity_);
    engine_.book(symbol).begin_auction();  // never match: the feed reports every execution
    if (symbols_.size() <= locate) {
        symbols_.resize(std::size_t{locate} + 1);
    }
    symbols_[locate] = symbol;
    if (stock_stats_.size() <= symbol) {
        stock_stats_.resize(std::size_t{symbol} + 1);
    }
    return symbol;
}

void BookReplica::count_order(bool added) noexcept {
    if (added) {
        ++stats_.live_orders;
        stats_.peak_orders = std::max(stats_.peak_orders, stats_.live_orders);
    } else {
        --stats_.live_orders;
    }
}

void BookReplica::record_volume(std::optional<SymbolId> symbol, std::uint64_t shares, std::uint32_t price) {
    if (!symbol) {
        return;
    }
    StockStats& stock = stock_stats_[*symbol];
    stock.volume += shares;
    stock.value += static_cast<double>(shares) * static_cast<double>(price) / kPriceScale;
    ++stock.executions;
    stock.last_price = price;
}

void BookReplica::add(const AddOrder& order) {
    auto symbol = symbol_at(order.header.stock_locate);
    if (!symbol) {
        symbol = add_stock(order.header.stock_locate, symbol_of(order.stock));  // the add names its stock
    }
    if (order.side != 'B' && order.side != 'S') {
        ++stats_.anomalies;
        return;
    }
    const OrderRequest request{order.order_reference,
                               order.side == 'B' ? Side::Buy : Side::Sell,
                               OrderType::Limit,
                               order.shares,
                               order.price};
    if (engine_.book(*symbol).submit(request).accepted) {
        count_order(true);
    } else {
        ++stats_.anomalies;
    }
}

std::optional<Price> BookReplica::take(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares) {
    const auto symbol = symbol_at(locate);
    if (!symbol) {
        ++stats_.anomalies;
        return std::nullopt;
    }
    OrderBook& book = engine_.book(*symbol);
    const auto order = book.get_order(reference);
    if (!order || shares == 0U) {
        ++stats_.anomalies;
        return std::nullopt;
    }
    if (shares >= order->remaining_quantity) {
        stats_.anomalies += shares > order->remaining_quantity ? 1U : 0U;
        book.cancel(reference);
        count_order(false);
    } else {
        // A smaller quantity at the same price is amended in place, so the order keeps its place in the queue.
        book.replace(
            OrderRequest{reference, order->side, OrderType::Limit, order->remaining_quantity - shares, order->price});
    }
    return order->price;
}

void BookReplica::remove(std::uint16_t locate, std::uint64_t reference) {
    const auto symbol = symbol_at(locate);
    if (symbol && engine_.book(*symbol).cancel(reference).cancelled) {
        count_order(false);
    } else {
        ++stats_.anomalies;
    }
}

void BookReplica::replace(const OrderReplace& replace) {
    const auto symbol = symbol_at(replace.header.stock_locate);
    if (!symbol) {
        ++stats_.anomalies;
        return;
    }
    OrderBook& book = engine_.book(*symbol);
    const auto original = book.get_order(replace.original_order_reference);
    if (!original) {
        ++stats_.anomalies;
        return;
    }
    book.cancel(replace.original_order_reference);
    count_order(false);
    const OrderRequest request{replace.new_order_reference,
                               original->side,
                               OrderType::Limit,
                               replace.shares,
                               replace.price};
    if (book.submit(request).accepted) {
        count_order(true);
    } else {
        ++stats_.anomalies;
    }
}

// Applies each kind of message to the replica's books.
struct BookReplica::Applier {
    BookReplica& replica;

    void operator()(const StockDirectory& directory) const {
        replica.add_stock(directory.header.stock_locate, symbol_of(directory.stock));
    }
    void operator()(const AddOrder& order) const {
        replica.add(order);
    }
    void operator()(const OrderExecuted& executed) const {
        const std::uint16_t locate = executed.header.stock_locate;
        if (const auto price = replica.take(locate, executed.order_reference, executed.executed_shares)) {
            replica.record_volume(replica.symbol_at(locate),
                                  executed.executed_shares,
                                  static_cast<std::uint32_t>(*price));
        }
    }
    void operator()(const OrderExecutedWithPrice& executed) const {
        const std::uint16_t locate = executed.header.stock_locate;
        // A non-printable execution's volume arrives in a cross trade message instead.
        if (replica.take(locate, executed.order_reference, executed.executed_shares) && executed.printable == 'Y') {
            replica.record_volume(replica.symbol_at(locate), executed.executed_shares, executed.execution_price);
        }
    }
    void operator()(const OrderCancel& cancel) const {
        replica.take(cancel.header.stock_locate, cancel.order_reference, cancel.cancelled_shares);
    }
    void operator()(const OrderDelete& deleted) const {
        replica.remove(deleted.header.stock_locate, deleted.order_reference);
    }
    void operator()(const OrderReplace& replacement) const {
        replica.replace(replacement);
    }
    void operator()(const NonCrossTrade& trade) const {
        replica.record_volume(replica.symbol_at(trade.header.stock_locate), trade.shares, trade.price);
    }
    void operator()(const CrossTrade& cross) const {
        replica.record_volume(replica.symbol_at(cross.header.stock_locate), cross.shares, cross.cross_price);
    }
    void operator()(const SystemEvent& /*message*/) const noexcept {}
    void operator()(const StockTradingAction& /*message*/) const noexcept {}
    void operator()(const BrokenTrade& /*message*/) const noexcept {}
    void operator()(const OtherMessage& /*message*/) const noexcept {}
};

void BookReplica::apply(const Message& message) {
    ++stats_.messages[static_cast<unsigned char>(type_of(message))];
    ++stats_.total_messages;
    stats_.last_timestamp = header_of(message).timestamp;
    apply_decoded(message);
}

// The replay path: the type and timestamp come straight from the bytes, sparing two visits of the decoded message.
bool BookReplica::apply(std::span<const std::uint8_t> bytes) {
    ++stats_.total_messages;
    const auto message = decode(bytes);
    if (!message) {
        ++stats_.invalid_messages;
        return false;
    }
    ++stats_.messages[bytes.front()];
    stats_.last_timestamp = timestamp_of(bytes).value_or(stats_.last_timestamp);
    apply_decoded(*message);
    return true;
}

void BookReplica::apply_decoded(const Message& message) {
    std::visit(Applier{*this}, message);
}

std::string first_difference(const MatchingEngine& engine, const FeedPublisher& publisher, const BookReplica& replica) {
    for (SymbolId symbol = 0; symbol < engine.symbol_count(); ++symbol) {
        const std::string name{engine.symbol_name(symbol)};
        const OrderBook& book = engine.book(symbol);
        const BookSnapshot expected = book.snapshot();
        const auto copy = replica.find(name);
        if (!copy) {
            if (expected.bids.empty() && expected.asks.empty()) {
                continue;
            }
            return name + " is missing from the replica";
        }
        const OrderBook& rebuilt = replica.engine().book(*copy);
        const BookSnapshot actual = rebuilt.snapshot();
        for (const auto& [side, expected_levels, actual_levels] :
             {std::tuple{"bid", &expected.bids, &actual.bids}, std::tuple{"ask", &expected.asks, &actual.asks}}) {
            if (expected_levels->size() != actual_levels->size()) {
                return name + ": the replica has " + std::to_string(actual_levels->size()) + " " + side +
                       " levels, the engine " + std::to_string(expected_levels->size());
            }
            for (std::size_t index = 0; index < expected_levels->size(); ++index) {
                const PriceLevelView& want = (*expected_levels)[index];
                const PriceLevelView& have = (*actual_levels)[index];
                const std::string where = name + " " + side + " level " + std::to_string(index) + ": ";
                if (have.price != want.price * publisher.price_multiplier()) {
                    return where + "the prices differ";
                }
                if (have.total_quantity != want.total_quantity || have.order_ids.size() != want.order_ids.size()) {
                    return where + "the quantities or order counts differ";
                }
                for (std::size_t position = 0; position < want.order_ids.size(); ++position) {
                    const auto reference = publisher.reference_of(symbol, want.order_ids[position]);
                    if (!reference || *reference != have.order_ids[position]) {
                        return where + "the queues differ at position " + std::to_string(position);
                    }
                    const auto shown = book.get_order(want.order_ids[position]);
                    const auto copied = rebuilt.get_order(have.order_ids[position]);
                    if (!shown || !copied || shown->visible_quantity != copied->remaining_quantity) {
                        return where + "an order shows a different quantity";
                    }
                }
            }
        }
    }
    return {};
}

}  // namespace tradematch::itch
