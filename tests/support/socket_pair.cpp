#include "socket_pair.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <mutex>
#include <stdexcept>

namespace c_share::test {

namespace {

#ifdef _WIN32
using length_t = int;
#else
using length_t = socklen_t;
#endif

// A peer that already closed must fail a send, not kill the test run with
// SIGPIPE: MSG_NOSIGNAL where there is one (Linux), SO_NOSIGPIPE on Apple's
// platforms (see send_all). Windows has no SIGPIPE.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

sockaddr_in loopback_address(const std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

} // namespace

void init_sockets() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void close_socket(const socket_handle s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

std::pair<socket_handle, std::uint16_t> listen_loopback() {
    init_sockets();
    const socket_handle listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) throw std::runtime_error("socket failed");
    sockaddr_in addr = loopback_address(0);
    length_t len = sizeof(addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 16) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close_socket(listener);
        throw std::runtime_error("cannot listen on loopback");
    }
    return {listener, ntohs(addr.sin_port)};
}

std::uint16_t unused_port() {
    const auto [listener, port] = listen_loopback();
    close_socket(listener);
    return port;
}

socket_handle connect_loopback(const std::uint16_t port) {
    init_sockets();
    const socket_handle client = socket(AF_INET, SOCK_STREAM, 0);
    const sockaddr_in addr = loopback_address(port);
    if (client == kInvalidSocket || connect(client, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (client != kInvalidSocket) close_socket(client);
        throw std::runtime_error("cannot connect on loopback");
    }
    return client;
}

std::pair<socket_handle, socket_handle> make_loopback_pair() {
    const auto [listener, port] = listen_loopback();
    const socket_handle client = [listener = listener, port = port] {
        try {
            return connect_loopback(port);
        } catch (const std::exception&) {
            close_socket(listener);
            throw;
        }
    }();
    const socket_handle server = accept(listener, nullptr, nullptr);
    close_socket(listener);
    if (server == kInvalidSocket) throw std::runtime_error("cannot accept on loopback");
    return {client, server};
}

bool send_all(const socket_handle s, const std::string& data) {
#ifdef SO_NOSIGPIPE
    constexpr int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), kSendFlags);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string recv_some(const socket_handle s, const std::size_t max) {
    std::string buffer(max, '\0');
    const auto n = recv(s, buffer.data(), static_cast<int>(max), 0);
    buffer.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return buffer;
}

} // namespace c_share::test
