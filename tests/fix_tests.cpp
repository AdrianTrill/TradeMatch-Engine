#include "tradematch/detail/socket.hpp"
#include "tradematch/fix.hpp"
#include "tradematch/fix_gateway.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using tradematch::fix::Clock;
using tradematch::fix::GatewayCore;
using tradematch::fix::Message;
using tradematch::fix::ParseStatus;
namespace tag = tradematch::fix::tag;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

std::string soh(std::string_view text) {
    std::string bytes{text};
    for (char& character : bytes) {
        if (character == '|') {
            character = tradematch::fix::kSoh;
        }
    }
    return bytes;
}

std::vector<Message> parse_all(std::string_view bytes) {
    std::vector<Message> messages;
    while (!bytes.empty()) {
        Message message;
        const auto result = tradematch::fix::parse(bytes, message);
        expect(result.status == ParseStatus::Complete, "The gateway should only send valid messages.");
        messages.push_back(message);
        bytes.remove_prefix(result.consumed);
    }
    return messages;
}

std::string field(const Message& message, int field_tag) {
    return std::string{message.get(field_tag).value_or("<missing>")};
}

constexpr Clock::time_point kStart = Clock::time_point{} + std::chrono::hours{24 * 20'000};

// A scripted FIX client talking to a GatewayCore directly.
class Client {
public:
    Client(GatewayCore& core, std::string comp_id)
        : core_(core), comp_id_(std::move(comp_id)), session_(core.open_session(kStart)) {}

    Message message(std::string_view type) {
        Message message(type);
        message.set(tag::SenderCompID, comp_id_);
        message.set(tag::TargetCompID, "TRADEMATCH");
        message.set_int(tag::MsgSeqNum, sequence_++);
        message.set(tag::SendingTime, "20250101-12:00:00.000");
        return message;
    }

    std::vector<Message> send(const Message& message, Clock::time_point now = kStart) {
        core_.receive(session_, tradematch::fix::encode(message), now);
        return receive();
    }

    std::vector<Message> receive() {
        return parse_all(core_.take_output(session_));
    }

    std::vector<Message> logon(int heartbeat = 30) {
        Message logon = message(tradematch::fix::msg_type::Logon);
        logon.set(tag::EncryptMethod, "0");
        logon.set_int(tag::HeartBtInt, heartbeat);
        return send(logon);
    }

    // side 1 = buy, 2 = sell; type 1 = market, 2 = limit, 3 = stop, 4 = stop limit; time in force 0 = day,
    // 1 = GTC, 3 = IOC, 4 = FOK.
    Message order(std::string_view cl_ord_id,
                  std::string_view side,
                  std::int64_t quantity,
                  std::string_view type,
                  std::string_view price,
                  std::string_view time_in_force = "1") {
        Message order = message(tradematch::fix::msg_type::NewOrderSingle);
        order.set(tag::ClOrdID, cl_ord_id);
        order.set(tag::Symbol, "ACME");
        order.set(tag::Side, side);
        order.set_int(tag::OrderQty, quantity);
        order.set(tag::OrdType, type);
        if (!price.empty()) {
            order.set(tag::Price, price);
        }
        order.set(tag::TimeInForce, time_in_force);
        return order;
    }

    Message cancel(std::string_view original, std::string_view cl_ord_id, std::string_view side) {
        Message cancel = message(tradematch::fix::msg_type::OrderCancelRequest);
        cancel.set(tag::OrigClOrdID, original).set(tag::ClOrdID, cl_ord_id).set(tag::Symbol, "ACME");
        cancel.set(tag::Side, side);
        return cancel;
    }

    // A replace with a limit price (none if empty); total is the new OrderQty, including fills.
    Message replace(std::string_view original,
                    std::string_view cl_ord_id,
                    std::string_view side,
                    std::int64_t total,
                    std::string_view price) {
        Message replace = message(tradematch::fix::msg_type::OrderCancelReplaceRequest);
        replace.set(tag::OrigClOrdID, original).set(tag::ClOrdID, cl_ord_id).set(tag::Symbol, "ACME");
        replace.set(tag::Side, side).set_int(tag::OrderQty, total).set(tag::OrdType, "2");
        if (!price.empty()) {
            replace.set(tag::Price, price);
        }
        return replace;
    }

    tradematch::fix::SessionId session() const noexcept {
        return session_;
    }

private:
    GatewayCore& core_;
    std::string comp_id_;
    tradematch::fix::SessionId session_;
    std::int64_t sequence_{1};
};

// The report types and statuses in order, as "150/39" pairs such as "0/0 2/2".
std::string reports(const std::vector<Message>& messages) {
    std::string summary;
    for (const Message& message : messages) {
        if (!summary.empty()) {
            summary += ' ';
        }
        summary += message.type() == tradematch::fix::msg_type::ExecutionReport
                       ? field(message, tag::ExecType) + "/" + field(message, tag::OrdStatus)
                       : "35=" + std::string{message.type()};
    }
    return summary;
}

void test_encoding_matches_the_standard() {
    Message heartbeat(tradematch::fix::msg_type::Heartbeat);
    heartbeat.set(tag::SenderCompID, "CLIENT")
        .set(tag::TargetCompID, "TRADEMATCH")
        .set_int(tag::MsgSeqNum, 2)
        .set(tag::SendingTime, "20250101-12:00:00.000");
    // BodyLength and CheckSum computed independently.
    const std::string expected =
        soh("8=FIX.4.2|9=59|35=0|49=CLIENT|56=TRADEMATCH|34=2|52=20250101-12:00:00.000|10=083|");
    const std::string encoded = tradematch::fix::encode(heartbeat);
    expect(encoded == expected, "Encoding should match: " + tradematch::fix::to_text(encoded));

    Message parsed;
    const auto result = tradematch::fix::parse(encoded + encoded, parsed);
    expect(result.status == ParseStatus::Complete && result.consumed == encoded.size() && parsed == heartbeat,
           "A message should parse back to the same fields, consuming exactly its own bytes.");
    expect(parsed.type() == "0" && parsed.get_int(tag::MsgSeqNum) == 2 && !parsed.get(tag::Text),
           "Fields should be readable by tag.");

    for (std::size_t size = 0; size < encoded.size(); ++size) {
        expect(
            tradematch::fix::parse(std::string_view{encoded}.substr(0, size), parsed).status == ParseStatus::Incomplete,
            "Every prefix of a message should be incomplete, not invalid.");
    }

    bool threw = false;
    try {
        Message bad(tradematch::fix::msg_type::Heartbeat);
        bad.set(tag::Text, std::string{"a"} + tradematch::fix::kSoh);
        tradematch::fix::encode(bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "Values can't contain the field separator.");
}

void test_broken_framing_is_rejected() {
    const std::string valid = soh("8=FIX.4.2|9=5|35=0|10=161|");
    Message parsed;
    expect(tradematch::fix::parse(valid, parsed).status == ParseStatus::Complete, "The sample should be valid.");
    const std::vector<std::string> broken{
        soh("8=FIX.4.4|9=5|35=0|10=163|"),        // another version
        soh("8=FIX.4.2|9=5|35=0|10=162|"),        // wrong checksum
        soh("8=FIX.4.2|9=4|35=0|10=161|"),        // body length too short
        soh("8=FIX.4.2|9=9999|35=0|"),            // body length too long
        soh("8=FIX.4.2|9=x|35=0|10=161|"),        // body length isn't a number
        soh("8=FIX.4.2|9=10|49=A|35=0|10=185|"),  // MsgType isn't first
        soh("8=FIX.4.2|9=9|35=0|58=|10=080|"),    // empty value
        soh("8=FIX.4.2|9=8|35=0|58|10=018|"),     // no equals sign
        soh("8=FIX.4.2|9=4|35=010=159|"),         // the body doesn't end with a field separator
        "GET / HTTP/1.1\r\n\r\n",
    };
    for (const std::string& bytes : broken) {
        expect(tradematch::fix::parse(bytes, parsed).status == ParseStatus::Invalid,
               "Broken framing should be invalid: " + tradematch::fix::to_text(bytes));
    }
}

void test_prices_convert_between_decimals_and_ticks() {
    expect(tradematch::parse_price("100") == 10'000 && tradematch::parse_price("100.5") == 10'050 &&
               tradematch::parse_price("100.25") == 10'025 && tradematch::parse_price("-1.5") == -150 &&
               tradematch::parse_price("0.01") == 1,
           "Decimal prices should convert to ticks.");
    for (const char* text : {"", "1.", ".5", "1.234", "1e5", "abc", "1.2.3", "99999999999999999"}) {
        expect(!tradematch::parse_price(text), std::string{"Not a price: "} + text);
    }
    expect(tradematch::fix::format_price(10'050) == "100.50", "Ticks should format as a decimal.");
}

void test_sessions_follow_the_protocol() {
    GatewayCore core({});
    Client client(core, "CLIENT");
    const auto logon = client.logon(5);
    expect(logon.size() == 1U && logon[0].type() == "A" && field(logon[0], tag::HeartBtInt) == "5" &&
               field(logon[0], tag::TargetCompID) == "CLIENT",
           "A Logon should be answered with a Logon.");

    Message test_request = client.message(tradematch::fix::msg_type::TestRequest);
    test_request.set(tag::TestReqID, "PING-1");
    const auto heartbeat = client.send(test_request);
    expect(heartbeat.size() == 1U && heartbeat[0].type() == "0" && field(heartbeat[0], tag::TestReqID) == "PING-1",
           "A TestRequest should get a Heartbeat that echoes its TestReqID.");

    core.on_timer(kStart + std::chrono::seconds{6});
    const auto idle = client.receive();
    expect(idle.size() == 1U && idle[0].type() == "0", "An idle session should be sent a Heartbeat.");
    core.on_timer(kStart + std::chrono::seconds{12});
    const auto dropped = client.receive();
    expect(dropped.size() == 1U && dropped[0].type() == "5" && core.closing(client.session()),
           "A session that stays silent for two heartbeat intervals should be logged out.");

    Client second(core, "SECOND");
    second.logon();
    const auto logout = second.send(second.message(tradematch::fix::msg_type::Logout));
    expect(logout.size() == 1U && logout[0].type() == "5" && core.closing(second.session()),
           "A Logout should be answered with a Logout.");
}

void test_session_violations_end_the_session() {
    GatewayCore core({});
    Client early(core, "EARLY");
    const auto not_logged_on = early.send(early.order("O1", "1", 10, "2", "100"));
    expect(not_logged_on.size() == 1U && not_logged_on[0].type() == "5" && core.closing(early.session()),
           "Anything before a Logon should end the session.");

    Client stranger(core, "STRANGER");
    Message logon = stranger.message(tradematch::fix::msg_type::Logon);
    logon.set(tag::TargetCompID, "ELSEWHERE").set_int(tag::HeartBtInt, 30);
    expect(stranger.send(logon).at(0).type() == "5", "A Logon for another CompID should be refused.");

    for (const int heartbeat : {-1, 3'601}) {
        Client client(core, "HEARTBEAT" + std::to_string(heartbeat));
        expect(client.logon(heartbeat).at(0).type() == "5" && core.closing(client.session()),
               "A HeartBtInt outside 0 to 3600 seconds should be refused.");
    }
    Client unnumbered(core, "UNNUMBERED");
    Message zero = unnumbered.message(tradematch::fix::msg_type::Logon);
    zero.set(tag::MsgSeqNum, "0").set_int(tag::HeartBtInt, 30);
    expect(unnumbered.send(zero).at(0).type() == "5", "A MsgSeqNum of 0 should end the session.");

    Client first(core, "TWIN");
    Client twin(core, "TWIN");
    first.logon();
    expect(twin.logon().at(0).type() == "5", "The same CompID can't log on twice.");

    Client gap(core, "GAP");
    gap.logon();
    gap.message(tradematch::fix::msg_type::Heartbeat);  // never sent, so the next sequence number skips one
    const auto skipped = gap.send(gap.message(tradematch::fix::msg_type::Heartbeat));
    expect(skipped.size() == 1U && skipped[0].type() == "5" &&
               field(skipped[0], tag::Text).find("2 expected") != std::string::npos,
           "A sequence gap should end the session: " + field(skipped.at(0), tag::Text));

    Client garbled(core, "GARBLED");
    garbled.logon();
    core.receive(garbled.session(),
                 "8=FIX.4.2\x01"
                 "9=5\x01"
                 "35=0\x01"
                 "10=000\x01",
                 kStart);
    expect(garbled.receive().at(0).type() == "5" && core.closing(garbled.session()),
           "Broken framing should end the session.");

    Client rude(core, "RUDE");
    rude.logon();
    const auto unsupported = rude.send(rude.message("V"));
    expect(unsupported.size() == 1U && unsupported[0].type() == "3" &&
               field(unsupported[0], tag::SessionRejectReason) == "11",
           "Unsupported messages should get a session-level Reject.");
    Message incomplete = rude.message(tradematch::fix::msg_type::NewOrderSingle);
    incomplete.set(tag::ClOrdID, "O2");
    const auto missing = rude.send(incomplete);
    expect(missing.size() == 1U && missing[0].type() == "3" && field(missing[0], tag::RefTagID) == "55",
           "A missing required tag should be named in a Reject.");
}

void test_orders_trade_across_sessions() {
    GatewayCore core({});
    Client seller(core, "SELLER");
    Client buyer(core, "BUYER");
    seller.logon();
    buyer.logon();

    expect(reports(seller.send(seller.order("S1", "2", 100, "2", "100.50"))) == "0/0", "A resting order is acked.");
    const auto bought = buyer.send(buyer.order("B1", "1", 60, "2", "100.50"));
    expect(reports(bought) == "0/0 2/2" && field(bought[1], tag::LastShares) == "60" &&
               field(bought[1], tag::LastPx) == "100.50" && field(bought[1], tag::LeavesQty) == "0" &&
               field(bought[1], tag::AvgPx) == "100.5000",
           "The aggressor should be acked, then filled: " + reports(bought));
    const auto sold = seller.receive();
    expect(reports(sold) == "1/1" && field(sold[0], tag::CumQty) == "60" && field(sold[0], tag::LeavesQty) == "40" &&
               field(sold[0], tag::ClOrdID) == "S1",
           "The resting side should get a partial fill on its own session.");

    // Replace: OrderQty is the new total, so 70 leaves 10 open after the 60 already filled.
    Message replace = seller.message(tradematch::fix::msg_type::OrderCancelReplaceRequest);
    replace.set(tag::OrigClOrdID, "S1").set(tag::ClOrdID, "S2").set(tag::Symbol, "ACME").set(tag::Side, "2");
    replace.set_int(tag::OrderQty, 70).set(tag::OrdType, "2").set(tag::Price, "100.50");
    const auto replaced = seller.send(replace);
    expect(reports(replaced) == "5/1" && field(replaced[0], tag::LeavesQty) == "10" &&
               field(replaced[0], tag::OrigClOrdID) == "S1" && field(replaced[0], tag::ClOrdID) == "S2",
           "A replace should be confirmed with the new ClOrdID: " + reports(replaced));

    Message cancel = seller.message(tradematch::fix::msg_type::OrderCancelRequest);
    cancel.set(tag::OrigClOrdID, "S2").set(tag::ClOrdID, "S3").set(tag::Symbol, "ACME").set(tag::Side, "2");
    const auto cancelled = seller.send(cancel);
    expect(reports(cancelled) == "4/4" && field(cancelled[0], tag::CumQty) == "60",
           "A cancel should be confirmed: " + reports(cancelled));
    Message cancel_again = seller.message(tradematch::fix::msg_type::OrderCancelRequest);
    cancel_again.set(tag::OrigClOrdID, "S2").set(tag::ClOrdID, "S4").set(tag::Symbol, "ACME").set(tag::Side, "2");
    const auto again = seller.send(cancel_again);
    expect(again.size() == 1U && again[0].type() == "9" && field(again[0], tag::CxlRejReason) == "1",
           "Cancelling an order that is gone should get an OrderCancelReject.");
    expect(core.engine().book(0).order_count() == 0U && core.stats().trades == 1U, "The book should be empty.");
}

void test_time_in_force_and_rejects() {
    GatewayCore core({});
    Client trader(core, "TRADER");
    Client other(core, "OTHER");
    trader.logon();
    other.logon();
    other.send(other.order("A1", "2", 50, "2", "101"));

    expect(reports(trader.send(trader.order("I1", "1", 80, "2", "101", "3"))) == "0/0 1/1 4/4",
           "An IOC should fill what it can, then cancel the rest.");
    other.send(other.order("A2", "2", 30, "2", "102"));
    expect(reports(trader.send(trader.order("F1", "1", 40, "2", "102", "4"))) == "0/0 4/4",
           "A FOK that can't fill completely should be cancelled.");
    expect(reports(trader.send(trader.order("M1", "1", 50, "1", ""))) == "0/0 1/1 4/4",
           "A market order should take what is there and cancel the rest.");

    trader.send(trader.order("R1", "1", 10, "2", "95"));
    Message post_only = trader.order("P1", "2", 10, "2", "90");  // built after R1 is sent, to keep sequence order
    post_only.set(tag::ExecInst, "6");
    const auto rejected = trader.send(post_only);
    expect(reports(rejected) == "8/8" && field(rejected[0], tag::Text) == "POST_ONLY_WOULD_CROSS",
           "A post-only order that would trade should be rejected.");
    Message unknown = trader.order("U1", "1", 10, "2", "100");
    unknown.set(tag::Symbol, "GLOBEX");
    const auto unknown_symbol = trader.send(unknown);
    expect(reports(unknown_symbol) == "8/8" && field(unknown_symbol[0], tag::OrdRejReason) == "1",
           "An unknown symbol should be rejected with OrdRejReason 1.");
    const auto duplicate = trader.send(trader.order("R1", "1", 5, "2", "94"));
    expect(reports(duplicate) == "8/8" && field(duplicate[0], tag::OrdRejReason) == "6",
           "Reusing an open order's ClOrdID should be rejected.");
}

void test_stops_report_to_their_owner() {
    GatewayCore core({});
    Client maker(core, "MAKER");
    Client stopper(core, "STOPPER");
    Client taker(core, "TAKER");
    maker.logon();
    stopper.logon();
    taker.logon();
    maker.send(maker.order("A1", "2", 10, "2", "100"));
    maker.send(maker.order("A2", "2", 10, "2", "100.10"));
    Message stop = stopper.order("ST1", "1", 30, "3", "");
    stop.set(tag::StopPx, "100");
    expect(reports(stopper.send(stop)) == "0/0", "A stop order should be acked while it waits.");

    // A trade at 100 triggers the stop, which buys 15 at market against the 15 left, and the rest expires.
    taker.send(taker.order("T1", "1", 5, "2", "100"));
    const auto stop_reports = stopper.receive();
    expect(reports(stop_reports) == "1/1 1/1 4/4" && field(stop_reports[1], tag::CumQty) == "15",
           "The triggered stop's fills, and the cancel of its remainder, go to its owner: " + reports(stop_reports));
    expect(reports(maker.receive()) == "1/1 2/2 2/2", "The maker should see every fill.");
}

void test_session_timers() {
    GatewayCore core({});
    Client silent(core, "SILENT");  // connects, but never logs on
    core.on_timer(kStart + std::chrono::seconds{9});
    expect(!core.closing(silent.session()), "A new connection has ten seconds to log on.");
    core.on_timer(kStart + std::chrono::seconds{11});
    const auto dropped = silent.receive();
    expect(dropped.size() == 1U && dropped[0].type() == "5" && core.closing(silent.session()),
           "A connection that never logs on should be dropped.");

    Client quiet(core, "QUIET");
    quiet.logon(0);
    core.on_timer(kStart + std::chrono::hours{1});
    expect(quiet.receive().empty() && !core.closing(quiet.session()),
           "A HeartBtInt of 0 should turn heartbeats and the silence timeout off.");
}

void test_new_orders_are_validated() {
    GatewayCore core({});
    Client trader(core, "TRADER");
    trader.logon();
    // FIX 4.2 has no "other" OrdRejReason, so every reject without a code of its own uses 0, broker option.
    const auto expect_rejected = [&](const Message& order, std::string_view text) {
        const auto replies = trader.send(order);
        expect(reports(replies) == "8/8" && field(replies.at(0), tag::OrdRejReason) == "0" &&
                   field(replies.at(0), tag::Text).find(text) != std::string::npos,
               "Expected a reject naming " + std::string{text} + ", got " + reports(replies) + ": " +
                   field(replies.at(0), tag::Text));
    };
    expect_rejected(trader.order("L1", "1", 10, "2", ""), "limit price");
    expect_rejected(trader.order("S1", "1", 10, "3", ""), "stop price");
    expect_rejected(trader.order("X1", "7", 10, "2", "100"), "Side");
    expect_rejected(trader.order("Q1", "1", 0, "2", "100"), "OrderQty");
    expect_rejected(trader.order("T1", "1", 10, "2", "100", "6"), "TimeInForce");  // good till date
    Message no_floor = trader.order("F1", "2", 10, "2", "100");
    no_floor.set(tag::MaxFloor, "0");
    expect_rejected(no_floor, "MaxFloor");

    // The engine's own checks, which the reject names.
    expect_rejected(trader.order("Z1", "1", 10, "2", "0"), "INVALID_PRICE");
    Message high_floor = trader.order("F2", "2", 10, "2", "100");
    high_floor.set_int(tag::MaxFloor, 20);
    expect_rejected(high_floor, "INVALID_DISPLAY_QUANTITY");
    Message post_only_ioc = trader.order("P1", "2", 10, "2", "100", "3");
    post_only_ioc.set(tag::ExecInst, "6");
    expect_rejected(post_only_ioc, "INVALID_POST_ONLY");
    expect(core.stats().order_rejects == 9U && core.stats().orders == 0U && core.engine().book(0).order_count() == 0U,
           "Rejected orders should never reach the book.");
}

void test_icebergs_show_one_tranche_at_a_time() {
    GatewayCore core({});
    Client maker(core, "MAKER");
    Client taker(core, "TAKER");
    maker.logon();
    taker.logon();
    Message iceberg = maker.order("I1", "2", 100, "2", "100");
    iceberg.set_int(tag::MaxFloor, 10);
    expect(reports(maker.send(iceberg)) == "0/0", "An iceberg should be acked.");
    expect(core.engine().book(0).snapshot().asks.at(0).total_quantity == 10U, "Only MaxFloor should show.");

    // The buy takes the tranche on show, then each tranche that replaces it.
    const auto bought = taker.send(taker.order("B1", "1", 25, "2", "100"));
    expect(reports(bought) == "0/0 1/1 1/1 2/2", "The buy should fill across three tranches: " + reports(bought));
    const auto sold = maker.receive();
    expect(reports(sold) == "1/1 1/1 1/1" && field(sold.at(2), tag::CumQty) == "25" &&
               field(sold.at(2), tag::LeavesQty) == "75",
           "The iceberg should be filled tranche by tranche: " + reports(sold));
    expect(core.engine().book(0).snapshot().asks.at(0).total_quantity == 5U, "What is left of a tranche shows.");

    // A smaller size at the same price is amended in place, keeping its place in the queue and the tranche on show.
    expect(reports(maker.send(maker.replace("I1", "I2", "2", 60, "100"))) == "5/1", "The iceberg should be replaced.");
    expect(core.engine().book(0).snapshot().asks.at(0).total_quantity == 5U, "An amend keeps the tranche on show.");
    // A new price re-enters the order, and it still shows only MaxFloor of the 35 left.
    expect(reports(maker.send(maker.replace("I2", "I3", "2", 60, "100.10"))) == "5/1", "The price should change.");
    const auto repriced = core.engine().book(0).snapshot().asks.at(0);
    expect(repriced.price == 10'010 && repriced.total_quantity == 10U, "The repriced iceberg should keep MaxFloor.");
}

void test_replaces_and_cancels_are_checked() {
    GatewayCore core({});
    Client trader(core, "TRADER");
    Client other(core, "OTHER");
    trader.logon();
    other.logon();
    trader.send(trader.order("R1", "2", 50, "2", "101"));
    trader.send(trader.order("R2", "2", 10, "2", "105"));
    other.send(other.order("B1", "1", 20, "2", "101"));
    expect(reports(trader.receive()) == "1/1", "R1 should be partly filled.");

    // A refused request gets an OrderCancelReject and leaves the order as it was.
    const auto refused = [&](const Message& request, std::string_view reason, std::string_view text) {
        const auto replies = trader.send(request);
        expect(replies.size() == 1U && replies[0].type() == "9" && field(replies[0], tag::CxlRejReason) == reason &&
                   field(replies[0], tag::Text).find(text) != std::string::npos,
               "Expected an OrderCancelReject naming " + std::string{text} + ", got " + reports(replies));
        return replies[0];
    };
    const Message too_small = refused(trader.replace("R1", "R3", "2", 20, "101"), "2", "above the filled quantity");
    expect(field(too_small, tag::OrdStatus) == "1" && field(too_small, tag::CxlRejResponseTo) == "2",
           "The reject should answer the replace and show the order as partly filled.");
    refused(trader.replace("R1", "R3", "2", 40, ""), "2", "limit price");
    Message to_ioc = trader.replace("R1", "R3", "2", 40, "101");
    to_ioc.set(tag::TimeInForce, "3");
    refused(to_ioc, "2", "INVALID_TIME_IN_FORCE");
    refused(trader.replace("R1", "R2", "2", 40, "101"), "2", "already in use");
    refused(trader.cancel("R1", "R2", "2"), "2", "already in use");
    const Message unknown = refused(trader.cancel("R9", "R3", "2"), "1", "unknown order");
    expect(field(unknown, tag::OrdStatus) == "8" && field(unknown, tag::CxlRejResponseTo) == "1",
           "An unknown order should be reported as such.");

    expect(reports(trader.send(trader.replace("R1", "R3", "2", 40, "101"))) == "5/1", "A valid replace goes through.");
    expect(reports(trader.send(trader.cancel("R2", "R4", "2"))) == "4/4", "R2 should be untouched by the refusals.");
    const auto cancelled = trader.send(trader.cancel("R3", "R5", "2"));
    expect(reports(cancelled) == "4/4" && field(cancelled[0], tag::CumQty) == "20",
           "The replaced order should be cancelled by its new ClOrdID.");
    expect(core.engine().book(0).order_count() == 0U, "The book should be empty.");
}

void test_orders_belong_to_their_comp_id() {
    GatewayCore core({});
    Client seller(core, "SELLER");
    seller.logon();
    seller.send(seller.order("S1", "2", 100, "2", "100"));
    core.close_session(seller.session());  // the connection drops, without a Logout

    Client buyer(core, "BUYER");
    buyer.logon();
    expect(reports(buyer.send(buyer.order("B1", "1", 30, "2", "100"))) == "0/0 2/2",
           "A disconnected client's order should still trade.");
    const auto foreign = buyer.send(buyer.cancel("S1", "B2", "2"));
    expect(foreign.size() == 1U && field(foreign[0], tag::CxlRejReason) == "1",
           "Another CompID can't cancel the order.");

    // Logging back on starts a new session, numbered from 1. The fill report sent while the client was away is lost,
    // since resends aren't supported, but the order is still the client's.
    Client returning(core, "SELLER");
    expect(returning.logon().at(0).type() == "A", "The client should be able to log back on.");
    const auto cancelled = returning.send(returning.cancel("S1", "S2", "2"));
    expect(reports(cancelled) == "4/4" && field(cancelled[0], tag::CumQty) == "30",
           "The client should be able to cancel the order after reconnecting: " + reports(cancelled));
}

void test_sockets_report_failures() {
    const tradematch::detail::SocketLibrary library;
    const auto throws = [](const std::function<void()>& action) {
        try {
            action();
        } catch (const std::system_error&) {
            return true;
        }
        return false;
    };
    const auto listener = tradematch::detail::Socket::listen(0, true);
    expect(throws([&] { static_cast<void>(tradematch::detail::Socket::listen(listener.local_port(), true)); }),
           "A port that is being listened on can't be taken over.");
    std::uint16_t closed_port = 0;
    {
        const auto probe = tradematch::detail::Socket::listen(0, true);
        closed_port = probe.local_port();
    }
    expect(throws([&] { static_cast<void>(tradematch::detail::Socket::connect("127.0.0.1", closed_port)); }),
           "Connecting to a port nobody listens on should fail.");
}

void send_bytes(const tradematch::detail::Socket& socket, std::string_view bytes) {
    expect(socket.send(bytes) == static_cast<std::ptrdiff_t>(bytes.size()), "The gateway should accept the bytes.");
}

// Reads until `count` complete messages have arrived.
std::vector<Message> read_messages(const tradematch::detail::Socket& socket, std::size_t count) {
    std::string buffer;
    std::vector<Message> messages;
    std::array<char, 4'096> chunk{};
    while (messages.size() < count) {
        const auto received = socket.receive(chunk);
        expect(received > 0, "The gateway should answer within the timeout.");
        buffer.append(chunk.data(), static_cast<std::size_t>(received));
        Message message;
        for (auto result = tradematch::fix::parse(buffer, message); result.status == ParseStatus::Complete;
             result = tradematch::fix::parse(buffer, message)) {
            messages.push_back(message);
            buffer.erase(0, result.consumed);
        }
    }
    return messages;
}

std::string logon_bytes(Client& script) {
    Message logon = script.message(tradematch::fix::msg_type::Logon);
    logon.set_int(tag::HeartBtInt, 30);
    return tradematch::fix::encode(logon);
}

void test_gateway_serves_clients_over_tcp() {
    tradematch::fix::Gateway gateway({});
    std::atomic<bool> stop{false};
    std::thread server([&] { gateway.run(stop); });

    const auto connect = [&](const std::string& comp_id) {
        auto socket = tradematch::detail::Socket::connect("127.0.0.1", gateway.port());
        socket.set_no_delay();
        socket.set_receive_timeout(5'000);
        return std::pair{std::move(socket), comp_id};
    };

    std::string failure;
    try {
        const tradematch::detail::SocketLibrary library;
        auto [seller, seller_id] = connect("SELLER");
        auto [buyer, buyer_id] = connect("BUYER");
        GatewayCore scripts({});  // only used to build messages with the right headers
        Client seller_script(scripts, seller_id);
        Client buyer_script(scripts, buyer_id);
        send_bytes(seller, logon_bytes(seller_script));
        send_bytes(buyer, logon_bytes(buyer_script));
        expect(read_messages(seller, 1).at(0).type() == "A" && read_messages(buyer, 1).at(0).type() == "A",
               "Both should log on.");

        send_bytes(seller, tradematch::fix::encode(seller_script.order("S1", "2", 100, "2", "99.75")));
        expect(reports(read_messages(seller, 1)) == "0/0", "The sell should be acked over TCP.");
        send_bytes(buyer, tradematch::fix::encode(buyer_script.order("B1", "1", 100, "2", "99.75")));
        expect(reports(read_messages(buyer, 2)) == "0/0 2/2", "The buy should fill over TCP.");
        expect(reports(read_messages(seller, 1)) == "2/2", "The seller should hear about the fill over TCP.");
    } catch (const std::exception& error) {
        failure = error.what();
    }
    stop = true;
    server.join();
    expect(failure.empty(), failure);
    expect(gateway.core().stats().trades == 1U && gateway.core().stats().sessions == 2U,
           "The gateway should count the sessions and the trade.");
}

// Without run(), the caller's own loop drives the gateway through poll_once().
void test_gateway_can_be_polled_by_its_owner() {
    tradematch::fix::Gateway gateway({});
    const tradematch::detail::SocketLibrary library;
    const auto socket = tradematch::detail::Socket::connect("127.0.0.1", gateway.port());
    socket.set_receive_timeout(5'000);
    const auto poll_until = [&](const std::function<bool()>& done) {
        for (int round = 0; round < 500 && !done(); ++round) {
            gateway.poll_once(std::chrono::milliseconds{10});
        }
        expect(done(), "The gateway should make progress as it is polled.");
    };
    GatewayCore scripts({});
    Client script(scripts, "POLLER");

    send_bytes(socket, logon_bytes(script));
    poll_until([&] { return gateway.core().stats().sessions == 1U; });
    expect(read_messages(socket, 1).at(0).type() == "A", "The gateway should answer the Logon.");
    send_bytes(socket, tradematch::fix::encode(script.message(tradematch::fix::msg_type::Logout)));
    poll_until([&] { return gateway.core().stats().messages_received == 2U; });
    expect(read_messages(socket, 1).at(0).type() == "5", "A Logout should be answered with a Logout.");
    std::array<char, 16> rest{};
    expect(socket.receive(rest) < 0, "The gateway should close the connection after the Logout.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"encoding_matches_the_standard", test_encoding_matches_the_standard},
        {"broken_framing_is_rejected", test_broken_framing_is_rejected},
        {"prices_convert_between_decimals_and_ticks", test_prices_convert_between_decimals_and_ticks},
        {"sessions_follow_the_protocol", test_sessions_follow_the_protocol},
        {"session_violations_end_the_session", test_session_violations_end_the_session},
        {"orders_trade_across_sessions", test_orders_trade_across_sessions},
        {"time_in_force_and_rejects", test_time_in_force_and_rejects},
        {"stops_report_to_their_owner", test_stops_report_to_their_owner},
        {"session_timers", test_session_timers},
        {"new_orders_are_validated", test_new_orders_are_validated},
        {"icebergs_show_one_tranche_at_a_time", test_icebergs_show_one_tranche_at_a_time},
        {"replaces_and_cancels_are_checked", test_replaces_and_cancels_are_checked},
        {"orders_belong_to_their_comp_id", test_orders_belong_to_their_comp_id},
        {"sockets_report_failures", test_sockets_report_failures},
        {"gateway_serves_clients_over_tcp", test_gateway_serves_clients_over_tcp},
        {"gateway_can_be_polled_by_its_owner", test_gateway_can_be_polled_by_its_owner},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures == 0U) {
        std::cout << "All FIX tests passed (" << tests.size() << ").\n";
        return 0;
    }
    std::cout << failures << " test(s) failed.\n";
    return 1;
}

}  // namespace

int main() {
    try {
        return run_all_tests();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
