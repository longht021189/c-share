#include "c_share/http/client.hpp"

#include "test_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace c_share::http;
using c_share::test::Connection;
using c_share::test::http_response;
using c_share::test::HttpRequest;
using c_share::test::TestServer;
using c_share::test::WsFrame;
using namespace std::chrono_literals;

namespace {

constexpr auto kWait = 10s;

// libcurl's process-wide setup, which Client leaves to its caller.
struct CurlGlobal {
    CurlGlobal() noexcept { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    CurlGlobal(CurlGlobal&&) = delete;
    CurlGlobal& operator=(CurlGlobal&&) = delete;
};
// Used for its constructor and destructor alone.
// ReSharper disable once CppDeclaratorNeverUsed
const CurlGlobal curl_global;

// A Client with its event loop running on a thread of its own.
class RunningClient {
public:
    RunningClient() : loop_([this] { client_.run(); }) {}
    ~RunningClient() {
        client_.stop();
        loop_.join();
    }
    RunningClient(const RunningClient&) = delete;
    RunningClient& operator=(const RunningClient&) = delete;
    RunningClient(RunningClient&&) = delete;
    RunningClient& operator=(RunningClient&&) = delete;

    Client* operator->() { return &client_; }

private:
    Client client_;
    std::thread loop_;
};

// What the server saw, shared with its thread.
struct Seen {
    std::mutex mutex;
    std::vector<HttpRequest> requests;

    void add(const HttpRequest& request) {
        std::scoped_lock lock(mutex);
        requests.push_back(request);
    }
    HttpRequest at(const std::size_t i) {
        std::scoped_lock lock(mutex);
        REQUIRE(i < requests.size());
        return requests[i];
    }
};

// Everything the callbacks of one request report.
struct Outcome {
    Response response;
    Error error;
    std::optional<Response> head;
    std::string streamed;
    bool body_before_head = false;
    Action head_action = Action::Continue;
    Action body_action = Action::Continue;
    std::promise<void> done;
};

Action on_head(void* context, const Response& head) {
    auto* outcome = static_cast<Outcome*>(context);
    outcome->head = head;
    return outcome->head_action;
}

Action on_body(void* context, const char* data, const std::size_t size) {
    auto* outcome = static_cast<Outcome*>(context);
    outcome->body_before_head = outcome->body_before_head || !outcome->head;
    outcome->streamed.append(data, size);
    return outcome->body_action;
}

void on_done(void* context, Response response, Error error) {
    auto* outcome = static_cast<Outcome*>(context);
    outcome->response = std::move(response);
    outcome->error = std::move(error);
    outcome->done.set_value();
}

// Sends `request` and waits for it to complete.
void send(RunningClient& client, Request request, Outcome& outcome, const bool stream = false,
          const bool with_head = false) {
    const auto done = outcome.done.get_future();
    client->send(std::move(request), &outcome, stream ? &on_body : nullptr, &on_done, with_head ? &on_head : nullptr);
    REQUIRE(done.wait_for(kWait) == std::future_status::ready);
}

std::optional<std::string> header(const Response& response, const std::string& name) {
    for (const auto& [header_name, value] : response.headers) {
        if (curl_strequal(header_name.c_str(), name.c_str())) return value;
    }
    return std::nullopt;
}

// Everything the callbacks of one WebSocket report, waited on with wait().
struct WsOutcome {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Response> open;
    std::vector<std::pair<WsMessageType, std::string>> messages;
    bool closed = false;
    Error error;
    std::uint16_t code = 0;
    std::string reason;

    template <typename Predicate> bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, kWait, [&] { return predicate(*this); });
    }
};

void on_ws_open(void* context, const Response& head) {
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->open = head;
    ws->changed.notify_all();
}

void on_ws_message(void* context, const WsMessageType type, const char* data, const std::size_t size) {
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->messages.emplace_back(type, std::string(data, size));
    ws->changed.notify_all();
}

