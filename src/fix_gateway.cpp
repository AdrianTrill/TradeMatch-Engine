#include "tradematch/fix_gateway.hpp"

#include "tradematch/detail/socket.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace tradematch::fix {

namespace {

constexpr auto kLogonTimeout = std::chrono::seconds{10};
constexpr std::int64_t kMaxHeartbeatSeconds = 3'600;

// Reason codes from FIX 4.2, which has no "other" value for OrdRejReason (103) or CxlRejReason (102): broker option
// covers everything without a code of its own.
constexpr int kOrdRejBrokerOption = 0;
constexpr int kOrdRejUnknownSymbol = 1;
constexpr int kOrdRejDuplicateOrder = 6;
constexpr int kCxlRejTooLateToCancel = 0;
constexpr int kCxlRejUnknownOrder = 1;
constexpr int kCxlRejBrokerOption = 2;
constexpr int kSessionRejRequiredTagMissing = 1;
constexpr int kSessionRejInvalidMsgType = 11;
// CxlRejResponseTo (434).
constexpr std::string_view kResponseToCancel = "1";
constexpr std::string_view kResponseToReplace = "2";

char side_code(Side side) noexcept {
    return side == Side::Buy ? '1' : '2';
}

std::optional<Side> parse_side(std::string_view code) {
    if (code == "1") {
        return Side::Buy;
    }
    if (code == "2") {
        return Side::Sell;
    }
    return std::nullopt;
}

std::optional<OrderType> parse_order_type(std::string_view code) {
    if (code == "1") {
        return OrderType::Market;
    }
    if (code == "2") {
        return OrderType::Limit;
    }
    if (code == "3") {
        return OrderType::Stop;
    }
    if (code == "4") {
        return OrderType::StopLimit;
    }
    return std::nullopt;
}

std::optional<TimeInForce> parse_time_in_force(std::string_view code) {
    if (code == "0") {
        return TimeInForce::Day;
    }
    if (code == "1") {
        return TimeInForce::GoodTillCancel;
    }
    if (code == "3") {
        return TimeInForce::ImmediateOrCancel;
    }
    if (code == "4") {
        return TimeInForce::FillOrKill;
    }
    return std::nullopt;
}

std::optional<Quantity> parse_quantity(const Message& message, int field) {
    const auto value = message.get_int(field);
    if (!value || *value <= 0 || std::cmp_greater(*value, std::numeric_limits<Quantity>::max())) {
        return std::nullopt;
    }
    return static_cast<Quantity>(*value);
}

}  // namespace

struct GatewayCore::Impl {
    // Orders belong to the CompID that sent them rather than to one connection, so after a disconnect the client can
    // log back on and manage them.
    struct OrderState {
        std::string owner;
        std::string cl_ord_id;
        SymbolId symbol{};
        Side side{Side::Buy};
        TimeInForce time_in_force{TimeInForce::Day};
        Quantity display_quantity{};
        Quantity order_quantity{};  // FIX OrderQty: the total, including fills
        Quantity cumulative_quantity{};
        long double notional{};  // the sum of price times quantity over the fills, in ticks
    };

    struct Session {
        std::string input;
        std::string output;
        bool logged_on{false};
        bool closing{false};
        std::string peer;  // the client's CompID
        std::uint64_t next_incoming{1};
        std::uint64_t next_outgoing{1};
        std::chrono::seconds heartbeat{30};
        Clock::time_point opened;
        Clock::time_point last_received;
        Clock::time_point last_sent;
    };

    explicit Impl(GatewayConfig gateway_config) : config(std::move(gateway_config)) {
        for (const std::string& symbol : config.symbols) {
            engine.add_symbol(symbol, config.capacity);
        }
    }

