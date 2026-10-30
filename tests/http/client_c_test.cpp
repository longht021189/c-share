#include "c_share/http/client_c.h"

#include "test_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using c_share::test::Connection;
using c_share::test::http_response;
using c_share::test::HttpRequest;
using c_share::test::TestServer;
using namespace std::chrono_literals;

namespace {

constexpr auto kWait = 10s;

// A client from c_share_http_client_create, destroyed at the end of the test.
struct ClientHandle {
    ClientHandle() : client(c_share_http_client_create()) { REQUIRE(client); }
    ~ClientHandle() { c_share_http_client_destroy(client); }
    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;
    ClientHandle(ClientHandle&&) = delete;
    ClientHandle& operator=(ClientHandle&&) = delete;

    c_share_http_client_t* client;
};

// What the C callbacks of one request report, copied out of their arguments.
struct Outcome {
    long head_status = 0;
    std::vector<std::pair<std::string, std::string>> head_headers;
    std::string streamed;
    long status = 0;
    std::string body;
    c_share_http_error_code_t error = C_SHARE_HTTP_ERROR_NONE;
    std::string message;
    std::promise<void> done;
};

c_share_http_action_t on_head(void* context, const long status_code, const c_share_http_header_t* headers,
                              const size_t headers_len) {
    auto* outcome = static_cast<Outcome*>(context);
    outcome->head_status = status_code;
    for (size_t i = 0; i < headers_len; ++i) {
        outcome->head_headers.emplace_back(std::string(headers[i].name, headers[i].name_len),
                                           std::string(headers[i].value, headers[i].value_len));
    }
    return C_SHARE_HTTP_ACTION_CONTINUE;
}

c_share_http_action_t on_body(void* context, const uint8_t* data, const size_t size) {
    static_cast<Outcome*>(context)->streamed.append(reinterpret_cast<const char*>(data), size);
    return C_SHARE_HTTP_ACTION_CONTINUE;
}

// By value, as the C API's callback type takes them.
// ReSharper disable CppPassValueParameterByConstReference
void on_done(void* context, const c_share_http_response_t response, const c_share_http_error_t error) {
    auto* outcome = static_cast<Outcome*>(context);
    outcome->status = response.status_code;
    outcome->body.assign(reinterpret_cast<const char*>(response.body), response.body_len);
    outcome->error = error.code;
    outcome->message.assign(error.message, error.message_len);
    outcome->done.set_value();
}

c_share_http_request_t get_request(const std::string& url) {
    c_share_http_request_t request{};
    request.url = url.data();
    request.url_len = url.size();
    request.method = C_SHARE_HTTP_METHOD_GET;
    return request;
}

struct WsOutcome {
    std::mutex mutex;
    std::condition_variable changed;
    bool opened = false;
    std::vector<std::pair<c_share_http_ws_type_t, std::string>> messages;
    bool closed = false;
    c_share_http_error_code_t error = C_SHARE_HTTP_ERROR_NONE;
    uint16_t code = 0;
    std::string reason;

    template <typename Predicate> bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, kWait, [&] { return predicate(*this); });
    }
};

void on_ws_open(void* context, long /*status_code*/, const c_share_http_header_t* /*headers*/, size_t /*headers_len*/) {
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->opened = true;
    ws->changed.notify_all();
}

void on_ws_message(void* context, const c_share_http_ws_type_t type, const uint8_t* data, const size_t size) {
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->messages.emplace_back(type, std::string(reinterpret_cast<const char*>(data), size));
    ws->changed.notify_all();
}

void on_ws_close(void* context, const c_share_http_error_t error, const uint16_t code, const char* reason,
                 const size_t reason_len) {
    // ReSharper restore CppPassValueParameterByConstReference
    auto* ws = static_cast<WsOutcome*>(context);
    std::scoped_lock lock(ws->mutex);
    ws->closed = true;
    ws->error = error.code;
    ws->code = code;
    ws->reason.assign(reason, reason_len);
    ws->changed.notify_all();
}

} // namespace

