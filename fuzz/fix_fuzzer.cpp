// Coverage-guided fuzz target for the FIX gateway. Raw bytes exercise the parser and the session's handling of
// broken input; the same bytes also become well-formed messages, with correct headers, sequence numbers, and
// checksums, from two logged-on sessions, which drives the order handling with arbitrary fields. Nothing may crash,
// a parsed message must survive re-encoding, and everything the gateway sends must parse.
#include "tradematch/fix.hpp"
#include "tradematch/fix_gateway.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace fix = tradematch::fix;

[[noreturn]] void fail(const char* problem) {
    std::fprintf(stderr, "%s\n", problem);
    std::abort();
}

constexpr fix::Clock::time_point kNow = fix::Clock::time_point{} + std::chrono::hours{24 * 20'000};

void check_output(fix::GatewayCore& core, fix::SessionId session) {
    std::string_view output;
    const std::string bytes = core.take_output(session);
    output = bytes;
    fix::Message message;
    while (!output.empty()) {
        const auto result = fix::parse(output, message);
        if (result.status != fix::ParseStatus::Complete) {
            fail("the gateway sent something that doesn't parse");
        }
        output.remove_prefix(result.consumed);
    }
}

void check_round_trip(std::string_view bytes) {
    fix::Message message;
    while (!bytes.empty()) {
        const auto result = fix::parse(bytes, message);
        if (result.status != fix::ParseStatus::Complete) {
            return;
        }
        fix::Message again;
        const std::string encoded = fix::encode(message);
        if (fix::parse(encoded, again).status != fix::ParseStatus::Complete || !(again == message)) {
            fail("a parsed message didn't survive re-encoding");
        }
        bytes.remove_prefix(result.consumed);
    }
}

// Reads the fuzz input as messages: a selector byte for the session and MsgType, a field count, then for each field a
// tag selector and a short value.
class MessageBuilder {
public:
    explicit MessageBuilder(std::string_view input) : input_(input) {}

    bool done() const noexcept {
        return position_ >= input_.size();
    }

    std::uint8_t byte() noexcept {
        return position_ < input_.size() ? static_cast<std::uint8_t>(input_[position_++]) : std::uint8_t{0};
    }

    std::string value() {
        static constexpr std::array<const char*, 12>
            kCommon{"1", "2", "3", "4", "0", "100", "100.50", "ACME", "C1", "C2", "6", "-5"};
        const std::uint8_t selector = byte();
        if (selector < 160) {
            return kCommon[selector % kCommon.size()];
        }
        std::string text;
        for (std::size_t length = 1 + selector % 8; length > 0; --length) {
            const char character = static_cast<char>(byte());
            text.push_back(character == fix::kSoh || character == '\0' ? 'x' : character);
        }
        return text;
    }

private:
    std::string_view input_;
    std::size_t position_{0};
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    check_round_trip(input);

    fix::GatewayCore core({});
    std::array<fix::SessionId, 2> sessions{core.open_session(kNow), core.open_session(kNow)};
    std::array<std::int64_t, 2> sequences{1, 1};
    const auto header = [&](std::size_t client, std::string_view type) {
        fix::Message message(type);
        message.set(fix::tag::SenderCompID, client == 0 ? "ALPHA" : "BETA");
        message.set(fix::tag::TargetCompID, "TRADEMATCH");
        message.set_int(fix::tag::MsgSeqNum, sequences[client]++);
        message.set(fix::tag::SendingTime, "20250101-12:00:00.000");
        return message;
    };
    const auto logon = [&](std::size_t client) {
        fix::Message message = header(client, fix::msg_type::Logon);
        message.set_int(fix::tag::HeartBtInt, 30);
        core.receive(sessions[client], fix::encode(message), kNow);
    };
    logon(0);
    logon(1);

    // Raw bytes on a third session.
    const fix::SessionId raw = core.open_session(kNow);
    core.receive(raw, input, kNow);
    check_output(core, raw);

    // An empty type disconnects the client, which then logs back on in a new session that still owns its orders.
    static constexpr std::array<std::string_view, 10> kTypes{"D", "D", "D", "F", "G", "0", "1", "5", "V", ""};
    static constexpr std::array<int, 14> kTags{11, 41, 55, 54, 38, 40, 44, 99, 59, 18, 111, 112, 108, 58};
    MessageBuilder builder(input);
    while (!builder.done()) {
        const std::uint8_t selector = builder.byte();
        const std::size_t client = selector & 1U;
        const std::string_view type = kTypes[(selector >> 1U) % kTypes.size()];
        if (type.empty()) {
            check_output(core, sessions[client]);
            core.close_session(sessions[client]);
            sessions[client] = core.open_session(kNow);
            sequences[client] = 1;
            logon(client);
            continue;
        }
        fix::Message message = header(client, type);
        for (std::size_t count = builder.byte() % 10U; count > 0; --count) {
            const int field = kTags[builder.byte() % kTags.size()];
            message.set(field, builder.value());
        }
        core.receive(sessions[client], fix::encode(message), kNow);
    }
    core.on_timer(kNow + std::chrono::seconds{45});
    for (const fix::SessionId session : sessions) {
        check_output(core, session);
    }
    return 0;
}