    // A message with the standard header after its MsgType; the caller adds the body.
    Message header(const Session& session, std::string_view type) const {
        Message message(type);
        message.set(tag::SenderCompID, config.comp_id);
        message.set(tag::TargetCompID, session.peer.empty() ? std::string_view{"UNKNOWN"} : session.peer);
        message.set_int(tag::MsgSeqNum, static_cast<std::int64_t>(session.next_outgoing));
        message.set(tag::SendingTime, utc_timestamp(now));
        return message;
    }

    void deliver(Session& session, const Message& message) {
        session.output += encode(message);
        ++session.next_outgoing;
        session.last_sent = now;
        ++stats.messages_sent;
    }

    void logout_and_close(Session& session, std::string_view reason) {
        Message logout = header(session, msg_type::Logout);
        logout.set(tag::Text, reason);
        deliver(session, logout);
        session.closing = true;
    }

    void session_reject(Session& session,
                        std::int64_t sequence,
                        int reason,
                        std::string_view text,
                        int rejected_field = 0) {
        Message reject = header(session, msg_type::Reject);
        reject.set_int(tag::RefSeqNum, sequence);
        if (rejected_field != 0) {
            reject.set_int(tag::RefTagID, rejected_field);
        }
        reject.set_int(tag::SessionRejectReason, reason);
        reject.set(tag::Text, text);
        deliver(session, reject);
        ++stats.session_rejects;
    }

    // An ExecutionReport for one of the gateway's orders. ExecType and OrdStatus are FIX 4.2 codes.
    struct Report {
        char exec_type{'0'};
        char order_status{'0'};
        Quantity last_shares{};
        Price last_price{};
        std::string_view orig_cl_ord_id;
        std::string_view text;
    };

    void send_report(OrderId order_id, const OrderState& order, const Report& report) {
        const auto active = active_sessions.find(order.owner);
        if (active == active_sessions.end() || sessions.at(active->second).closing) {
            return;  // the owner isn't connected: the order still trades, but without resends the report is lost
        }
        Session& session = sessions.at(active->second);
        Message message = header(session, msg_type::ExecutionReport);
        message.set_int(tag::OrderID, static_cast<std::int64_t>(order_id));
        message.set(tag::ClOrdID, order.cl_ord_id);
        if (!report.orig_cl_ord_id.empty()) {
            message.set(tag::OrigClOrdID, report.orig_cl_ord_id);
        }
        message.set(tag::ExecID, "E" + std::to_string(next_exec_id++));
        message.set(tag::ExecTransType, "0");
        message.set(tag::ExecType, std::string_view{&report.exec_type, 1});
        message.set(tag::OrdStatus, std::string_view{&report.order_status, 1});
        message.set(tag::Symbol, engine.symbol_name(order.symbol));
        const char side = side_code(order.side);
        message.set(tag::Side, std::string_view{&side, 1});
        message.set_int(tag::OrderQty, order.order_quantity);
        if (report.last_shares != 0U) {
            message.set_int(tag::LastShares, report.last_shares);
            message.set(tag::LastPx, format_price(report.last_price));
        }
        const bool done = report.order_status == '2' || report.order_status == '4' || report.order_status == '8';
        message.set_int(tag::LeavesQty, done ? 0 : order.order_quantity - order.cumulative_quantity);
        message.set_int(tag::CumQty, order.cumulative_quantity);
        message.set(tag::AvgPx, average_price(order));
        if (!report.text.empty()) {
            message.set(tag::Text, report.text);
        }
        deliver(session, message);
    }

    static std::string average_price(const OrderState& order) {
        if (order.cumulative_quantity == 0U) {
            return "0";
        }
        std::array<char, 48> text{};
        std::snprintf(text.data(),
                      text.size(),
                      "%.4Lf",
                      order.notional / static_cast<long double>(order.cumulative_quantity) / 100.0L);
        return text.data();
    }

