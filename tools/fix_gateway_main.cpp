// A FIX 4.2 order-entry gateway in front of the matching engine. Runs until interrupted (Ctrl+C).
#include "tradematch/fix_gateway.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};
}  // namespace

// Signal handlers need C linkage; this one only sets a lock-free flag, which is async-signal-safe.
extern "C" void tradematch_gateway_interrupt(int /*signal*/) {
    g_stop = true;
}

namespace {

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::stringstream stream{std::string{text}};
    for (std::string part; std::getline(stream, part, separator);) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

int run(int argc, char** argv) {
    tradematch::fix::GatewayConfig config;
    config.port = 9878;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tradematch_fix_gateway [--port N] [--symbols ACME,GLOBEX] [--comp-id ID] "
                         "[--any-interface]\n";
            return 0;
        }
        if (arg == "--any-interface") {
            config.loopback_only = false;
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const std::string& value = args[++index];
        if (arg == "--port") {
            unsigned port = 0;
            const auto [stop, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || stop != value.data() + value.size() || port > 65'535U) {
                throw std::invalid_argument("invalid port " + value);
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--symbols") {
            config.symbols = split(value, ',');
        } else if (arg == "--comp-id") {
            config.comp_id = value;
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }

    tradematch::fix::Gateway gateway(config);
    std::cout << "FIX 4.2 gateway listening on " << (config.loopback_only ? "127.0.0.1" : "0.0.0.0") << ':'
              << gateway.port() << " as " << config.comp_id << ", symbols";
    for (const auto& symbol : config.symbols) {
        std::cout << ' ' << symbol;
    }
    std::cout << "\nPress Ctrl+C to stop.\n" << std::flush;
    std::signal(SIGINT, tradematch_gateway_interrupt);
    gateway.run(g_stop);

    const auto& stats = gateway.core().stats();
    std::cout << "\nsessions " << stats.sessions << ", messages in " << stats.messages_received << ", out "
              << stats.messages_sent << ", orders " << stats.orders << ", trades " << stats.trades << ", rejects "
              << stats.order_rejects + stats.session_rejects << '\n';
    return 0;
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