void on_ws_close(void* context, Error error, const std::uint16_t code, const std::string& reason) {
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->closed = true;
    ws->error = std::move(error);
    ws->code = code;
    ws->reason = reason;
    ws->changed.notify_all();
}

Client::Handle open(RunningClient& client, const std::string& url, WsOutcome& ws, const bool deflate = false,
                    std::vector<Header> headers = {}) {
    Request request;
    request.url = url;
    request.headers = std::move(headers);
    WsOptions options;
    options.deflate = deflate;
    return client->open_websocket(std::move(request), &ws, &on_ws_open, &on_ws_message, &on_ws_close, options);
}

std::string close_payload(const std::uint16_t code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xff));
    return payload + reason;
}

// Echoes every data message back until the client's close, answered in kind.
void echo(Connection& connection) {
    while (const auto frame = connection.read_frame()) {
        if (frame->opcode == 0x8) {
            // The connection ends either way.
            (void)connection.write_frame(WsFrame{true, false, 0x8, frame->payload.substr(0, 2)});
            return;
        }
        if ((frame->opcode == 0x1 || frame->opcode == 0x2) && !connection.write_frame(*frame)) return;
    }
}

} // namespace

TEST_CASE("a GET returns status, headers and body", "[client]") {
    Seen seen;
    auto server = TestServer::http([&seen](const HttpRequest& request) {
        seen.add(request);
        return http_response(200, "hello", {{"X-Test", "yes"}});
    });
    RunningClient client;

    Request request;
    request.url = server.url("/path?q=1");
    request.headers = {{"X-Custom", "value"}};
    Outcome outcome;
    send(client, request, outcome);

    CHECK_FALSE(outcome.error);
    CHECK(outcome.response.status_code == 200);
    CHECK(outcome.response.body == "hello");
    CHECK(header(outcome.response, "x-test") == "yes");

    const HttpRequest received = seen.at(0);
    CHECK(received.method == "GET");
    CHECK(received.target == "/path?q=1");
    CHECK(received.header("x-custom") == "value");
    // reqwest's defaults: Accept */*, the codings this libcurl decodes, no
    // User-Agent unless the caller sets one.
    CHECK(received.header("accept") == "*/*");
    CHECK_THAT(received.header("accept-encoding").value_or(""), Catch::Matchers::ContainsSubstring("gzip"));
    CHECK_FALSE(received.header("user-agent"));
}

TEST_CASE("a caller's own default headers win", "[client]") {
    Seen seen;
    auto server = TestServer::http([&seen](const HttpRequest& request) {
        seen.add(request);
        return http_response(200, "");
    });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    request.headers = {{"Accept", "text/html"}, {"User-Agent", "tests"}};
    Outcome outcome;
    send(client, request, outcome);

    const HttpRequest received = seen.at(0);
    CHECK(received.header("accept") == "text/html");
    CHECK(received.header("user-agent") == "tests");
}

TEST_CASE("methods with a body send it", "[client]") {
    const auto [method, verb] = GENERATE(std::pair{Method::Post, "POST"}, std::pair{Method::Put, "PUT"},
                                         std::pair{Method::Patch, "PATCH"}, std::pair{Method::Delete, "DELETE"});
    CAPTURE(verb);
    Seen seen;
    auto server = TestServer::http([&seen](const HttpRequest& request) {
        seen.add(request);
        return http_response(201, "created");
    });
    RunningClient client;
    Request request;
    request.url = server.url("/items");
    request.method = method;
    request.body = "{\"a\":1}";
    request.headers = {{"Content-Type", "application/json"}};
    Outcome outcome;
    send(client, request, outcome);

    CHECK(outcome.response.status_code == 201);
    const HttpRequest received = seen.at(0);
    CHECK(received.method == verb);
    CHECK(received.body == "{\"a\":1}");
    CHECK_FALSE(received.header("expect")); // no 100-continue round trip
}