    // The open order the session's CompID knows by this ClOrdID.
    std::optional<OrderId> find_open_order(const Session& session, std::string_view cl_ord_id) const {
        const auto owned = open_orders.find(session.peer);
        if (owned == open_orders.end()) {
            return std::nullopt;
        }
        const auto found = owned->second.find(std::string{cl_ord_id});
        return found == owned->second.end() ? std::nullopt : std::optional<OrderId>{found->second};
    }

    void close_order(OrderId order_id) {
        const auto found = orders.find(order_id);
        if (found == orders.end()) {
            return;
        }
        auto& owned = open_orders[found->second.owner];
        if (const auto entry = owned.find(found->second.cl_ord_id); entry != owned.end() && entry->second == order_id) {
            owned.erase(entry);
        }
        orders.erase(found);
    }

    // Fill reports for both sides of every trade.
    void publish(std::span<const Trade> trades) {
        for (const Trade& trade : trades) {
            ++stats.trades;
            for (const OrderId order_id : {trade.buy_order_id, trade.sell_order_id}) {
                const auto found = orders.find(order_id);
                if (found == orders.end()) {
                    continue;
                }
                OrderState& order = found->second;
                order.cumulative_quantity += trade.quantity;
                order.notional += static_cast<long double>(trade.price) * trade.quantity;
                const bool filled = order.cumulative_quantity >= order.order_quantity;
                send_report(order_id,
                            order,
                            Report{filled ? '2' : '1', filled ? '2' : '1', trade.quantity, trade.price, {}, {}});
                if (filled) {
                    close_order(order_id);
                }
            }
        }
    }

    // A triggered stop that neither rests nor filled completely was expired by the engine, so its owner is told.
    void settle_triggered_stops(SymbolId symbol, std::span<const OrderId> triggered_stops) {
        for (const OrderId order_id : triggered_stops) {
            const auto found = orders.find(order_id);
            if (found != orders.end() && !engine.book(symbol).has_order(order_id)) {
                send_report(order_id, found->second, Report{'4', '4', 0, 0, {}, "unfilled quantity cancelled"});
                close_order(order_id);
            }
        }
    }

    void cancel_remainder(OrderId order_id, const SubmitResult& result) {
        if (!result.expired) {
            return;
        }
        if (const auto found = orders.find(order_id); found != orders.end()) {
            send_report(order_id, found->second, Report{'4', '4', 0, 0, {}, "unfilled quantity cancelled"});
            close_order(order_id);
        }
    }

    // A required field, or a session-level Reject naming it.
    std::optional<std::string_view> require(Session& session, const Message& message, int field) {
        const auto value = message.get(field);
        if (!value) {
            session_reject(session,
                           message.get_int(tag::MsgSeqNum).value_or(0),
                           kSessionRejRequiredTagMissing,
                           "required tag missing",
                           field);
        }
        return value;
    }

    void reject_new_order(Session& session, const Message& message, std::string_view text, int reason) {
        Message report = header(session, msg_type::ExecutionReport);
        report.set(tag::OrderID, "NONE");
        report.set(tag::ClOrdID, message.get(tag::ClOrdID).value_or("NONE"));
        report.set(tag::ExecID, "E" + std::to_string(next_exec_id++));
        report.set(tag::ExecTransType, "0");
        report.set(tag::ExecType, "8");
        report.set(tag::OrdStatus, "8");
        report.set(tag::Symbol, message.get(tag::Symbol).value_or("NONE"));
        report.set(tag::Side, message.get(tag::Side).value_or("1"));
        report.set_int(tag::LeavesQty, 0);
        report.set_int(tag::CumQty, 0);
        report.set(tag::AvgPx, "0");
        report.set_int(tag::OrdRejReason, reason);
        report.set(tag::Text, text);
        deliver(session, report);
        ++stats.order_rejects;
    }