TEST_CASE("the C API sends a request and reports its response", "[client_c]") {
    auto server = TestServer::http([](const HttpRequest& request) {
        return http_response(200, request.method + " " + request.body, {{"X-Test", "yes"}});
    });
    ClientHandle handle;
    const std::string url = server.url("/");
    c_share_http_request_t request = get_request(url);
    request.method = C_SHARE_HTTP_METHOD_POST;
    const std::string body = "payload";
    request.body = reinterpret_cast<const uint8_t*>(body.data());
    request.body_len = body.size();
    constexpr c_share_http_header_t header{"X-Custom", 8, "1", 1};
    request.headers = &header;
    request.headers_len = 1;

    Outcome outcome;
    auto done = outcome.done.get_future();
    CHECK(c_share_http_client_send(handle.client, &request, &outcome, nullptr, &on_done, &on_head) != 0);
    REQUIRE(done.wait_for(kWait) == std::future_status::ready);

    CHECK(outcome.error == C_SHARE_HTTP_ERROR_NONE);
    CHECK(outcome.status == 200);
    CHECK(outcome.body == "POST payload");
    CHECK(outcome.head_status == 200);
    CHECK(std::find(outcome.head_headers.begin(), outcome.head_headers.end(),
                    std::pair<std::string, std::string>{"X-Test", "yes"}) != outcome.head_headers.end());
}

TEST_CASE("the C API streams a body to its body callback", "[client_c]") {
    auto server = TestServer::http([](const HttpRequest&) { return http_response(200, std::string(50000, 'b')); });
    ClientHandle handle;
    const std::string url = server.url("/");
    const c_share_http_request_t request = get_request(url);
    Outcome outcome;
    auto done = outcome.done.get_future();
    REQUIRE(c_share_http_client_send(handle.client, &request, &outcome, &on_body, &on_done, nullptr) != 0);
    REQUIRE(done.wait_for(kWait) == std::future_status::ready);
    CHECK(outcome.body.empty());
    CHECK(outcome.streamed == std::string(50000, 'b'));
}

TEST_CASE("the C API maps transport errors and cancellation", "[client_c]") {
    Outcome outcome;
    auto done = outcome.done.get_future();
    ClientHandle handle;

    SECTION("nothing listening") {
        const std::string url = "http://127.0.0.1:" + std::to_string(c_share::test::unused_port()) + "/";
        const c_share_http_request_t request = get_request(url);
        REQUIRE(c_share_http_client_send(handle.client, &request, &outcome, nullptr, &on_done, nullptr) != 0);
        REQUIRE(done.wait_for(kWait) == std::future_status::ready);
        CHECK(outcome.error == C_SHARE_HTTP_ERROR_COULD_NOT_CONNECT);
        CHECK_FALSE(outcome.message.empty());
    }
    SECTION("cancelled") {
        TestServer server([](Connection& connection) {
            connection.read_request();
            connection.read_request();
        });
        const std::string url = server.url("/");
        const c_share_http_request_t request = get_request(url);
        const uint64_t id = c_share_http_client_send(handle.client, &request, &outcome, nullptr, &on_done, nullptr);
        REQUIRE(id != 0);
        std::this_thread::sleep_for(50ms);
        c_share_http_client_cancel(handle.client, id);
        REQUIRE(done.wait_for(kWait) == std::future_status::ready);
        CHECK(outcome.error == C_SHARE_HTTP_ERROR_ABORTED);
    }
}

TEST_CASE("the C API refuses missing arguments", "[client_c]") {
    ClientHandle handle;
    const std::string url = "http://127.0.0.1/";
    const c_share_http_request_t request = get_request(url);
    Outcome outcome;
    CHECK(c_share_http_client_send(nullptr, &request, &outcome, nullptr, &on_done, nullptr) == 0);
    CHECK(c_share_http_client_send(handle.client, nullptr, &outcome, nullptr, &on_done, nullptr) == 0);
    CHECK(c_share_http_client_send(handle.client, &request, &outcome, nullptr, nullptr, nullptr) == 0);
    CHECK(c_share_http_client_ws_open(handle.client, &request, nullptr, nullptr, nullptr, nullptr, nullptr) == 0);
    CHECK(c_share_http_client_ws_send(handle.client, 0, C_SHARE_HTTP_WS_TEXT, nullptr, 0) == -1);
    CHECK(c_share_http_client_ws_send(handle.client, 1, C_SHARE_HTTP_WS_TEXT, nullptr, 5) == -1);
    // No-ops.
    c_share_http_client_cancel(handle.client, 0);
    c_share_http_client_ws_close(handle.client, 0, 1000, nullptr, 0);
    c_share_http_client_destroy(nullptr);
}