TEST_CASE("HEAD and OPTIONS send their verb", "[client]") {
    const auto [method, verb] = GENERATE(std::pair{Method::Head, "HEAD"}, std::pair{Method::Options, "OPTIONS"});
    Seen seen;
    auto server = TestServer::http([&seen, method = method](const HttpRequest& request) {
        seen.add(request);
        // A HEAD response carries the length of a body it does not send.
        return method == Method::Head ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n" : http_response(204, "");
    });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    request.method = method;
    Outcome outcome;
    send(client, request, outcome);

    CHECK_FALSE(outcome.error);
    CHECK(outcome.response.body.empty());
    CHECK(seen.at(0).method == verb);
}

TEST_CASE("an HTTP error status is a response, not an error", "[client]") {
    auto server = TestServer::http([](const HttpRequest&) { return http_response(404, "missing"); });
    RunningClient client;
    Request request;
    request.url = server.url("/nowhere");
    Outcome outcome;
    send(client, request, outcome);

    CHECK_FALSE(outcome.error);
    CHECK(outcome.response.status_code == 404);
    CHECK(outcome.response.body == "missing");
}

TEST_CASE("a gzip body arrives decoded, without the headers describing it", "[client]") {
    const std::string body(10000, 'a');
    auto server = TestServer::http([&body](const HttpRequest&) {
        return http_response(200, c_share::test::gzip(body), {{"Content-Encoding", "gzip"}});
    });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    Outcome outcome;
    send(client, request, outcome);

    CHECK(outcome.response.body == body);
    CHECK_FALSE(header(outcome.response, "content-encoding"));
    CHECK_FALSE(header(outcome.response, "content-length"));
}

TEST_CASE("a body callback gets the body instead of the response", "[client]") {
    std::string body;
    for (int i = 0; i < 100000; ++i)
        body += static_cast<char>('a' + i % 26);
    auto server = TestServer::http([&body](const HttpRequest&) { return http_response(200, body); });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    Outcome outcome;
    send(client, request, outcome, true, true);

    CHECK_FALSE(outcome.error);
    CHECK(outcome.response.body.empty());
    CHECK(outcome.streamed == body);
    CHECK_FALSE(outcome.body_before_head);
}

TEST_CASE("a body callback can abort the transfer", "[client]") {
    auto server = TestServer::http([](const HttpRequest&) { return http_response(200, std::string(100000, 'x')); });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    Outcome outcome;
    outcome.body_action = Action::Abort;
    send(client, request, outcome, true);
    CHECK(outcome.error.code == ErrorCode::Aborted);
}

TEST_CASE("the head callback sees the final status and headers first", "[client]") {
    auto server = TestServer::http([](const HttpRequest&) { return http_response(203, "body", {{"X-Head", "1"}}); });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    Outcome outcome;

    SECTION("and lets it continue") {
        send(client, request, outcome, false, true);
        REQUIRE(outcome.head);
        CHECK(outcome.head->status_code == 203);
        CHECK(header(*outcome.head, "x-head") == "1");
        CHECK(outcome.head->body.empty());
        CHECK(outcome.response.body == "body");
    }
    SECTION("or aborts it") {
        outcome.head_action = Action::Abort;
        send(client, request, outcome, false, true);
        CHECK(outcome.error.code == ErrorCode::Aborted);
    }
}

TEST_CASE("redirects are followed only when asked", "[client]") {
    Seen seen;
    auto server = TestServer::http([&seen](const HttpRequest& request) {
        seen.add(request);
        if (request.target == "/from") return http_response(302, "", {{"Location", "/to"}});
        return http_response(200, "arrived");
    });
    RunningClient client;
    Request request;
    request.url = server.url("/from");

    SECTION("followed") {
        Outcome with_head;
        send(client, request, with_head, false, true);
        CHECK(with_head.response.status_code == 200);
        CHECK(with_head.response.body == "arrived");
        // The head callback only sees the final hop.
        REQUIRE(with_head.head);
        CHECK(with_head.head->status_code == 200);
        CHECK(seen.at(1).target == "/to");
    }
    SECTION("not followed") {
        request.follow_redirects = false;
        Outcome outcome;
        send(client, request, outcome);
        CHECK(outcome.response.status_code == 302);
        CHECK(header(outcome.response, "location") == "/to");
    }
}

