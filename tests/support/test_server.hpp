#pragma once

// A tiny HTTP/1.1 and WebSocket server on loopback, for the http module's
// tests: each test scripts what the server reads and answers.

#include "socket_pair.hpp"

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::test {

struct HttpRequest {
    std::string method;
    std::string target; // e.g. "/path?query"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // The first header named `name` (any case).
    [[nodiscard]] std::optional<std::string> header(const std::string& name) const;
};

// A frame as the server reads it (unmasked) or writes it (never masked).
struct WsFrame {
    bool fin = true;
    bool rsv1 = false;
    std::uint8_t opcode = 0x1;
    std::string payload;
};

// One accepted connection.
class Connection {
public:
    explicit Connection(const socket_handle s) : s_(s) {}

    // The next request, with its Content-Length body; nullopt once the
    // client closed the connection.
    std::optional<HttpRequest> read_request();
    [[nodiscard]] bool write(const std::string& data) const;

    // Answers `request`, an upgrade, with 101 and `extensions` as its
    // Sec-WebSocket-Extensions (none if empty).
    [[nodiscard]] bool accept_websocket(const HttpRequest& request, const std::string& extensions = {}) const;
    // The next frame; nullopt once the connection closed.
    std::optional<WsFrame> read_frame();
    [[nodiscard]] bool write_frame(const WsFrame& frame) const;

private:
    // Reads until `buffered_` holds `size` bytes. False if the connection closed.
    bool fill(std::size_t size);

    socket_handle s_;
    std::string buffered_;
};

class TestServer {
public:
    using Handler = std::function<void(Connection&)>;

    // Calls `handler` for each connection, on a thread of its own, closing
    // the connection after.
    explicit TestServer(Handler handler);
    // An HTTP server answering each request with what `respond` returns: a
    // whole raw response ("HTTP/1.1 200 OK\r\n...").
    static TestServer http(std::function<std::string(const HttpRequest&)> respond);
    ~TestServer();

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    TestServer(TestServer&&) = delete;
    TestServer& operator=(TestServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    // "<scheme>://127.0.0.1:<port><path>"
    [[nodiscard]] std::string url(const std::string& path, const std::string& scheme = "http") const;

private:
    void accept_loop();

    Handler handler_;
    socket_handle listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_; // guards connections_, workers_
    std::vector<socket_handle> connections_;
    std::vector<std::thread> workers_;
    std::thread acceptor_;
};

// A raw HTTP/1.1 response with a Content-Length body.
std::string http_response(int status, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& headers = {});

// Raw deflate as permessage-deflate uses it (RFC 7692): each message ends
// with a sync flush whose 00 00 ff ff tail is left off the wire.
class Deflate {
public:
    Deflate();
    ~Deflate();

    Deflate(const Deflate&) = delete;
    Deflate& operator=(const Deflate&) = delete;
    Deflate(Deflate&&) = delete;
    Deflate& operator=(Deflate&&) = delete;

    std::string compress(const std::string& message);
    std::string decompress(const std::string& message);
    void reset_compressor();

private:
    struct Streams;
    Streams* streams_;
};

// gzip, for a Content-Encoding: gzip body.
std::string gzip(const std::string& data);

} // namespace c_share::test
