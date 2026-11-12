#include "tradematch/detail/socket.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace tradematch::detail {

namespace {

#ifdef _WIN32
int last_socket_error() noexcept {
    return WSAGetLastError();
}

bool would_block(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}

constexpr NativeSocket kInvalid = INVALID_SOCKET;
#else
int last_socket_error() noexcept {
    return errno;
}

bool would_block(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

constexpr NativeSocket kInvalid = -1;
#endif

[[noreturn]] void fail(const char* what) {
    throw std::system_error(last_socket_error(), std::system_category(), what);
}

void close_native(NativeSocket native) noexcept {
#ifdef _WIN32
    closesocket(native);
#else
    ::close(native);
#endif
}

}  // namespace

SocketLibrary::SocketLibrary() {
#ifdef _WIN32
    WSADATA data{};
    if (const int error = WSAStartup(MAKEWORD(2, 2), &data); error != 0) {
        throw std::system_error(error, std::system_category(), "WSAStartup");
    }
#endif
}

SocketLibrary::~SocketLibrary() {
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket::Socket(Socket&& other) noexcept : native_(std::exchange(other.native_, kInvalid)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        native_ = std::exchange(other.native_, kInvalid);
    }
    return *this;
}

Socket::~Socket() {
    close();
}

bool Socket::valid() const noexcept {
    return native_ != kInvalid;
}

void Socket::close() noexcept {
    if (valid()) {
        close_native(native_);
        native_ = kInvalid;
    }
}

Socket Socket::listen(std::uint16_t port, bool loopback_only) {
    Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid()) {
        fail("socket");
    }
    const int enabled = 1;
#ifdef _WIN32
    // On Windows, SO_REUSEADDR would let another socket take over a port that is in use; this forbids that instead.
    setsockopt(socket.native_,
               SOL_SOCKET,
               SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&enabled),
               sizeof(enabled));
#else
    // Lets a restarted gateway listen again at once, while connections from its previous run are in TIME_WAIT.
    setsockopt(socket.native_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(socket.native_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        fail("bind");
    }
    if (::listen(socket.native_, SOMAXCONN) != 0) {
        fail("listen");
    }
    return socket;
}

Socket Socket::connect(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (const int error = getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
        error != 0 || results == nullptr) {
        throw std::system_error(std::make_error_code(std::errc::host_unreachable), "cannot resolve " + host);
    }
    Socket socket{::socket(results->ai_family, results->ai_socktype, results->ai_protocol)};
    const bool connected =
        socket.valid() && ::connect(socket.native_, results->ai_addr, static_cast<int>(results->ai_addrlen)) == 0;
    freeaddrinfo(results);
    if (!connected) {
        fail("connect");
    }
    return socket;
}

Socket Socket::accept() const {
    const NativeSocket accepted = ::accept(native_, nullptr, nullptr);
    if (accepted == kInvalid) {
        if (would_block(last_socket_error())) {
            return Socket{};
        }
        fail("accept");
    }
    return Socket{accepted};
}

void Socket::set_nonblocking() const {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(native_, FIONBIO, &enabled) != 0) {
        fail("ioctlsocket");
    }
#else
    const int flags = fcntl(native_, F_GETFL, 0);
    if (flags < 0 || fcntl(native_, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail("fcntl");
    }
#endif
}

void Socket::set_no_delay() const {
    const int enabled = 1;
    if (setsockopt(native_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        fail("setsockopt(TCP_NODELAY)");
    }
#ifdef SO_NOSIGPIPE
    // macOS: a write to a closed connection reports EPIPE instead of killing the process with SIGPIPE.
    setsockopt(native_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

void Socket::set_receive_timeout(int milliseconds) const {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    const auto* value = reinterpret_cast<const char*>(&timeout);
    const int size = sizeof(timeout);
#else
    const timeval timeout{milliseconds / 1000, static_cast<suseconds_t>(milliseconds % 1000) * 1000};
    const auto* value = &timeout;
    const auto size = static_cast<socklen_t>(sizeof(timeout));
#endif
    if (setsockopt(native_, SOL_SOCKET, SO_RCVTIMEO, value, size) != 0) {
        fail("setsockopt(SO_RCVTIMEO)");
    }
}

std::ptrdiff_t Socket::send(std::span<const char> data) const noexcept {
#ifdef _WIN32
    const int sent = ::send(native_, data.data(), static_cast<int>(data.size()), 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t sent = ::send(native_, data.data(), data.size(), MSG_NOSIGNAL);
#else
    const ssize_t sent = ::send(native_, data.data(), data.size(), 0);
#endif
    if (sent < 0) {
        return would_block(last_socket_error()) ? 0 : -1;
    }
    return static_cast<std::ptrdiff_t>(sent);
}

std::ptrdiff_t Socket::receive(std::span<char> buffer) const noexcept {
#ifdef _WIN32
    const int received = ::recv(native_, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t received = ::recv(native_, buffer.data(), buffer.size(), 0);
#endif
    if (received == 0) {
        return -1;  // orderly shutdown by the peer
    }
    if (received < 0) {
        return would_block(last_socket_error()) ? 0 : -1;
    }
    return static_cast<std::ptrdiff_t>(received);
}

std::uint16_t Socket::local_port() const {
    sockaddr_in address{};
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(native_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        fail("getsockname");
    }
    return ntohs(address.sin_port);
}

int poll_sockets(std::span<PollEntry> entries, int timeout_milliseconds) {
#ifdef _WIN32
    const int ready = WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), timeout_milliseconds);
#else
    const int ready = ::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeout_milliseconds);
#endif
    if (ready < 0) {
        if (would_block(last_socket_error())) {
            return 0;  // interrupted by a signal
        }
        fail("poll");
    }
    return ready;
}

}  // namespace tradematch::detail