    void cancel_reject(Session& session,
                       const Message& message,
                       std::string_view response_to,
                       int reason,
                       std::string_view text) {
        Message reject = header(session, msg_type::OrderCancelReject);
        const auto order_id = find_open_order(session, message.get(tag::OrigClOrdID).value_or(""));
        std::string_view status = "8";  // rejected: there is no such open order
        if (order_id) {
            status = orders.at(*order_id).cumulative_quantity != 0U ? "1" : "0";
        }
        reject.set(tag::OrderID, order_id ? std::to_string(*order_id) : std::string{"NONE"});
        reject.set(tag::ClOrdID, message.get(tag::ClOrdID).value_or("NONE"));
        reject.set(tag::OrigClOrdID, message.get(tag::OrigClOrdID).value_or("NONE"));
        reject.set(tag::OrdStatus, status);
        reject.set(tag::CxlRejResponseTo, response_to);
        reject.set_int(tag::CxlRejReason, reason);
        reject.set(tag::Text, text);
        deliver(session, reject);
        ++stats.order_rejects;
    }

    void new_order(Session& session, const Message& message) {
        const auto cl_ord_id = require(session, message, tag::ClOrdID);
        const auto symbol_name = cl_ord_id ? require(session, message, tag::Symbol) : std::nullopt;
        const auto side_text = symbol_name ? require(session, message, tag::Side) : std::nullopt;
        const auto quantity_text = side_text ? require(session, message, tag::OrderQty) : std::nullopt;
        const auto type_text = quantity_text ? require(session, message, tag::OrdType) : std::nullopt;
        if (!type_text) {
            return;
        }
        const auto side = parse_side(*side_text);
        const auto type = parse_order_type(*type_text);
        const auto time_in_force = parse_time_in_force(message.get(tag::TimeInForce).value_or("0"));
        const auto quantity = parse_quantity(message, tag::OrderQty);
        if (!side || !type || !time_in_force || !quantity) {
            reject_new_order(session,
                             message,
                             "unsupported Side, OrdType, TimeInForce, or OrderQty",
                             kOrdRejBrokerOption);
            return;
        }
        const auto symbol = engine.find_symbol(*symbol_name);
        if (!symbol) {
            reject_new_order(session, message, "unknown symbol", kOrdRejUnknownSymbol);
            return;
        }
        if (find_open_order(session, *cl_ord_id)) {
            reject_new_order(session, message, "ClOrdID is already in use by an open order", kOrdRejDuplicateOrder);
            return;
        }

        OrderRequest request;
        request.order_id = next_order_id++;
        request.side = *side;
        request.type = *type;
        request.quantity = *quantity;
        request.time_in_force = *time_in_force;
        request.post_only = message.get(tag::ExecInst).value_or("").find('6') != std::string_view::npos;
        if (*type == OrderType::Limit || *type == OrderType::StopLimit) {
            const auto price = parse_price(message.get(tag::Price).value_or(""));
            if (!price) {
                reject_new_order(session, message, "a limit price is required", kOrdRejBrokerOption);
                return;
            }
            request.price = *price;
        }
        if (*type == OrderType::Stop || *type == OrderType::StopLimit) {
            const auto stop_price = parse_price(message.get(tag::StopPx).value_or(""));
            if (!stop_price) {
                reject_new_order(session, message, "a stop price is required", kOrdRejBrokerOption);
                return;
            }
            request.stop_price = *stop_price;
        }
        if (message.get(tag::MaxFloor)) {
            const auto display = parse_quantity(message, tag::MaxFloor);
            if (!display) {
                reject_new_order(session, message, "MaxFloor must be a positive quantity", kOrdRejBrokerOption);
                return;
            }
            request.display_quantity = *display;
        }

        // Registered before submitting, because the order's own fills are reported as the engine returns them.
        orders.emplace(request.order_id,
                       OrderState{session.peer,
                                  std::string{*cl_ord_id},
                                  *symbol,
                                  request.side,
                                  request.time_in_force,
                                  request.display_quantity,
                                  request.quantity,
                                  0,
                                  0});
        open_orders[session.peer].emplace(std::string{*cl_ord_id}, request.order_id);

        const SubmitResult result = engine.submit(*symbol, request);
        if (!result.accepted) {
            close_order(request.order_id);
            reject_new_order(session, message, to_string(result.reject_reason), kOrdRejBrokerOption);
            return;
        }
        ++stats.orders;
        send_report(request.order_id, orders.at(request.order_id), Report{'0', '0', 0, 0, {}, {}});
        publish(result.trades);
        cancel_remainder(request.order_id, result);
        settle_triggered_stops(*symbol, result.triggered_stops);
    }

