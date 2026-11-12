#ifndef TRADEMATCH_FIX_HPP
#define TRADEMATCH_FIX_HPP

#include "tradematch/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The FIX 4.2 tag=value encoding: fields separated by SOH (0x01), framed by BeginString and BodyLength in front and
// a CheckSum at the end.
namespace tradematch::fix {

inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString = "FIX.4.2";
inline constexpr std::size_t kMaxBodyLength = 4'096;

namespace tag {
inline constexpr int Account = 1;
inline constexpr int AvgPx = 6;
inline constexpr int BeginString = 8;
inline constexpr int BodyLength = 9;
inline constexpr int CheckSum = 10;
inline constexpr int ClOrdID = 11;
inline constexpr int CumQty = 14;
inline constexpr int ExecID = 17;
inline constexpr int ExecInst = 18;
inline constexpr int ExecTransType = 20;
inline constexpr int LastPx = 31;
inline constexpr int LastShares = 32;
inline constexpr int MsgSeqNum = 34;
inline constexpr int MsgType = 35;
inline constexpr int OrderID = 37;
inline constexpr int OrderQty = 38;
inline constexpr int OrdStatus = 39;
inline constexpr int OrdType = 40;
inline constexpr int OrigClOrdID = 41;
inline constexpr int Price = 44;
inline constexpr int RefSeqNum = 45;
inline constexpr int SenderCompID = 49;
inline constexpr int SendingTime = 52;
inline constexpr int Side = 54;
inline constexpr int Symbol = 55;
inline constexpr int TargetCompID = 56;
inline constexpr int Text = 58;
inline constexpr int TimeInForce = 59;
inline constexpr int EncryptMethod = 98;
inline constexpr int StopPx = 99;
inline constexpr int CxlRejReason = 102;
inline constexpr int OrdRejReason = 103;
inline constexpr int HeartBtInt = 108;
inline constexpr int MaxFloor = 111;
inline constexpr int TestReqID = 112;
inline constexpr int ExecType = 150;
inline constexpr int LeavesQty = 151;
inline constexpr int RefTagID = 371;
inline constexpr int SessionRejectReason = 373;
inline constexpr int CxlRejResponseTo = 434;
}  // namespace tag

namespace msg_type {
inline constexpr std::string_view Heartbeat = "0";
inline constexpr std::string_view TestRequest = "1";
inline constexpr std::string_view Reject = "3";
inline constexpr std::string_view Logout = "5";
inline constexpr std::string_view ExecutionReport = "8";
inline constexpr std::string_view OrderCancelReject = "9";
inline constexpr std::string_view Logon = "A";
inline constexpr std::string_view NewOrderSingle = "D";
inline constexpr std::string_view OrderCancelRequest = "F";
inline constexpr std::string_view OrderCancelReplaceRequest = "G";
}  // namespace msg_type

// A message as an ordered list of fields, MsgType first. encode() adds the framing fields.
class Message {
public:
    Message() = default;
    explicit Message(std::string_view type);

    // The MsgType (35), or an empty view if there is none.
    std::string_view type() const;

    // Replaces the tag's value, or appends the field.
    Message& set(int field_tag, std::string_view value);
    Message& set_int(int field_tag, std::int64_t value);

    std::optional<std::string_view> get(int field_tag) const;
    // The value as a whole number, or nothing if the tag is missing or not a number.
    std::optional<std::int64_t> get_int(int field_tag) const;

    const std::vector<std::pair<int, std::string>>& fields() const noexcept {
        return fields_;
    }

    void clear() noexcept {
        fields_.clear();
    }

    bool operator==(const Message&) const = default;

private:
    std::vector<std::pair<int, std::string>> fields_;
};

// Adds BeginString and BodyLength in front and the CheckSum at the end. Throws std::invalid_argument if the message
// has no MsgType, a tag is not positive, or a value is empty or contains SOH.
std::string encode(const Message& message);

enum class ParseStatus : std::uint8_t {
    Complete,    // `consumed` bytes held a valid message
    Incomplete,  // the buffer holds the start of a message; read more
    Invalid      // the framing is broken; the stream can't be trusted from here on
};

struct ParseResult {
    ParseStatus status{ParseStatus::Incomplete};
    std::size_t consumed{0};
    std::string error;
};

// Parses the message at the start of `buffer`. Checks that BeginString and BodyLength come first, that the body has
// exactly BodyLength bytes and starts with MsgType, that every field is tag=value with a positive tag and a value,
// and that the CheckSum matches.
ParseResult parse(std::string_view buffer, Message& message);

// FIX prices are decimals; the engine works in ticks of 0.01.
std::string format_price(Price ticks);

// A SendingTime value: UTC as YYYYMMDD-HH:MM:SS.sss.
std::string utc_timestamp(std::chrono::system_clock::time_point now);

// A readable rendering, with | in place of SOH, for logs and tests.
std::string to_text(std::string_view encoded);

}  // namespace tradematch::fix

#endif