TEST_CASE("transport failures come back as errors", "[client]") {
    Outcome outcome;
    RunningClient client;
    Request request;

    SECTION("nothing listening") {
        request.url = "http://127.0.0.1:" + std::to_string(c_share::test::unused_port()) + "/";
        send(client, request, outcome);
        CHECK(outcome.error.code == ErrorCode::CouldNotConnect);
        CHECK_FALSE(outcome.error.message.empty());
    }
    SECTION("a malformed URL") {
        // Fails before any I/O, which must still reach done_cb.
        request.url = GENERATE(as<std::string>{}, "https://[::1/", "not a url");
        send(client, request, outcome);
        CHECK(outcome.error.code == ErrorCode::InvalidUrl);
    }
}

TEST_CASE("a server that never answers times out", "[client]") {
    // Reads the request, then nothing more until the client goes away.
    TestServer server([](Connection& connection) {
        connection.read_request();
        connection.read_request();
    });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    request.timeout = 200ms;
    Outcome outcome;
    send(client, request, outcome);
    CHECK(outcome.error.code == ErrorCode::Timeout);
}

TEST_CASE("cancel() ends a request with Aborted", "[client]") {
    TestServer server([](Connection& connection) {
        connection.read_request();
        connection.read_request();
    });
    RunningClient client;
    Request request;
    request.url = server.url("/");
    Outcome outcome;
    auto done = outcome.done.get_future();
    const Client::Handle handle = client->send(request, &outcome, nullptr, &on_done);
    CHECK(handle != 0);
    std::this_thread::sleep_for(50ms);
    client->cancel(handle);
    REQUIRE(done.wait_for(kWait) == std::future_status::ready);
    CHECK(outcome.error.code == ErrorCode::Aborted);
    // A finished or unknown handle is a no-op.
    client->cancel(handle);
    client->cancel(12345);
}

TEST_CASE("many requests run at once", "[client]") {
    auto server = TestServer::http([](const HttpRequest& request) { return http_response(200, request.target); });
    // Outlives every request, so not in the loop.
    // ReSharper disable once CppTooWideScope
    RunningClient client;
    constexpr int kCount = 20;
    std::vector<Outcome> outcomes(kCount);
    std::vector<std::future<void>> done;
    for (int i = 0; i < kCount; ++i) {
        done.push_back(outcomes[i].done.get_future());
        Request request;
        request.url = server.url("/" + std::to_string(i));
        client->send(request, &outcomes[i], nullptr, &on_done);
    }
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(done[i].wait_for(kWait) == std::future_status::ready);
        CHECK(outcomes[i].response.body == "/" + std::to_string(i));
    }
}

