#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "c_share/waker/waker.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using socket_handle = SOCKET;
constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
void close_socket(const socket_handle s) {
    closesocket(s);
}
#else
using socket_handle = int;
constexpr socket_handle kInvalidSocket = -1;
void close_socket(const socket_handle s) {
    close(s);
}
#endif

// Creates a connected loopback TCP pair -- POSIX's socketpair() doesn't
// support AF_INET, and Windows has no socketpair() at all.
std::pair<socket_handle, socket_handle> make_loopback_pair() {
    const socket_handle listener = socket(AF_INET, SOCK_STREAM, 0);
    const socket_handle client = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    // Any failure leaves the demo without its sockets: stop right there.
    if (listener == kInvalidSocket || client == kInvalidSocket ||
        bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("cannot set up the loopback socket pair");
    }

    const socket_handle server = accept(listener, nullptr, nullptr);
    close_socket(listener);
    return {client, server};
}

void run_demo() {
    c_share::waker::Waker waker;

    // 1) wake_after(): a scheduled, one-shot wake-up.
    waker.wake_after(std::chrono::milliseconds(50));
    std::cout << "waiting for the 50ms timer...\n";
    if (waker.wait() == c_share::waker::WakeReason::Timeout) {
        std::cout << "-> woke up: Timeout\n";
    }

    // 2) wake(): wake from another thread immediately.
    std::thread([&waker] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waker.wake();
    }).detach();

    std::cout << "waiting for a manual wake() from another thread...\n";
    if (waker.wait() == c_share::waker::WakeReason::Manual) {
        std::cout << "-> woke up: Manual\n";
    }

    // 3) attach_socket(): wake when a socket has data to read.
    const auto [client, server] = make_loopback_pair();
    waker.attach_socket(server);

    // A structured binding cannot be captured as such before C++20.
    std::thread([client = client] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        constexpr char msg[] = "hi";
        send(client, msg, sizeof(msg), 0);
    }).detach();

    std::cout << "waiting for data on the attached socket...\n";
    if (waker.wait() == c_share::waker::WakeReason::Socket) {
        for (const auto& [fd, ready] : waker.take_ready_sockets()) {
            char buf[64];
            const auto n = recv(fd, buf, sizeof(buf), 0);
            std::cout << "-> woke up: Socket, read " << n << " bytes\n";
        }
    }

    waker.detach_socket(server);
    close_socket(client);
    close_socket(server);
}

} // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    int status = 0;
    try {
        run_demo();
    } catch (const std::exception& error) {
        std::cerr << "waker_demo: " << error.what() << '\n';
        status = 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return status;
}
