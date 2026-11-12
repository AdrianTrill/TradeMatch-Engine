// A FIX 4.2 client for tradematch_fix_gateway.
//
//   tradematch_fix_client [--host H] [--port N] [--sender ID]   an interactive order ticket
//   tradematch_fix_client --bench N [--port N]                   order round-trip latency; without --port it starts
//                                                                 its own gateway on a free port and also measures
//                                                                 the same exchange without FIX, as a floor
#include "tradematch/detail/socket.hpp"
#include "tradematch/fix.hpp"
#include "tradematch/fix_gateway.hpp"
#include "tradematch/latency_histogram.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fix = tradematch::fix;
namespace tag = tradematch::fix::tag;
using fix::Message;

// Sends all of the bytes, waiting out a full socket buffer; false if the connection is gone.
bool send_all(const tradematch::detail::Socket& socket, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto sent = socket.send(bytes);
        if (sent < 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

// One FIX session over a blocking socket.
class Connection {
public:
    Connection(tradematch::detail::Socket socket, std::string sender, std::string target)
        : socket_(std::move(socket)), sender_(std::move(sender)), target_(std::move(target)) {
        socket_.set_no_delay();
    }

    Message make(std::string_view type) const {
        Message message(type);
        message.set(tag::SenderCompID, sender_);
        message.set(tag::TargetCompID, target_);
        message.set_int(tag::MsgSeqNum, 0);  // assigned by send()
        message.set(tag::SendingTime, fix::utc_timestamp(std::chrono::system_clock::now()));
        return message;
    }

    // Returns the size of the encoded message.
    std::size_t send(Message message) {
        message.set_int(tag::MsgSeqNum, sequence_++);
        const std::string bytes = fix::encode(message);
        if (!send_all(socket_, bytes)) {
            throw std::runtime_error("the gateway closed the connection");
        }
        return bytes.size();
    }

    // Blocks until a whole message arrives; returns nothing once the connection is closed.
    std::optional<Message> receive() {
        Message message;
        while (true) {
            const auto result = fix::parse(buffer_, message);
            if (result.status == fix::ParseStatus::Complete) {
                buffer_.erase(0, result.consumed);
                last_received_size_ = result.consumed;
                return message;
            }
            if (result.status == fix::ParseStatus::Invalid) {
                throw std::runtime_error("the gateway sent an invalid message: " + result.error);
            }
            const auto received = socket_.receive(chunk_);
            if (received < 0) {
                return std::nullopt;
            }
            buffer_.append(chunk_.data(), static_cast<std::size_t>(received));
        }
    }

    // The size of the message receive() last returned, as it arrived.
    std::size_t last_received_size() const noexcept {
        return last_received_size_;
    }

    void logon(int heartbeat_seconds) {
        Message logon = make(fix::msg_type::Logon);
        logon.set(tag::EncryptMethod, "0");
        logon.set_int(tag::HeartBtInt, heartbeat_seconds);
        send(logon);
        const auto reply = receive();
        if (!reply || reply->type() != fix::msg_type::Logon) {
            throw std::runtime_error("logon refused: " +
                                     std::string{reply ? reply->get(tag::Text).value_or("") : "disconnected"});
        }
    }

private:
    tradematch::detail::Socket socket_;
    std::string sender_;
    std::string target_;
    std::int64_t sequence_{1};
    std::string buffer_;
    std::size_t last_received_size_{0};
    std::array<char, 65'536> chunk_{};
};

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

std::string field(const Message& message, int field_tag) {
    return std::string{message.get(field_tag).value_or("")};
}

// One line per received message.
std::string describe(const Message& message) {
    const std::string_view type = message.type();
    if (type == fix::msg_type::ExecutionReport) {
        const std::string exec_type = field(message, tag::ExecType);
        std::ostringstream text;
        text << field(message, tag::ClOrdID) << ' ';
        if (exec_type == "0") {
            text << "NEW " << (field(message, tag::Side) == "1" ? "BUY " : "SELL ") << field(message, tag::OrderQty)
                 << ' ' << field(message, tag::Symbol) << ", open " << field(message, tag::LeavesQty);
        } else if (exec_type == "1" || exec_type == "2") {
            text << (exec_type == "2" ? "FILLED " : "PARTIAL ") << field(message, tag::LastShares) << " @ "
                 << field(message, tag::LastPx) << ", filled " << field(message, tag::CumQty) << ", open "
                 << field(message, tag::LeavesQty) << ", average " << field(message, tag::AvgPx);
        } else if (exec_type == "4") {
            text << "CANCELED, filled " << field(message, tag::CumQty);
            if (message.get(tag::Text)) {
                text << " (" << field(message, tag::Text) << ')';
            }
        } else if (exec_type == "5") {
            text << "REPLACED (was " << field(message, tag::OrigClOrdID) << "), quantity "
                 << field(message, tag::OrderQty) << ", open " << field(message, tag::LeavesQty);
        } else if (exec_type == "8") {
            text << "REJECTED: " << field(message, tag::Text);
        } else {
            text << "ExecType " << exec_type;
        }
        return text.str();
    }
    if (type == fix::msg_type::OrderCancelReject) {
        return "request for " + field(message, tag::OrigClOrdID) + " rejected: " + field(message, tag::Text);
    }
    if (type == fix::msg_type::Reject) {
        return "session reject: " + field(message, tag::Text);
    }
    if (type == fix::msg_type::Logout) {
        return "logout" + (message.get(tag::Text) ? ": " + field(message, tag::Text) : std::string{});
    }
    if (type == fix::msg_type::Heartbeat) {
        return "heartbeat";
    }
    return "MsgType " + std::string{type};
}

struct OrderInfo {
    std::string symbol;
    std::string side;
};

void print_help() {
    std::cout << "  buy|sell SYMBOL QTY @ PRICE [day|gtc|ioc|fok] [post-only]\n"
                 "  buy|sell SYMBOL QTY market\n"
                 "  cancel CLORDID\n"
                 "  replace CLORDID TOTAL_QTY @ PRICE\n"
                 "  quit\n";
}

int interactive(const std::string& host, std::uint16_t port, const std::string& sender) {
    const tradematch::detail::SocketLibrary library;
    Connection connection(tradematch::detail::Socket::connect(host, port), sender, "TRADEMATCH");
    connection.logon(30);
    std::cout << "logged on to " << host << ':' << port << " as " << sender << ". Commands:\n";
    print_help();

    std::atomic<bool> connected{true};
    std::thread reader([&] {
        try {
            while (const auto message = connection.receive()) {
                std::cout << "<- " << describe(*message) << '\n' << std::flush;
            }
        } catch (const std::exception& error) {
            std::cout << "<- " << error.what() << '\n';
        }
        connected = false;
    });

    std::map<std::string, OrderInfo, std::less<>> orders;
    int next_id = 1;
    std::string line;
    while (connected && std::getline(std::cin, line)) {
        std::istringstream words(line);
        std::vector<std::string> tokens{std::istream_iterator<std::string>(words),
                                        std::istream_iterator<std::string>()};
        if (tokens.empty()) {
            continue;
        }
        const std::string command = lower(tokens[0]);
        try {
            if (command == "quit" || command == "exit") {
                break;
            }
            if ((command == "buy" || command == "sell") && tokens.size() >= 4U) {
                const std::string cl_ord_id = "C" + std::to_string(next_id++);
                Message order = connection.make(fix::msg_type::NewOrderSingle);
                order.set(tag::ClOrdID, cl_ord_id).set(tag::Symbol, tokens[1]);
                order.set(tag::Side, command == "buy" ? "1" : "2").set(tag::OrderQty, tokens[2]);
                const bool market = lower(tokens[3]) == "market";
                if (!market && (tokens[3] != "@" || tokens.size() < 5U)) {
                    throw std::invalid_argument("expected '@ PRICE' or 'market'");
                }
                order.set(tag::OrdType, market ? "1" : "2");
                if (!market) {
                    order.set(tag::Price, tokens[4]);
                }
                for (std::size_t index = market ? 4U : 5U; index < tokens.size(); ++index) {
                    const std::string option = lower(tokens[index]);
                    static const std::map<std::string, std::string, std::less<>> kTimeInForce{{"day", "0"},
                                                                                              {"gtc", "1"},
                                                                                              {"ioc", "3"},
                                                                                              {"fok", "4"}};
                    if (const auto found = kTimeInForce.find(option); found != kTimeInForce.end()) {
                        order.set(tag::TimeInForce, found->second);
                    } else if (option == "post-only") {
                        order.set(tag::ExecInst, "6");
                    } else {
                        throw std::invalid_argument("unknown option " + option);
                    }
                }
                orders[cl_ord_id] = {tokens[1], command == "buy" ? "1" : "2"};
                connection.send(order);
                std::cout << "-> " << cl_ord_id << ' ' << line << '\n';
            } else if (command == "cancel" && tokens.size() == 2U) {
                const auto found = orders.find(tokens[1]);
                const std::string cl_ord_id = "C" + std::to_string(next_id++);
                Message cancel = connection.make(fix::msg_type::OrderCancelRequest);
                cancel.set(tag::OrigClOrdID, tokens[1]).set(tag::ClOrdID, cl_ord_id);
                cancel.set(tag::Symbol, found == orders.end() ? "ACME" : found->second.symbol);
                cancel.set(tag::Side, found == orders.end() ? "1" : found->second.side);
                connection.send(cancel);
                std::cout << "-> " << cl_ord_id << " cancel " << tokens[1] << '\n';
            } else if (command == "replace" && tokens.size() == 5U && tokens[3] == "@") {
                const auto found = orders.find(tokens[1]);
                const std::string cl_ord_id = "C" + std::to_string(next_id++);
                Message replace = connection.make(fix::msg_type::OrderCancelReplaceRequest);
                replace.set(tag::OrigClOrdID, tokens[1]).set(tag::ClOrdID, cl_ord_id);
                replace.set(tag::Symbol, found == orders.end() ? "ACME" : found->second.symbol);
                replace.set(tag::Side, found == orders.end() ? "1" : found->second.side);
                replace.set(tag::OrderQty, tokens[2]).set(tag::OrdType, "2").set(tag::Price, tokens[4]);
                if (found != orders.end()) {
                    orders[cl_ord_id] = found->second;
                }
                connection.send(replace);
                std::cout << "-> " << cl_ord_id << " replace " << tokens[1] << '\n';
            } else {
                print_help();
            }
        } catch (const std::exception& error) {
            std::cout << "error: " << error.what() << '\n';
        }
    }
    if (connected) {
        connection.send(connection.make(fix::msg_type::Logout));
    }
    reader.join();
    return 0;
}

std::string format_ns(std::uint64_t nanoseconds) {
    std::ostringstream text;
    if (nanoseconds < 10'000) {
        text << nanoseconds << " ns";
    } else {
        text << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / 1e3 << " us";
    }
    return text.str();
}

void print_latencies(const tradematch::LatencyHistogram& latencies) {
    std::cout << "  p50 " << format_ns(latencies.percentile(50.0)) << "  p90 " << format_ns(latencies.percentile(90.0))
              << "  p99 " << format_ns(latencies.percentile(99.0)) << "  p99.9 "
              << format_ns(latencies.percentile(99.9)) << "  max " << format_ns(latencies.max()) << '\n';
}

// The floor under the FIX round trips: the same exchange as raw bytes, a request the size of an order answered with a
// reply the size of its acknowledgement. The answering thread polls a non-blocking socket the way the gateway does,
// but does no FIX work, so what remains is the cost of loopback TCP and of waking each thread.
tradematch::LatencyHistogram echo_round_trips(std::size_t request_size, std::size_t reply_size, std::uint64_t rounds) {
    namespace detail = tradematch::detail;
    const detail::Socket listener = detail::Socket::listen(0, true);
    const detail::Socket client = detail::Socket::connect("127.0.0.1", listener.local_port());
    const detail::Socket peer = listener.accept();  // already connected, so this doesn't wait
    client.set_no_delay();
    peer.set_nonblocking();
    peer.set_no_delay();

    std::atomic<bool> stop{false};
    std::thread echo([&] {
        const std::string reply(reply_size, 'x');
        std::array<char, 4096> buffer{};
        std::size_t unanswered = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::array entries{detail::PollEntry{peer.native(), POLLIN, 0}};
            detail::poll_sockets(entries, 50);
            for (auto received = peer.receive(buffer); received != 0; received = peer.receive(buffer)) {
                if (received < 0) {
                    return;
                }
                unanswered += static_cast<std::size_t>(received);
            }
            for (; unanswered >= request_size; unanswered -= request_size) {
                if (!send_all(peer, reply)) {
                    return;
                }
            }
        }
    });

    tradematch::LatencyHistogram latencies;
    std::string failure;
    const std::string request(request_size, 'x');
    std::array<char, 4096> buffer{};
    for (std::uint64_t round = 0; round < rounds && failure.empty(); ++round) {
        const auto sent = std::chrono::steady_clock::now();
        if (!send_all(client, request)) {
            failure = "the echo connection closed";
        }
        for (std::size_t received = 0; received < reply_size && failure.empty();) {
            const auto count = client.receive(buffer);
            if (count < 0) {
                failure = "the echo connection closed";
            }
            received += static_cast<std::size_t>(std::max<std::ptrdiff_t>(count, 0));
        }
        latencies.record(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sent).count()));
    }
    stop = true;
    echo.join();
    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }
    return latencies;
}