TEST_CASE("destroying a client fails what is still in flight", "[client]") {
    TestServer server([](Connection& connection) {
        connection.read_request();
        connection.read_request();
    });
    Outcome outcome;
    auto done = outcome.done.get_future();
    {
        RunningClient client;
        Request request;
        request.url = server.url("/");
        client->send(request, &outcome, nullptr, &on_done);
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(done.wait_for(0s) == std::future_status::ready);
    CHECK(outcome.error.code == ErrorCode::Aborted);
}

TEST_CASE("a WebSocket exchanges messages and closes", "[client][websocket]") {
    // Offered but declined, deflate leaves the frames to this module's codec;
    // not offered, to libcurl's.
    const bool deflate = GENERATE(false, true);
    CAPTURE(deflate);
    Seen seen;
    std::promise<WsFrame> server_close;
    TestServer server([&](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        seen.add(*request);
        if (!connection.accept_websocket(*request)) return;
        while (const auto frame = connection.read_frame()) {
            if (frame->opcode == 0x8) {
                server_close.set_value(*frame);
                return;
            }
            if (!connection.write_frame(*frame)) return;
        }
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    const auto handle = open(client, server.url("/socket", "ws"), ws, deflate,
                             {{"Origin", "https://example.com"}, {"Sec-WebSocket-Extensions", "x-other"}});
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.open.has_value() || w.closed; }));
    REQUIRE(ws.open);
    CHECK(ws.open->status_code == 101);

    const HttpRequest upgrade = seen.at(0);
    CHECK(upgrade.target == "/socket");
    CHECK(upgrade.header("origin") == "https://example.com");
    CHECK(upgrade.header("sec-websocket-extensions") ==
          (deflate ? std::optional<std::string>("permessage-deflate") : std::nullopt));

    const std::string binary("\x00\x01\x02", 3);
    REQUIRE(client->send_websocket(handle, WsMessageType::Text, "hello", 5));
    REQUIRE(client->send_websocket(handle, WsMessageType::Binary, binary.data(), binary.size()));
    const std::string large(200000, 'L');
    REQUIRE(client->send_websocket(handle, WsMessageType::Text, large.data(), large.size()));
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.messages.size() == 3; }));
    CHECK(ws.messages[0] == std::pair{WsMessageType::Text, std::string("hello")});
    CHECK(ws.messages[1] == std::pair{WsMessageType::Binary, binary});
    CHECK(ws.messages[2].second == large);

    client->close_websocket(handle, 1000, "bye");
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK_FALSE(ws.error);
    CHECK(ws.code == 1000);
    CHECK(ws.reason == "bye");
    auto close_frame = server_close.get_future();
    REQUIRE(close_frame.wait_for(kWait) == std::future_status::ready);
    CHECK(close_frame.get().payload == close_payload(1000, "bye"));

    // Closed: nothing more to send or close.
    CHECK_FALSE(client->send_websocket(handle, WsMessageType::Text, "late", 4));
    client->close_websocket(handle, 1000, "");
}