    void cancel(Session& session, const Message& message) {
        const auto cl_ord_id = require(session, message, tag::ClOrdID);
        const auto original = cl_ord_id ? require(session, message, tag::OrigClOrdID) : std::nullopt;
        if (!original) {
            return;
        }
        const auto order_id = find_open_order(session, *original);
        if (!order_id) {
            cancel_reject(session, message, kResponseToCancel, kCxlRejUnknownOrder, "unknown order");
            return;
        }
        if (*cl_ord_id != *original && find_open_order(session, *cl_ord_id)) {
            cancel_reject(session,
                          message,
                          kResponseToCancel,
                          kCxlRejBrokerOption,
                          "ClOrdID is already in use by an open order");
            return;
        }
        OrderState& order = orders.at(*order_id);
        const CancelResult result = engine.cancel(order.symbol, *order_id);
        if (!result.cancelled) {
            cancel_reject(session, message, kResponseToCancel, kCxlRejTooLateToCancel, to_string(result.reject_reason));
            return;
        }
        // The confirmation carries the cancel's ClOrdID, so the old one is released first.
        open_orders[order.owner].erase(order.cl_ord_id);
        order.cl_ord_id = std::string{*cl_ord_id};
        send_report(*order_id, order, Report{'4', '4', 0, 0, *original, {}});
        orders.erase(*order_id);
    }

    void replace(Session& session, const Message& message) {
        const auto cl_ord_id = require(session, message, tag::ClOrdID);
        const auto original = cl_ord_id ? require(session, message, tag::OrigClOrdID) : std::nullopt;
        const auto quantity_text = original ? require(session, message, tag::OrderQty) : std::nullopt;
        if (!quantity_text) {
            return;
        }
        const auto order_id = find_open_order(session, *original);
        if (!order_id) {
            cancel_reject(session, message, kResponseToReplace, kCxlRejUnknownOrder, "unknown order");
            return;
        }
        if (*cl_ord_id != *original && find_open_order(session, *cl_ord_id)) {
            cancel_reject(session,
                          message,
                          kResponseToReplace,
                          kCxlRejBrokerOption,
                          "ClOrdID is already in use by an open order");
            return;
        }
        OrderState& order = orders.at(*order_id);
        const auto total = parse_quantity(message, tag::OrderQty);
        const auto price = parse_price(message.get(tag::Price).value_or(""));
        const auto time_in_force_code = message.get(tag::TimeInForce);
        const auto time_in_force = time_in_force_code ? parse_time_in_force(*time_in_force_code)
                                                      : std::optional<TimeInForce>{order.time_in_force};
        if (!total || *total <= order.cumulative_quantity || !price || !time_in_force ||
            message.get(tag::OrdType).value_or("2") != "2") {
            cancel_reject(session,
                          message,
                          kResponseToReplace,
                          kCxlRejBrokerOption,
                          "a replace needs a limit price and an OrderQty above the filled quantity");
            return;
        }

        OrderRequest request;
        request.order_id = *order_id;
        request.side = order.side;
        request.type = OrderType::Limit;
        request.quantity = *total - order.cumulative_quantity;
        request.price = *price;
        request.time_in_force = *time_in_force;
        request.post_only = message.get(tag::ExecInst).value_or("").find('6') != std::string_view::npos;
        request.display_quantity = order.display_quantity;
        const ReplaceResult result = engine.replace(order.symbol, request);
        if (!result.replaced) {
            cancel_reject(session, message, kResponseToReplace, kCxlRejBrokerOption, to_string(result.reject_reason));
            return;
        }

        auto& owned = open_orders[order.owner];
        owned.erase(order.cl_ord_id);
        order.cl_ord_id = std::string{*cl_ord_id};
        order.order_quantity = *total;
        order.time_in_force = *time_in_force;
        owned.emplace(order.cl_ord_id, *order_id);
        send_report(*order_id, order, Report{'5', order.cumulative_quantity != 0U ? '1' : '0', 0, 0, *original, {}});
        const SymbolId symbol = order.symbol;
        publish(result.submit_result.trades);
        cancel_remainder(*order_id, result.submit_result);
        settle_triggered_stops(symbol, result.submit_result.triggered_stops);
    }