// Sends orders one at a time and times each from sending it to receiving its acknowledgement. Buys and sells
// alternate at one price, so every second order trades and the book stays small.
int benchmark(const std::string& host, std::optional<std::uint16_t> port, std::uint64_t orders) {
    const tradematch::detail::SocketLibrary library;
    std::optional<tradematch::fix::Gateway> local;
    std::atomic<bool> stop{false};
    std::thread server;
    if (!port) {
        local.emplace(tradematch::fix::GatewayConfig{});
        port = local->port();
        server = std::thread([&] { local->run(stop); });
    }

    tradematch::LatencyHistogram round_trips;
    std::chrono::steady_clock::duration elapsed{};
    std::size_t order_bytes = 0;
    std::size_t report_bytes = 0;
    std::string failure;
    try {
        Connection connection(tradematch::detail::Socket::connect(host, *port), "BENCH", "TRADEMATCH");
        connection.logon(30);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < orders; ++index) {
            const std::string cl_ord_id = "B" + std::to_string(index);
            Message order = connection.make(fix::msg_type::NewOrderSingle);
            order.set(tag::ClOrdID, cl_ord_id).set(tag::Symbol, "ACME").set(tag::Side, index % 2 == 0 ? "1" : "2");
            order.set(tag::OrderQty, "1").set(tag::OrdType, "2").set(tag::Price, "100.00").set(tag::TimeInForce, "1");
            const auto sent = std::chrono::steady_clock::now();
            order_bytes = connection.send(order);
            while (true) {
                const auto message = connection.receive();
                if (!message) {
                    throw std::runtime_error("the gateway disconnected");
                }
                if (message->type() == fix::msg_type::ExecutionReport && message->get(tag::ClOrdID) == cl_ord_id) {
                    report_bytes = connection.last_received_size();
                    break;  // the first report for an order is its acknowledgement
                }
            }
            round_trips.record(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sent).count()));
        }
        elapsed = std::chrono::steady_clock::now() - start;
        connection.send(connection.make(fix::msg_type::Logout));
    } catch (const std::exception& error) {
        failure = error.what();
    }
    stop = true;
    if (server.joinable()) {
        server.join();
    }
    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::cout << "FIX round trips: " << orders << " NewOrderSingle messages, each timed from sending it to receiving "
              << "its ExecutionReport over loopback TCP" << (local ? " (in-process gateway)" : "") << '\n';
    print_latencies(round_trips);
    if (seconds > 0.0) {
        std::cout << "  " << std::fixed << std::setprecision(0) << static_cast<double>(orders) / seconds
                  << " orders per second, one at a time\n";
    }
    if (local) {
        const auto floor = echo_round_trips(order_bytes, report_bytes, orders);
        std::cout << "Loopback floor: the same exchange as raw bytes (" << order_bytes << " out, " << report_bytes
                  << " back) with an echo thread in place of the gateway\n";
        print_latencies(floor);
    }
    return 0;
}

int run(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::optional<std::uint16_t> port;
    std::string sender = "CLIENT";
    std::optional<std::uint64_t> bench;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tradematch_fix_client [--host H] [--port N] [--sender ID]\n"
                         "       tradematch_fix_client --bench ORDERS [--port N]\n";
            return 0;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const std::string& value = args[++index];
        std::uint64_t number = 0;
        const bool numeric = std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc{};
        if (arg == "--host") {
            host = value;
        } else if (arg == "--port" && numeric && number <= 65'535U) {
            port = static_cast<std::uint16_t>(number);
        } else if (arg == "--sender") {
            sender = value;
        } else if (arg == "--bench" && numeric && number > 0U) {
            bench = number;
        } else {
            throw std::invalid_argument(std::string{"invalid option "}.append(arg).append(" ").append(value));
        }
    }
    if (bench) {
        return benchmark(host, port, *bench);
    }
    return interactive(host, port.value_or(9878), sender);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