TEST_CASE("a WebSocket reports the server's close, fragments and pings", "[client][websocket]") {
    const bool deflate = GENERATE(false, true);
    CAPTURE(deflate);
    std::promise<std::vector<WsFrame>> replies;
    TestServer server([&](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        if (!connection.accept_websocket(*request)) return;
        for (const auto& frame :
             {WsFrame{false, false, 0x1, "Hel"}, WsFrame{true, false, 0x9, "are you there"},
              WsFrame{true, false, 0x0, "lo"}, WsFrame{true, false, 0x8, close_payload(4000, "done")}}) {
            if (!connection.write_frame(frame)) return;
        }
        std::vector<WsFrame> frames;
        while (const auto frame = connection.read_frame()) {
            frames.push_back(*frame);
            if (frame->opcode == 0x8) break;
        }
        replies.set_value(frames);
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    open(client, server.url("/", "ws"), ws, deflate);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    REQUIRE(ws.messages.size() == 1);
    CHECK(ws.messages[0].second == "Hello");
    CHECK_FALSE(ws.error);
    CHECK(ws.code == 4000);
    CHECK(ws.reason == "done");

    // The ping was answered, and the close echoed.
    auto frames = replies.get_future();
    REQUIRE(frames.wait_for(kWait) == std::future_status::ready);
    const auto got = frames.get();
    REQUIRE(got.size() == 2);
    CHECK(got[0].opcode == 0xa);
    CHECK(got[0].payload == "are you there");
    CHECK(got[1].opcode == 0x8);
    CHECK(got[1].payload == close_payload(4000, ""));
}

TEST_CASE("a WebSocket with permessage-deflate compresses both ways", "[client][websocket]") {
    const std::string extensions =
        GENERATE(as<std::string>{}, "permessage-deflate",
                 "permessage-deflate; server_no_context_takeover; client_no_context_takeover");
    CAPTURE(extensions);
    const bool no_context = extensions.find("no_context") != std::string::npos;
    std::promise<std::vector<WsFrame>> received;
    TestServer server([&](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        if (!connection.accept_websocket(*request, extensions)) return;
        c_share::test::Deflate deflate;
        std::vector<WsFrame> frames;
        // Two messages compressed, one not; then echo what the client sends.
        for (const char* message : {"Hello", "Hello"}) {
            if (!connection.write_frame(WsFrame{true, true, 0x1, deflate.compress(message)})) return;
            if (no_context) deflate.reset_compressor();
        }
        if (!connection.write_frame(WsFrame{true, false, 0x2, "raw"})) return;
        while (const auto frame = connection.read_frame()) {
            WsFrame plain = *frame;
            if (frame->rsv1) plain.payload = deflate.decompress(frame->payload);
            frames.push_back(plain);
            if (frame->opcode == 0x8) break;
        }
        received.set_value(frames);
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    const auto handle = open(client, server.url("/", "ws"), ws, true);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.messages.size() == 3 || w.closed; }));
    CHECK(header(*ws.open, "sec-websocket-extensions") == extensions);
    CHECK(ws.messages[0].second == "Hello");
    CHECK(ws.messages[1].second == "Hello");
    CHECK(ws.messages[2] == std::pair{WsMessageType::Binary, std::string("raw")});

    const std::string repetitive(50000, 'r');
    REQUIRE(client->send_websocket(handle, WsMessageType::Text, "first", 5));
    REQUIRE(client->send_websocket(handle, WsMessageType::Text, repetitive.data(), repetitive.size()));
    client->close_websocket(handle, 1000, "");

    auto frames = received.get_future();
    REQUIRE(frames.wait_for(kWait) == std::future_status::ready);
    const auto got = frames.get();
    REQUIRE(got.size() == 3);
    CHECK(got[0].rsv1);
    CHECK(got[0].payload == "first");
    CHECK(got[1].payload == repetitive);
    CHECK(got[2].opcode == 0x8);
    CHECK_FALSE(got[2].rsv1); // control frames are never compressed
}

TEST_CASE("a refused WebSocket upgrade closes with an error", "[client][websocket]") {
    auto server = TestServer::http([](const HttpRequest&) { return http_response(403, "no"); });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    open(client, server.url("/", "ws"), ws);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK_FALSE(ws.open);
    CHECK(ws.error);
    CHECK(ws.code == kWsAbnormalClosure);
    CHECK_THAT(ws.error.message, Catch::Matchers::ContainsSubstring("HTTP 403"));
}

TEST_CASE("a WebSocket server that declares an unknown extension is refused", "[client][websocket]") {
    TestServer server([](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        if (!connection.accept_websocket(*request, "x-unknown")) return;
        connection.read_frame(); // until the client gives up
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    open(client, server.url("/", "ws"), ws, true);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK_FALSE(ws.open);
    CHECK(ws.error.code == ErrorCode::TransportError);
}

TEST_CASE("messages sent before the handshake wait for it", "[client][websocket]") {
    TestServer server([](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        std::this_thread::sleep_for(100ms);
        if (connection.accept_websocket(*request)) echo(connection);
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    const auto handle = open(client, server.url("/", "ws"), ws);
    REQUIRE(client->send_websocket(handle, WsMessageType::Text, "early", 5));
    REQUIRE(ws.wait([](const WsOutcome& w) { return !w.messages.empty() || w.closed; }));
    REQUIRE(ws.messages.size() == 1);
    CHECK(ws.messages[0].second == "early");
    client->close_websocket(handle, 1000, "");
}

TEST_CASE("closing a WebSocket before its handshake aborts it", "[client][websocket]") {
    TestServer server([](Connection& connection) {
        connection.read_request();
        connection.read_request();
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    const auto handle = open(client, server.url("/", "ws"), ws);
    std::this_thread::sleep_for(50ms);
    client->close_websocket(handle, 1000, "");
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK_FALSE(ws.open);
    CHECK(ws.error.code == ErrorCode::Aborted);
    CHECK(ws.code == kWsAbnormalClosure);
}

TEST_CASE("a WebSocket whose connection drops closes abnormally", "[client][websocket]") {
    const bool deflate = GENERATE(false, true);
    CAPTURE(deflate);
    TestServer server([](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        // Returning closes the connection, without a close frame.
        (void)connection.accept_websocket(*request);
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    RunningClient client;
    open(client, server.url("/", "ws"), ws, deflate);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK(ws.error.code == ErrorCode::TransportError);
    CHECK(ws.code == kWsAbnormalClosure);
}

TEST_CASE("an unknown WebSocket handle takes nothing", "[client][websocket]") {
    RunningClient client;
    CHECK_FALSE(client->send_websocket(999, WsMessageType::Text, "x", 1));
    client->close_websocket(999, 1000, "");
}

namespace {

// What timers report, in the order they fire.
struct TimerLog {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<int, bool>> fired; // (id, cancelled)

    struct Entry {
        TimerLog* log;
        int id;
    };

    static void on_timer(void* context, const bool cancelled) {
        const auto* entry = static_cast<Entry*>(context);
        std::scoped_lock lock(entry->log->mutex);
        entry->log->fired.emplace_back(entry->id, cancelled);
        entry->log->changed.notify_all();
    }

    bool wait_for(const std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, kWait, [&] { return fired.size() >= count; });
    }
};

} // namespace

TEST_CASE("timers fire in deadline order, on the loop", "[client][timer]") {
    TimerLog log; // outlives the client, whose last callbacks may come late
    std::array<TimerLog::Entry, 3> entries{{{&log, 0}, {&log, 1}, {&log, 2}}};
    RunningClient client;
    const auto start = std::chrono::steady_clock::now();
    client->set_timer(150ms, entries.data(), &TimerLog::on_timer);
    client->set_timer(50ms, &entries[1], &TimerLog::on_timer);
    client->set_timer(0ms, &entries[2], &TimerLog::on_timer);
    REQUIRE(log.wait_for(3));
    CHECK(std::chrono::steady_clock::now() - start >= 150ms);
    std::scoped_lock lock(log.mutex);
    CHECK(log.fired == std::vector<std::pair<int, bool>>{{2, false}, {1, false}, {0, false}});
}

TEST_CASE("timers and requests share the loop", "[client][timer]") {
    auto server = TestServer::http([](const HttpRequest&) {
        std::this_thread::sleep_for(100ms);
        return http_response(200, "late");
    });
    TimerLog log;
    TimerLog::Entry entry{&log, 0};
    RunningClient client;
    Outcome outcome;
    const auto done = outcome.done.get_future();
    Request request;
    request.url = server.url("/");
    client->send(request, &outcome, nullptr, &on_done);
    client->set_timer(20ms, &entry, &TimerLog::on_timer);
    // The timer is not held up by the slow response.
    REQUIRE(log.wait_for(1));
    CHECK(done.wait_for(0s) == std::future_status::timeout);
    REQUIRE(done.wait_for(kWait) == std::future_status::ready);
    CHECK(outcome.response.body == "late");
}

TEST_CASE("cancel() ends a timer, which reports it", "[client][timer]") {
    TimerLog log;
    std::array<TimerLog::Entry, 2> entries{{{&log, 0}, {&log, 1}}};
    RunningClient client;
    const auto cancelled = client->set_timer(10s, entries.data(), &TimerLog::on_timer);
    client->set_timer(30ms, &entries[1], &TimerLog::on_timer);
    client->cancel(cancelled);
    REQUIRE(log.wait_for(2));
    std::scoped_lock lock(log.mutex);
    CHECK(log.fired == std::vector<std::pair<int, bool>>{{0, true}, {1, false}});
    client->cancel(cancelled); // no longer known: a no-op
}

TEST_CASE("destroying a client cancels its timers", "[client][timer]") {
    TimerLog log;
    TimerLog::Entry entry{&log, 0};
    {
        RunningClient client;
        client->set_timer(10s, &entry, &TimerLog::on_timer);
    }
    std::scoped_lock lock(log.mutex);
    CHECK(log.fired == std::vector<std::pair<int, bool>>{{0, true}});
}
