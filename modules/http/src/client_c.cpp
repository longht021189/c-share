#include "c_share/http/client_c.h"
#include "c_share/http/client_c.hpp"

#include "c_share/http/client.hpp"

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace http = c_share::http;

// libcurl's process-wide state (see client.hpp's notes) must be set up
// exactly once before the first Client and torn down after the last one, so
// count live clients here instead of making every caller do it.
std::mutex g_curl_mutex;
int g_curl_clients = 0;

void curl_acquire() {
    std::scoped_lock lock(g_curl_mutex);
    if (g_curl_clients++ == 0) curl_global_init(CURL_GLOBAL_DEFAULT);
}

void curl_release() {
    std::scoped_lock lock(g_curl_mutex);
    if (--g_curl_clients == 0) curl_global_cleanup();
}

http::Method to_cpp(const c_share_http_method_t method) {
    switch (method) {
        case C_SHARE_HTTP_METHOD_GET: return http::Method::Get;
        case C_SHARE_HTTP_METHOD_POST: return http::Method::Post;
        case C_SHARE_HTTP_METHOD_PUT: return http::Method::Put;
        case C_SHARE_HTTP_METHOD_PATCH: return http::Method::Patch;
        case C_SHARE_HTTP_METHOD_DELETE: return http::Method::Delete;
        case C_SHARE_HTTP_METHOD_HEAD: return http::Method::Head;
        case C_SHARE_HTTP_METHOD_OPTIONS: return http::Method::Options;
    }
    return http::Method::Get;
}

c_share_http_error_code_t to_c(const http::ErrorCode code) {
    switch (code) {
        case http::ErrorCode::None: return C_SHARE_HTTP_ERROR_NONE;
        case http::ErrorCode::InvalidUrl: return C_SHARE_HTTP_ERROR_INVALID_URL;
        case http::ErrorCode::CouldNotResolveHost: return C_SHARE_HTTP_ERROR_COULD_NOT_RESOLVE_HOST;
        case http::ErrorCode::CouldNotConnect: return C_SHARE_HTTP_ERROR_COULD_NOT_CONNECT;
        case http::ErrorCode::TlsError: return C_SHARE_HTTP_ERROR_TLS_ERROR;
        case http::ErrorCode::Timeout: return C_SHARE_HTTP_ERROR_TIMEOUT;
        case http::ErrorCode::Aborted: return C_SHARE_HTTP_ERROR_ABORTED;
        case http::ErrorCode::TransportError: return C_SHARE_HTTP_ERROR_TRANSPORT_ERROR;
    }
    return C_SHARE_HTTP_ERROR_TRANSPORT_ERROR;
}

// What the C++ callbacks get as their `context`: the caller's own context
// and C callbacks, owned by the completion trampoline (see below) since a
// C++ function pointer has nowhere else to carry them.
struct Trampoline {
    void* context;
    c_share_http_head_cb head_cb;
    c_share_http_body_cb body_cb;
    c_share_http_completion_cb done_cb;
};

// Views `headers` as the C array the callbacks take; the vector must outlive
// the call it is handed to.
std::vector<c_share_http_header_t> to_c_headers(const std::vector<http::Header>& headers) {
    std::vector<c_share_http_header_t> out;
    out.reserve(headers.size());
    for (const auto& [name, value] : headers) {
        out.push_back({name.data(), name.size(), value.data(), value.size()});
    }
    return out;
}

// The trampolines take the parameters of the C++ callback types they are,
// even where they could take them as const or by reference.