    void logon(SessionId session_id, Session& session, const Message& message) {
        const auto peer = message.get(tag::SenderCompID);
        const auto heartbeat = message.get_int(tag::HeartBtInt);
        session.peer = std::string{peer.value_or("")};
        if (!peer || message.get(tag::TargetCompID) != std::optional<std::string_view>{config.comp_id}) {
            logout_and_close(session, "Logon needs a SenderCompID and TargetCompID " + config.comp_id);
            return;
        }
        if (!heartbeat || *heartbeat < 0 || *heartbeat > kMaxHeartbeatSeconds) {
            logout_and_close(session, "Logon needs a HeartBtInt between 0 and 3600 seconds");
            return;
        }
        if (active_sessions.contains(session.peer)) {
            logout_and_close(session, session.peer + " is already logged on");
            return;
        }
        session.logged_on = true;
        active_sessions.emplace(session.peer, session_id);
        session.heartbeat = std::chrono::seconds{*heartbeat};
        session.next_incoming = static_cast<std::uint64_t>(message.get_int(tag::MsgSeqNum).value_or(1)) + 1;
        Message reply = header(session, msg_type::Logon);
        reply.set(tag::EncryptMethod, "0");
        reply.set_int(tag::HeartBtInt, *heartbeat);
        deliver(session, reply);
        ++stats.sessions;
    }

    void handle(SessionId session_id, Session& session, const Message& message) {
        ++stats.messages_received;
        const auto sequence = message.get_int(tag::MsgSeqNum);
        if (!sequence || *sequence <= 0) {
            logout_and_close(session, "MsgSeqNum is missing or not positive");
            return;
        }
        if (!session.logged_on) {
            if (message.type() != msg_type::Logon) {
                logout_and_close(session, "the first message must be a Logon");
            } else {
                logon(session_id, session, message);
            }
            return;
        }
        if (std::cmp_not_equal(*sequence, session.next_incoming)) {
            logout_and_close(session,
                             "MsgSeqNum " + std::to_string(*sequence) + " received, " +
                                 std::to_string(session.next_incoming) + " expected");
            return;
        }
        ++session.next_incoming;

        const std::string_view type = message.type();
        if (type == msg_type::Heartbeat) {
            return;
        }
        if (type == msg_type::TestRequest) {
            Message heartbeat = header(session, msg_type::Heartbeat);
            heartbeat.set(tag::TestReqID, message.get(tag::TestReqID).value_or("TEST"));
            deliver(session, heartbeat);
        } else if (type == msg_type::Logout) {
            const Message logout = header(session, msg_type::Logout);
            deliver(session, logout);
            session.closing = true;
        } else if (type == msg_type::NewOrderSingle) {
            new_order(session, message);
        } else if (type == msg_type::OrderCancelRequest) {
            cancel(session, message);
        } else if (type == msg_type::OrderCancelReplaceRequest) {
            replace(session, message);
        } else {
            session_reject(session, *sequence, kSessionRejInvalidMsgType, "unsupported MsgType", tag::MsgType);
        }
    }

