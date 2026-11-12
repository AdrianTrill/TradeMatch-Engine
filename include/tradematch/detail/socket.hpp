#ifndef TRADEMATCH_DETAIL_SOCKET_HPP
#define TRADEMATCH_DETAIL_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <poll.h>
#endif

// A thin, portable layer over TCP sockets: POSIX sockets on Linux and macOS, Winsock on Windows.
namespace tradematch::detail {

#ifdef _WIN32
using NativeSocket = SOCKET;
using PollEntry = WSAPOLLFD;
#else
using NativeSocket = int;
using PollEntry = pollfd;
#endif

// Initializes the socket library for as long as an instance lives (Winsock needs it; elsewhere it does nothing).
class SocketLibrary {
public:
    SocketLibrary();
    SocketLibrary(const SocketLibrary&) = delete;
    SocketLibrary& operator=(const SocketLibrary&) = delete;
    SocketLibrary(SocketLibrary&&) = delete;
    SocketLibrary& operator=(SocketLibrary&&) = delete;
    ~SocketLibrary();
};

// An owned TCP socket. Methods throw std::system_error on failure, except send() and receive(), which report a closed
// or failed connection through their return value. The object is a handle, so operations on the connection are const;
// only close() and moves change which socket it holds.
class Socket {
public:
    Socket() = default;
    explicit Socket(NativeSocket native) noexcept : native_(native) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    // A listening socket on the port (0 picks a free one), on the loopback interface or on every interface.
    static Socket listen(std::uint16_t port, bool loopback_only);
    // A blocking connection to host:port; host is a name or an address.
    static Socket connect(const std::string& host, std::uint16_t port);

    // Accepts a pending connection, or returns an invalid socket if none is waiting (non-blocking listeners).
    Socket accept() const;

    void set_nonblocking() const;
    void set_no_delay() const;  // send small messages at once instead of batching them (Nagle's algorithm)
    void set_receive_timeout(int milliseconds) const;

    // Bytes sent, 0 if the socket would block, or -1 if the connection is gone.
    std::ptrdiff_t send(std::span<const char> data) const noexcept;
    // Bytes received, 0 if the socket would block, or -1 if the connection is closed or failed.
    std::ptrdiff_t receive(std::span<char> buffer) const noexcept;

    std::uint16_t local_port() const;
    bool valid() const noexcept;
    NativeSocket native() const noexcept {
        return native_;
    }
    void close() noexcept;

private:
#ifdef _WIN32
    NativeSocket native_{INVALID_SOCKET};
#else
    NativeSocket native_{-1};
#endif
};

// poll(2), or WSAPoll on Windows. Returns the number of entries with events, or throws std::system_error.
int poll_sockets(std::span<PollEntry> entries, int timeout_milliseconds);

}  // namespace tradematch::detail

#endif
