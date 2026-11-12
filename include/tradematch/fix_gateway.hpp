#ifndef TRADEMATCH_FIX_GATEWAY_HPP
#define TRADEMATCH_FIX_GATEWAY_HPP

#include "tradematch/fix.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/order_book.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tradematch::fix {

struct GatewayConfig {
    std::vector<std::string> symbols{"ACME"};
    BookCapacity capacity;
    std::string comp_id{"TRADEMATCH"};  // the gateway's CompID, which clients send as TargetCompID
    std::uint16_t port{0};              // 0 picks a free port
    bool loopback_only{true};           // accept connections from this machine only
};

struct GatewayStats {
    std::uint64_t sessions{0};  // sessions that logged on
    std::uint64_t messages_received{0};
    std::uint64_t messages_sent{0};
    std::uint64_t orders{0};         // new orders the engine accepted
    std::uint64_t order_rejects{0};  // new orders, cancels, and replaces that were rejected
    std::uint64_t trades{0};
    std::uint64_t session_rejects{0};  // messages that broke the session rules
};

using SessionId = std::uint64_t;
using Clock = std::chrono::system_clock;

// The gateway's protocol and order handling, with no sockets involved, so it can be tested and fuzzed directly. A
// session's bytes go in through receive(); the messages each session should be sent come out of take_output().
//
// Session layer: a Logon must come first, with the gateway's CompID as TargetCompID and a HeartBtInt, and each CompID
// can have one session at a time. Sequence numbers must arrive in order; a gap ends the session, since resending isn't
// supported. TestRequests get Heartbeats, idle sessions are sent Heartbeats, silent ones are dropped, and a Logout is
// answered with a Logout.
//
// Application layer: NewOrderSingle (limit, market, stop, and stop-limit orders; DAY, GTC, IOC, and FOK; post-only
// through ExecInst 6; icebergs through MaxFloor), OrderCancelRequest, and OrderCancelReplaceRequest, where OrderQty is
// the new total quantity including fills. Every trade sends a fill report to both sides, whichever session they are on.
// Orders belong to the CompID that sent them, not to a connection: they stay in the book when a session ends, and the
// CompID can cancel or replace them after logging back on. Reports for a CompID that isn't connected are lost.
class GatewayCore {
public:
    explicit GatewayCore(const GatewayConfig& config);
    GatewayCore(const GatewayCore&) = delete;
    GatewayCore& operator=(const GatewayCore&) = delete;
    GatewayCore(GatewayCore&&) = delete;
    GatewayCore& operator=(GatewayCore&&) = delete;
    ~GatewayCore();

    SessionId open_session(Clock::time_point now);
    void receive(SessionId session, std::string_view bytes, Clock::time_point now);
    // Sends Heartbeats to quiet sessions and ends sessions that have gone silent or never logged on.
    void on_timer(Clock::time_point now);

    // The encoded messages waiting for the session, which are removed.
    std::string take_output(SessionId session);
    std::vector<SessionId> sessions_with_output() const;
    // Whether the session should be disconnected once its output has been sent.
    bool closing(SessionId session) const;
    void close_session(SessionId session);

    const MatchingEngine& engine() const noexcept;
    const GatewayStats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A FIX 4.2 order-entry gateway: a TCP server running GatewayCore on one thread with poll().
class Gateway {
public:
    explicit Gateway(const GatewayConfig& config);
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;
    ~Gateway();

    std::uint16_t port() const noexcept;

    // Waits up to `timeout` for network activity and handles all of it.
    void poll_once(std::chrono::milliseconds timeout);
    // Polls until `stop` becomes true.
    void run(const std::atomic<bool>& stop);

    const GatewayCore& core() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tradematch::fix

#endif