    GatewayConfig config;
    MatchingEngine engine;
    std::unordered_map<SessionId, Session> sessions;
    std::unordered_map<std::string, SessionId> active_sessions;  // logged-on sessions by CompID
    std::unordered_map<OrderId, OrderState> orders;
    std::unordered_map<std::string, std::unordered_map<std::string, OrderId>> open_orders;  // by CompID, then ClOrdID
    SessionId next_session{1};
    OrderId next_order_id{1};
    std::uint64_t next_exec_id{1};
    Clock::time_point now;
    GatewayStats stats;
};

GatewayCore::GatewayCore(const GatewayConfig& config) : impl_(std::make_unique<Impl>(config)) {}

GatewayCore::~GatewayCore() = default;

SessionId GatewayCore::open_session(Clock::time_point now) {
    const SessionId id = impl_->next_session++;
    Impl::Session& session = impl_->sessions[id];
    session.opened = now;
    session.last_received = now;
    session.last_sent = now;
    return id;
}

void GatewayCore::receive(SessionId session_id, std::string_view bytes, Clock::time_point now) {
    const auto found = impl_->sessions.find(session_id);
    if (found == impl_->sessions.end() || found->second.closing) {
        return;
    }
    Impl::Session& session = found->second;
    impl_->now = now;
    session.last_received = now;
    session.input += bytes;

    Message message;
    std::size_t offset = 0;
    while (!session.closing) {
        const ParseResult result = parse(std::string_view{session.input}.substr(offset), message);
        if (result.status == ParseStatus::Incomplete) {
            break;
        }
        if (result.status == ParseStatus::Invalid) {
            // Without trustworthy framing there is no way to find the next message, so the session ends.
            impl_->logout_and_close(session, result.error);
            break;
        }
        offset += result.consumed;
        impl_->handle(session_id, session, message);
    }
    session.input.erase(0, session.closing ? session.input.size() : offset);
}

void GatewayCore::on_timer(Clock::time_point now) {
    impl_->now = now;
    for (auto& [id, session] : impl_->sessions) {
        if (session.closing) {
            continue;
        }
        if (!session.logged_on) {
            if (now - session.opened > kLogonTimeout) {
                impl_->logout_and_close(session, "no Logon received");
            }
            continue;
        }
        if (session.heartbeat.count() == 0) {
            continue;
        }
        if (now - session.last_received > 2 * session.heartbeat + std::chrono::seconds{1}) {
            impl_->logout_and_close(session, "no messages received within two heartbeat intervals");
        } else if (now - session.last_sent >= session.heartbeat) {
            impl_->deliver(session, impl_->header(session, msg_type::Heartbeat));
        }
    }
}

std::string GatewayCore::take_output(SessionId session) {
    const auto found = impl_->sessions.find(session);
    return found == impl_->sessions.end() ? std::string{} : std::exchange(found->second.output, {});
}

std::vector<SessionId> GatewayCore::sessions_with_output() const {
    std::vector<SessionId> ready;
    for (const auto& [id, session] : impl_->sessions) {
        if (!session.output.empty()) {
            ready.push_back(id);
        }
    }
    return ready;
}

bool GatewayCore::closing(SessionId session) const {
    const auto found = impl_->sessions.find(session);
    return found == impl_->sessions.end() || found->second.closing;
}

void GatewayCore::close_session(SessionId session) {
    // Open orders stay in the book: a disconnect isn't a cancel. They belong to the CompID, which can manage them again
    // after logging back on.
    const auto found = impl_->sessions.find(session);
    if (found == impl_->sessions.end()) {
        return;
    }
    if (const auto active = impl_->active_sessions.find(found->second.peer);
        active != impl_->active_sessions.end() && active->second == session) {
        impl_->active_sessions.erase(active);
    }
    impl_->sessions.erase(found);
}

