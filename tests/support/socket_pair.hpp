#pragma once

// Plain TCP sockets over loopback for the tests, the same on POSIX and
// Windows.

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::test {

#ifdef _WIN32
using socket_handle = std::uintptr_t;                              // SOCKET
inline constexpr socket_handle kInvalidSocket = ~socket_handle{0}; // INVALID_SOCKET
#else
using socket_handle = int;
inline constexpr socket_handle kInvalidSocket = -1;
#endif

// Starts Winsock on Windows, once; nothing elsewhere.
void init_sockets();

void close_socket(socket_handle s);

// A socket connected to 127.0.0.1:`port`. Throws on failure.
socket_handle connect_loopback(std::uint16_t port);

// A connected loopback TCP pair: {client, server}. Throws on failure.
std::pair<socket_handle, socket_handle> make_loopback_pair();

// A socket listening on 127.0.0.1 at an ephemeral port, and that port.
std::pair<socket_handle, std::uint16_t> listen_loopback();

// A port on 127.0.0.1 nothing listens on (it was free a moment ago).
std::uint16_t unused_port();

// Writes all of `data`. False if the connection failed.
bool send_all(socket_handle s, const std::string& data);

// Reads up to `max` bytes: empty once the peer closed or on an error.
std::string recv_some(socket_handle s, std::size_t max = std::size_t{64} * 1024);

} // namespace c_share::test