TEST_CASE("destroying a C client fails what is still in flight", "[client_c]") {
    TestServer server([](Connection& connection) {
        connection.read_request();
        connection.read_request();
    });
    const std::string url = server.url("/");
    const c_share_http_request_t request = get_request(url);
    Outcome outcome;
    auto done = outcome.done.get_future();
    c_share_http_client_t* client = c_share_http_client_create();
    REQUIRE(client);
    REQUIRE(c_share_http_client_send(client, &request, &outcome, nullptr, &on_done, nullptr) != 0);
    std::this_thread::sleep_for(50ms);
    c_share_http_client_destroy(client);
    REQUIRE(done.wait_for(0s) == std::future_status::ready);
    CHECK(outcome.error == C_SHARE_HTTP_ERROR_ABORTED);
}

TEST_CASE("the C API opens, uses and closes a WebSocket", "[client_c][websocket]") {
    const int deflate = GENERATE(0, 1);
    CAPTURE(deflate);
    std::promise<std::string> close_payload;
    TestServer server([&close_payload](Connection& connection) {
        const auto request = connection.read_request();
        if (!request) return;
        if (!connection.accept_websocket(*request)) return;
        while (const auto frame = connection.read_frame()) {
            if (frame->opcode == 0x8) {
                close_payload.set_value(frame->payload);
                return;
            }
            if (!connection.write_frame(*frame)) return;
        }
    });
    WsOutcome ws; // outlives the client, whose last callbacks may come late
    ClientHandle handle;
    const std::string url = server.url("/", "ws");
    const c_share_http_request_t request = get_request(url);
    c_share_http_ws_options_t options{};
    options.deflate = deflate;
    const uint64_t id =
        c_share_http_client_ws_open(handle.client, &request, &options, &ws, &on_ws_open, &on_ws_message, &on_ws_close);
    REQUIRE(id != 0);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.opened || w.closed; }));
    REQUIRE(ws.opened);

    constexpr uint8_t binary[] = {0, 1, 2};
    CHECK(c_share_http_client_ws_send(handle.client, id, C_SHARE_HTTP_WS_TEXT, reinterpret_cast<const uint8_t*>("hi"),
                                      2) == 0);
    CHECK(c_share_http_client_ws_send(handle.client, id, C_SHARE_HTTP_WS_BINARY, binary, sizeof binary) == 0);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.messages.size() == 2; }));
    CHECK(ws.messages[0] == std::pair{C_SHARE_HTTP_WS_TEXT, std::string("hi")});
    CHECK(ws.messages[1] == std::pair{C_SHARE_HTTP_WS_BINARY, std::string("\x00\x01\x02", 3)});

    c_share_http_client_ws_close(handle.client, id, 4001, "bye", 3);
    REQUIRE(ws.wait([](const WsOutcome& w) { return w.closed; }));
    CHECK(ws.error == C_SHARE_HTTP_ERROR_NONE);
    CHECK(ws.code == 4001);
    CHECK(ws.reason == "bye");
    auto payload = close_payload.get_future();
    REQUIRE(payload.wait_for(kWait) == std::future_status::ready);
    CHECK(payload.get() == std::string("\x0f\xa1", 2) + "bye");
    CHECK(c_share_http_client_ws_send(handle.client, id, C_SHARE_HTTP_WS_TEXT, reinterpret_cast<const uint8_t*>("x"),
                                      1) == -1);
}

TEST_CASE("the C API's timers fire and cancel", "[client_c][timer]") {
    struct Log {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<int> cancelled;
    } log;
    {
        const ClientHandle handle;
        const auto on_timer = [](void* context, const int cancelled) {
            auto* l = static_cast<Log*>(context);
            std::scoped_lock lock(l->mutex);
            l->cancelled.push_back(cancelled);
            l->changed.notify_all();
        };
        CHECK(c_share_http_client_set_timer(handle.client, 0, &log, nullptr) == 0);
        CHECK(c_share_http_client_set_timer(nullptr, 0, &log, on_timer) == 0);
        REQUIRE(c_share_http_client_set_timer(handle.client, 10, &log, on_timer) != 0);
        {
            std::unique_lock lock(log.mutex);
            REQUIRE(log.changed.wait_for(lock, 10s, [&] { return log.cancelled.size() == 1; }));
        }
        const uint64_t pending = c_share_http_client_set_timer(handle.client, 10000, &log, on_timer);
        c_share_http_client_cancel(handle.client, pending);
        std::unique_lock lock(log.mutex);
        REQUIRE(log.changed.wait_for(lock, 10s, [&] { return log.cancelled.size() == 2; }));
    }
    CHECK(log.cancelled == std::vector<int>{0, 1});
}