const MatchingEngine& GatewayCore::engine() const noexcept {
    return impl_->engine;
}

const GatewayStats& GatewayCore::stats() const noexcept {
    return impl_->stats;
}

struct Gateway::Impl {
    explicit Impl(const GatewayConfig& config)
        : listener(detail::Socket::listen(config.port, config.loopback_only)),
          port(listener.local_port()),
          core(config) {
        listener.set_nonblocking();
    }

    struct Connection {
        detail::Socket socket;
        std::string pending;  // output the socket hasn't accepted yet
        bool disconnected{false};
    };

    void flush(SessionId id, Connection& connection) {
        connection.pending += core.take_output(id);
        while (!connection.pending.empty()) {
            const auto sent = connection.socket.send(connection.pending);
            if (sent < 0) {
                connection.disconnected = true;
                return;
            }
            if (sent == 0) {
                return;  // the socket buffer is full; poll for POLLOUT
            }
            connection.pending.erase(0, static_cast<std::size_t>(sent));
        }
    }

    void poll_once(std::chrono::milliseconds timeout) {
        std::vector<detail::PollEntry> entries;
        std::vector<SessionId> ids;
        entries.push_back(detail::PollEntry{listener.native(), POLLIN, 0});
        for (const auto& [id, connection] : connections) {
            const short events = connection.pending.empty() ? short{POLLIN} : static_cast<short>(POLLIN | POLLOUT);
            entries.push_back(detail::PollEntry{connection.socket.native(), events, 0});
            ids.push_back(id);
        }
        detail::poll_sockets(entries, static_cast<int>(timeout.count()));
        const auto now = Clock::now();

        if ((entries.front().revents & POLLIN) != 0) {
            for (detail::Socket accepted = listener.accept(); accepted.valid(); accepted = listener.accept()) {
                accepted.set_nonblocking();
                accepted.set_no_delay();
                connections.emplace(core.open_session(now), Connection{std::move(accepted), {}, false});
            }
        }
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const short events = entries[index + 1].revents;
            Connection& connection = connections.at(ids[index]);
            if ((events & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            while (true) {
                const auto received = connection.socket.receive(buffer);
                if (received < 0) {
                    connection.disconnected = true;
                    break;
                }
                if (received == 0) {
                    break;
                }
                core.receive(ids[index], std::string_view{buffer.data(), static_cast<std::size_t>(received)}, now);
            }
        }
        core.on_timer(now);

        // A message on one session can produce reports for others, so every session's output is flushed.
        for (const SessionId id : core.sessions_with_output()) {
            if (const auto found = connections.find(id); found != connections.end()) {
                flush(id, found->second);
            }
        }
        for (auto& [id, connection] : connections) {
            if (!connection.pending.empty() && !connection.disconnected) {
                flush(id, connection);
            }
        }
        std::erase_if(connections, [this](auto& entry) {
            auto& [id, connection] = entry;
            const bool done = connection.disconnected || (core.closing(id) && connection.pending.empty());
            if (done) {
                core.close_session(id);
            }
            return done;
        });
    }

    detail::SocketLibrary library;
    detail::Socket listener;
    std::uint16_t port;
    GatewayCore core;
    std::unordered_map<SessionId, Connection> connections;
    std::array<char, 65'536> buffer{};
};

Gateway::Gateway(const GatewayConfig& config) : impl_(std::make_unique<Impl>(config)) {}

Gateway::~Gateway() = default;

std::uint16_t Gateway::port() const noexcept {
    return impl_->port;
}

void Gateway::poll_once(std::chrono::milliseconds timeout) {
    impl_->poll_once(timeout);
}

void Gateway::run(const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        impl_->poll_once(std::chrono::milliseconds{50});
    }
}

const GatewayCore& Gateway::core() const noexcept {
    return impl_->core;
}

}  // namespace tradematch::fix