// ReSharper disable once CppParameterMayBeConstPtrOrRef
http::Action head_trampoline(void* context, const http::Response& head) {
    const auto* trampoline = static_cast<const Trampoline*>(context);
    const auto headers = to_c_headers(head.headers);
    const auto action = trampoline->head_cb(trampoline->context, head.status_code,
                                            headers.empty() ? nullptr : headers.data(), headers.size());
    return action == C_SHARE_HTTP_ACTION_ABORT ? http::Action::Abort : http::Action::Continue;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
http::Action body_trampoline(void* context, const char* data, const std::size_t size) {
    const auto* trampoline = static_cast<const Trampoline*>(context);
    const auto action = trampoline->body_cb(trampoline->context, reinterpret_cast<const uint8_t*>(data), size);
    return action == C_SHARE_HTTP_ACTION_ABORT ? http::Action::Abort : http::Action::Continue;
}

// Runs exactly once per handle send() returned (see CompletionCallback in
// client.hpp), so it is where the Trampoline made in send() gets freed.
// ReSharper disable CppPassValueParameterByConstReference
void completion_trampoline(void* context, http::Response response, http::Error error) {
    // ReSharper restore CppPassValueParameterByConstReference
    const std::unique_ptr<Trampoline> trampoline(static_cast<Trampoline*>(context));

    const auto headers = to_c_headers(response.headers);

    c_share_http_response_t c_response{};
    c_response.status_code = response.status_code;
    c_response.headers = headers.empty() ? nullptr : headers.data();
    c_response.headers_len = headers.size();
    c_response.body = reinterpret_cast<const uint8_t*>(response.body.data());
    c_response.body_len = response.body.size();

    c_share_http_error_t c_error{};
    c_error.code = to_c(error.code);
    c_error.message = error.message.data();
    c_error.message_len = error.message.size();

    trampoline->done_cb(trampoline->context, c_response, c_error);
}

// The WebSocket counterpart of Trampoline, freed by ws_close_trampoline.
struct WsTrampoline {
    void* context;
    c_share_http_ws_open_cb open_cb;
    c_share_http_ws_message_cb message_cb;
    c_share_http_ws_close_cb close_cb;
};

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void ws_open_trampoline(void* context, const http::Response& head) {
    const auto* trampoline = static_cast<const WsTrampoline*>(context);
    if (!trampoline->open_cb) return;
    const auto headers = to_c_headers(head.headers);
    trampoline->open_cb(trampoline->context, head.status_code, headers.empty() ? nullptr : headers.data(),
                        headers.size());
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void ws_message_trampoline(void* context, const http::WsMessageType type, const char* data, const std::size_t size) {
    const auto* trampoline = static_cast<const WsTrampoline*>(context);
    if (!trampoline->message_cb) return;
    trampoline->message_cb(trampoline->context,
                           type == http::WsMessageType::Binary ? C_SHARE_HTTP_WS_BINARY : C_SHARE_HTTP_WS_TEXT,
                           reinterpret_cast<const uint8_t*>(data), size);
}

// ReSharper disable once CppPassValueParameterByConstReference
void ws_close_trampoline(void* context, http::Error error, const std::uint16_t code, const std::string& reason) {
    const std::unique_ptr<WsTrampoline> trampoline(static_cast<WsTrampoline*>(context));
    c_share_http_error_t c_error{};
    c_error.code = to_c(error.code);
    c_error.message = error.message.data();
    c_error.message_len = error.message.size();
    trampoline->close_cb(trampoline->context, c_error, code, reason.data(), reason.size());
}

// The C++ request for a C one; see c_share_http_client_send.
http::Request to_cpp(const c_share_http_request_t& request) {
    http::Request cpp_request;
    cpp_request.url.assign(request.url, request.url_len);
    cpp_request.method = to_cpp(request.method);
    cpp_request.headers.reserve(request.headers_len);
    for (std::size_t i = 0; i < request.headers_len; ++i) {
        const auto& [name, name_len, value, value_len] = request.headers[i];
        cpp_request.headers.push_back({std::string(name, name_len), std::string(value, value_len)});
    }
    if (request.body_len > 0) {
        cpp_request.body.assign(reinterpret_cast<const char*>(request.body), request.body_len);
    }
    cpp_request.timeout = std::chrono::milliseconds(request.timeout_ms);
    cpp_request.follow_redirects = request.follow_redirects != 0;
    if (request.pinned_public_key) {
        cpp_request.pinned_public_key = std::string(request.pinned_public_key, request.pinned_public_key_len);
    }
    return cpp_request;
}

} // namespace

// The type client_c.h declares, so it cannot move into the namespace above.
// ReSharper disable once CppUseInternalLinkage
struct c_share_http_client {
    http::Client client;
    std::thread loop; // runs client.run(); started in create, joined in destroy
};

namespace c_share::http {

Client& client_of(c_share_http_client_t* client) {
    return client->client;
}

} // namespace c_share::http

extern "C" {

c_share_http_client_t* c_share_http_client_create(void) {
    curl_acquire();
    try {
        auto client = std::make_unique<c_share_http_client>();
        client->loop = std::thread([raw = client.get()] { raw->client.run(); });
        return client.release();
    } catch (...) {
        curl_release();
        return nullptr;
    }
}

void c_share_http_client_destroy(c_share_http_client_t* client) {
    if (!client) return;
    client->client.stop();
    if (client->loop.joinable()) client->loop.join();
    delete client;
    curl_release();
}

uint64_t c_share_http_client_send(c_share_http_client_t* client, const c_share_http_request_t* request, void* context,
                                  const c_share_http_body_cb body_cb, const c_share_http_completion_cb done_cb,
                                  const c_share_http_head_cb head_cb) {
    if (!client || !request || !done_cb) return 0;

    // No exception may cross this extern "C" boundary: everything below
    // that can throw (allocation, Client::send's own runtime_errors) is
    // inside the try.
    try {
        http::Request cpp_request = to_cpp(*request);

        auto trampoline = std::make_unique<Trampoline>(Trampoline{context, head_cb, body_cb, done_cb});
        const http::Client::Handle handle =
            client->client.send(std::move(cpp_request), trampoline.get(), body_cb ? &body_trampoline : nullptr,
                                &completion_trampoline, head_cb ? &head_trampoline : nullptr);
        // send() succeeded: from here on completion_trampoline owns it.
        trampoline.release(); // NOLINT(bugprone-unused-return-value): the callbacks own it now
        return handle;
    } catch (...) {
        return 0; // Client::Handle starts at 1, so 0 is never a real handle
    }
}

void c_share_http_client_cancel(c_share_http_client_t* client, const uint64_t handle) {
    if (!client || handle == 0) return;
    client->client.cancel(handle);
}

uint64_t c_share_http_client_set_timer(c_share_http_client_t* client, const uint64_t delay_ms, void* context,
                                       const c_share_http_timer_cb callback) {
    if (!client || !callback) return 0;
    struct Timer {
        void* context;
        c_share_http_timer_cb callback;
    };
    auto* timer = new Timer{context, callback};
    return client->client.set_timer(std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(delay_ms)),
                                    timer, [](void* raw, const bool cancelled) {
                                        const std::unique_ptr<Timer> owned(static_cast<Timer*>(raw));
                                        owned->callback(owned->context, cancelled ? 1 : 0);
                                    });
}

uint64_t c_share_http_client_ws_open(c_share_http_client_t* client, const c_share_http_request_t* request,
                                     const c_share_http_ws_options_t* options, void* context,
                                     const c_share_http_ws_open_cb open_cb, const c_share_http_ws_message_cb message_cb,
                                     const c_share_http_ws_close_cb close_cb) {
    if (!client || !request || !close_cb) return 0;
    try {
        http::WsOptions cpp_options;
        if (options) cpp_options.deflate = options->deflate != 0;
        auto trampoline = std::make_unique<WsTrampoline>(WsTrampoline{context, open_cb, message_cb, close_cb});
        const http::Client::Handle handle =
            client->client.open_websocket(to_cpp(*request), trampoline.get(), &ws_open_trampoline,
                                          &ws_message_trampoline, &ws_close_trampoline, cpp_options);
        // Submitted: from here on ws_close_trampoline owns it.
        trampoline.release(); // NOLINT(bugprone-unused-return-value): the callbacks own it now
        return handle;
    } catch (...) {
        return 0;
    }
}

int c_share_http_client_ws_send(c_share_http_client_t* client, const uint64_t handle, const c_share_http_ws_type_t type,
                                const uint8_t* data, const size_t size) {
    if (!client || handle == 0 || (!data && size > 0)) return -1;
    try {
        const auto cpp_type = type == C_SHARE_HTTP_WS_BINARY ? http::WsMessageType::Binary : http::WsMessageType::Text;
        return client->client.send_websocket(handle, cpp_type, reinterpret_cast<const char*>(data), size) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

void c_share_http_client_ws_close(c_share_http_client_t* client, const uint64_t handle, const uint16_t code,
                                  const char* reason, const size_t reason_len) {
    if (!client || handle == 0) return;
    try {
        client->client.close_websocket(handle, code, reason ? std::string(reason, reason_len) : std::string());
    } catch (...) { // NOLINT(bugprone-empty-catch): allocation failure, nothing to do across a C boundary
    }
}

} // extern "C"
