#include "c_share/http/client.hpp"

#include "c_share/waker/waker.hpp"
#include "ws_codec.hpp"

#include <curl/curl.h>

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

// Client's methods all change it through impl_, where CLion does not look, so
// it would make each of them const.
// ReSharper disable CppMemberFunctionMayBeConst

namespace c_share::http {

namespace {

ErrorCode map_curl_code(const CURLcode code) {
    switch (code) {
        case CURLE_OK: return ErrorCode::None;
        case CURLE_URL_MALFORMAT: return ErrorCode::InvalidUrl;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST: return ErrorCode::CouldNotResolveHost;
        case CURLE_COULDNT_CONNECT: return ErrorCode::CouldNotConnect;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_PEER_FAILED_VERIFICATION: // formerly CURLE_SSL_CACERT (same value)
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH: return ErrorCode::TlsError;
        case CURLE_OPERATION_TIMEDOUT: return ErrorCode::Timeout;
        case CURLE_WRITE_ERROR:         // BodyCallback returned Action::Abort
        case CURLE_ABORTED_BY_CALLBACK: // cancel()
            return ErrorCode::Aborted;
        default: return ErrorCode::TransportError;
    }
}

// Header names are case-insensitive.
bool name_equals(const std::string_view name, const std::string_view expected) {
    return name.size() == expected.size() &&
           std::equal(name.begin(), name.end(), expected.begin(), [](const char a, const char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

bool has_header(const std::vector<Header>& headers, std::string_view name) {
    return std::any_of(headers.begin(), headers.end(),
                       [name](const Header& header) { return name_equals(header.name, name); });
}

// Whether the header block that just ended is the one the caller should see:
// 1xx interim responses are followed by the real one, and with
// follow_redirects a 3xx with a Location is a redirect libcurl will follow
// (304 has no Location to follow, so it is final).
bool is_final_hop(const Response& response, const bool follow_redirects) {
    const long status = response.status_code;
    if (status >= 100 && status < 200) return false;
    if (follow_redirects && status >= 300 && status < 400 && status != 304) {
        return !has_header(response.headers, "location");
    }
    return true;
}

// The codings libcurl can decode in this build, as reqwest's default
// gzip/deflate/brotli features advertise them -- "gzip,deflate,br", minus
// anything this libcurl lacks (advertising a coding we cannot decode would
// hand the caller raw compressed bytes).
std::string accept_encoding_value() {
    const auto features = curl_version_info(CURLVERSION_NOW)->features;
    std::string value;
    const auto add = [&value](const char* coding) {
        if (!value.empty()) value += ',';
        value += coding;
    };
    if (features & CURL_VERSION_LIBZ) {
        add("gzip");
        add("deflate");
    }
    if (features & CURL_VERSION_BROTLI) add("br");
    return value;
}

// Whether libcurl decodes a body with this Content-Encoding, so that the
// header describing the compressed body no longer applies. Matches reqwest:
// the exact lowercase coding, and only when this build can decode it.
bool is_decoded_by_libcurl(const std::string_view coding) {
    const auto features = curl_version_info(CURLVERSION_NOW)->features;
    if (coding == "gzip" || coding == "deflate") return features & CURL_VERSION_LIBZ;
    if (coding == "br") return features & CURL_VERSION_BROTLI;
    return false;
}

// Once libcurl has decoded the body, drop the Content-Encoding and
// Content-Length that described the compressed bytes -- what reqwest does.
void drop_headers_describing_the_encoded_body(std::vector<Header>& headers) {
    const auto encoding = std::find_if(headers.begin(), headers.end(), [](const Header& header) {
        return name_equals(header.name, "content-encoding");
    });
    if (encoding == headers.end() || !is_decoded_by_libcurl(encoding->value)) return;
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const Header& header) {
                                     return name_equals(header.name, "content-encoding") ||
                                            name_equals(header.name, "content-length");
                                 }),
                  headers.end());
}

const char* method_verb(const Method method) {
    switch (method) {
        case Method::Get: return "GET";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Patch: return "PATCH";
        case Method::Delete: return "DELETE";
        case Method::Head: return "HEAD";
        case Method::Options: return "OPTIONS";
    }
    return "GET";
}

// RFC 6455's "no status received": what a close frame without a payload
// reports, and never sent on the wire itself.
constexpr std::uint16_t kWsNoStatus = 1005;
// A close frame's payload is at most 125 bytes, two of them the code.
constexpr std::size_t kWsMaxCloseReason = 123;

// The payload of a close frame with `code` and `reason`.
std::string ws_close_payload(const std::uint16_t code, const std::string& reason) {
    if (code == kWsNoStatus) return {};
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xff));
    payload.append(reason, 0, kWsMaxCloseReason);
    return payload;
}

} // namespace

struct Client::Impl {
    struct WsCallbacks {
        void* context = nullptr;
        WsOpenCallback on_open = nullptr;
        WsMessageCallback on_message = nullptr;
        WsCloseCallback on_close = nullptr;
    };

    // One frame waiting in a WebSocket's outbox: its CURLWS_* flags, its
    // payload, and how much of it libcurl has taken so far.
    struct WsFrame {
        unsigned int flags = 0;
        std::string data;
        std::size_t offset = 0;
        // With a codec, `data` has been replaced by the frame's wire bytes.
        bool encoded = false;
    };

    // An open WebSocket: the CONNECT_ONLY transfer that did the handshake
    // stays in `multi` (removing it would close the connection), and its
    // socket is watched here instead of by libcurl.
    struct WsState {
        Handle handle = 0;
        CURL* easy = nullptr;
        waker::socket_t fd{};
        WsCallbacks callbacks;
        std::deque<WsFrame> outbox;
        // close_websocket() was called: its close frame is the outbox's last.
        bool closing = false;
        std::uint16_t close_code = kWsNoStatus;
        std::string close_reason;
        // The data message being received, across frames and recv calls.
        bool in_message = false;
        WsMessageType message_type = WsMessageType::Text;
        std::string message;
        // The close frame being received (it may arrive in pieces too).
        std::string close_frame;
        // Set when permessage-deflate was offered: frames are then encoded
        // and decoded here, and the connection read and written raw.
        std::unique_ptr<detail::WsCodec> codec;
    };

    // A WebSocket callback to run once `mutex` is released, in order.
    struct WsEvent {
        enum class Kind {
            Open,
            Message,
            Close
        };

        WsEvent(const Kind kind, const WsCallbacks& callbacks) : kind(kind), callbacks(callbacks) {}

        Kind kind;
        WsCallbacks callbacks;
        Response head;                            // Open
        WsMessageType type = WsMessageType::Text; // Message
        std::string data;                         // Message
        Error error;                              // Close
        std::uint16_t code = kWsAbnormalClosure;  // Close
        std::string reason;                       // Close
    };

    struct RequestState {
        Handle handle = 0;
        Request request; // owned copy: keeps url/body/header strings alive for curl
        void* context = nullptr;
        HeadCallback head_cb = nullptr;
        BodyCallback body_cb = nullptr;
        CompletionCallback done_cb = nullptr;

        curl_slist* header_list = nullptr;
        Response response;

        // Set for a WebSocket handshake (see open_websocket), which has no
        // done_cb: once it finishes, the transfer becomes a WsState.
        std::optional<WsCallbacks> ws;
        WsOptions ws_options;
        // Messages queued (send_websocket) before the handshake finished.
        std::deque<WsFrame> ws_outbox;
    };

    struct Finished {
        void* context = nullptr;
        CompletionCallback done_cb = nullptr;
        Response response;
        Error error;
    };

    using Clock = std::chrono::steady_clock;

    struct Timer {
        Clock::time_point deadline;
        void* context = nullptr;
        TimerCallback callback = nullptr;
    };
    // A timer that is due or cancelled, for run() to call outside `mutex`.
    struct FiredTimer {
        void* context = nullptr;
        TimerCallback callback = nullptr;
        bool cancelled = false;
    };

    CURLM* multi = nullptr;
    waker::Waker waker;

    std::mutex mutex; // guards multi, requests, handles, next_handle, the timers
    std::unordered_map<CURL*, RequestState> requests;
    std::unordered_map<Handle, CURL*> handles;
    // Open WebSockets, by handle and by socket. A handshake still in flight
    // is in `requests`/`handles` like any other transfer.
    std::unordered_map<Handle, WsState> websockets;
    std::unordered_map<waker::socket_t, Handle> ws_handles_by_fd;
    // WebSocket callbacks waiting for run() to release `mutex`.
    std::vector<WsEvent> ws_events;
    Handle next_handle = 1;
    // set_timer()'s timers, by handle and in deadline order; the waker's one
    // timer is armed for the earliest of these and libcurl's own deadline.
    std::unordered_map<Handle, Timer> timers;
    std::set<std::pair<Clock::time_point, Handle>> timer_queue;
    std::optional<Clock::time_point> curl_deadline;
    std::vector<FiredTimer> fired_timers;
    // Starts true, not in run(): a stop() that lands before run() starts must
    // stick, or run() would never return. Atomic: run() checks it again after
    // releasing `mutex`, while stop() may be clearing it on another thread.
    std::atomic<bool> running{true};

    Impl() : multi(curl_multi_init()) {
        if (!multi) throw std::runtime_error("curl_multi_init failed");

        curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, &Impl::socket_callback);
        curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, &Impl::timer_callback);
        curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // Callbacks must not throw (see client.hpp), so only running out of
    // memory could escape here -- terminating, as a destructor is noexcept.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Impl() {
        // Best-effort cleanup for anything still in flight. The caller is
        // responsible for having stopped run() before destroying the
        // Client; this does not attempt to synchronize with a
        // concurrently-running run() call.
        std::vector<Finished> abandoned;
        {
            std::scoped_lock lock(mutex);
            const Error destroyed{ErrorCode::Aborted, "Client destroyed while in flight"};
            for (auto& [easy, state] : requests) {
                curl_multi_remove_handle(multi, easy);
                if (state.header_list) curl_slist_free_all(state.header_list);
                curl_easy_cleanup(easy);
                if (state.ws) {
                    ws_events.push_back(close_event(*state.ws, destroyed, kWsAbnormalClosure, {}));
                } else {
                    abandoned.push_back(Finished{state.context, state.done_cb, std::move(state.response), destroyed});
                }
            }
            requests.clear();
            handles.clear();
            for (auto& [handle, ws] : websockets) {
                waker.detach_socket(ws.fd);
                curl_multi_remove_handle(multi, ws.easy);
                curl_easy_cleanup(ws.easy);
                ws_events.push_back(close_event(ws.callbacks, destroyed, kWsAbnormalClosure, {}));
            }
            websockets.clear();
            ws_handles_by_fd.clear();
            for (const auto& [handle, timer] : timers)
                fired_timers.push_back(FiredTimer{timer.context, timer.callback, true});
            timers.clear();
            timer_queue.clear();
        }
        for (auto& [context, done_cb, response, error] : abandoned) {
            if (done_cb) done_cb(context, std::move(response), error);
        }
        dispatch_ws_events(take_ws_events());
        dispatch_timers(fired_timers);

        if (multi) curl_multi_cleanup(multi);
    }

    // Drains CURLMSG_DONE notifications for finished transfers. Must be
    // called while `mutex` is held; the caller dispatches the returned
    // completions after releasing it, so a done_cb() may freely call
    // send()/cancel() without deadlocking.
    std::vector<Finished> collect_finished() {
        std::vector<Finished> finished;
        const CURLMsg* msg = nullptr;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(multi, &msgs_left)) != nullptr) {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL* easy = msg->easy_handle;
            const auto it = requests.find(easy);
            if (it == requests.end()) continue; // already removed via cancel()

            RequestState state = std::move(it->second);
            requests.erase(it);
            handles.erase(state.handle);
            if (state.ws) {
                finish_ws_handshake(easy, std::move(state), msg->data.result);
                continue;
            }
            curl_multi_remove_handle(multi, easy);

            Error error;
            if (msg->data.result == CURLE_OK) {
                long status = 0;
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
                state.response.status_code = status;
            } else {
                error.code = map_curl_code(msg->data.result);
                error.message = curl_easy_strerror(msg->data.result);
            }

            if (state.header_list) curl_slist_free_all(state.header_list);
            curl_easy_cleanup(easy);

            finished.push_back(Finished{state.context, state.done_cb, std::move(state.response), std::move(error)});
        }
        return finished;
    }

    static WsEvent close_event(const WsCallbacks& callbacks, Error error, const std::uint16_t code,
                               std::string reason) {
        WsEvent event{WsEvent::Kind::Close, callbacks};
        event.error = std::move(error);
        event.code = code;
        event.reason = std::move(reason);
        return event;
    }

    std::vector<WsEvent> take_ws_events() {
        std::vector<WsEvent> events;
        events.swap(ws_events);
        return events;
    }

    // Runs the callbacks of `events`. Must be called without `mutex` held.
    static void dispatch_ws_events(std::vector<WsEvent> events) {
        for (auto& event : events) {
            // By name: CLion would then count none of WsCallbacks' fields as used.
            // ReSharper disable once CppUseStructuredBinding
            const auto& cb = event.callbacks;
            switch (event.kind) {
                case WsEvent::Kind::Open:
                    if (cb.on_open) cb.on_open(cb.context, event.head);
                    break;
                case WsEvent::Kind::Message:
                    if (cb.on_message) cb.on_message(cb.context, event.type, event.data.data(), event.data.size());
                    break;
                case WsEvent::Kind::Close:
                    cb.on_close(cb.context, std::move(event.error), event.code, event.reason);
                    break;
            }
        }
    }

    // A WebSocket handshake's transfer is done (CURLMSG_DONE, `result`):
    // either the connection is now an open WebSocket, or it failed. Called
    // with `mutex` held, `state` already out of `requests`/`handles`.
    void finish_ws_handshake(CURL* easy, RequestState state, const CURLcode result) {
        if (!state.ws) return; // only ever called for a WebSocket's transfer
        const WsCallbacks callbacks = *state.ws;
        if (state.header_list) curl_slist_free_all(state.header_list);
        state.header_list = nullptr;

        curl_socket_t socket = CURL_SOCKET_BAD;
        if (result == CURLE_OK) curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &socket);
        if (result != CURLE_OK || socket == CURL_SOCKET_BAD) {
            Error error{map_curl_code(result), curl_easy_strerror(result)};
            if (error.code == ErrorCode::None) {
                error = Error{ErrorCode::TransportError, "the WebSocket connection has no socket"};
            }
            if (const long status = state.response.status_code; status != 0 && status != 101) {
                error.message += " (HTTP " + std::to_string(status) + ")";
            }
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            ws_events.push_back(close_event(callbacks, std::move(error), kWsAbnormalClosure, {}));
            return;
        }

        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        state.response.status_code = status;

        std::unique_ptr<detail::WsCodec> codec;
        if (state.ws_options.deflate) {
            std::vector<std::string> extensions;
            for (const auto& [name, value] : state.response.headers) {
                if (name_equals(name, "sec-websocket-extensions")) extensions.push_back(value);
            }
            codec = make_codec(detail::parse_ws_extensions(extensions));
            if (!codec) {
                curl_multi_remove_handle(multi, easy);
                curl_easy_cleanup(easy);
                ws_events.push_back(close_event(
                    callbacks,
                    Error{ErrorCode::TransportError, "the server answered with WebSocket extensions we cannot use"},
                    kWsAbnormalClosure, {}));
                return;
            }
        }

        const Handle handle = state.handle;
        WsState& ws = websockets[handle];
        ws.handle = handle;
        ws.easy = easy;
        ws.fd = socket;
        ws.callbacks = callbacks;
        ws.outbox = std::move(state.ws_outbox);
        ws.codec = std::move(codec);
        ws_handles_by_fd[ws.fd] = handle;

        WsEvent open{WsEvent::Kind::Open, ws.callbacks};
        open.head = std::move(state.response);
        ws_events.push_back(std::move(open));

        // libcurl may already hold frames that arrived with the 101, which
        // the socket will never report as readable again.
        service_ws(handle, true, true);
    }

    // The codec for the extensions the server accepted, or null if they are
    // unusable (`params` is nullopt) or zlib could not start.
    static std::unique_ptr<detail::WsCodec> make_codec(const std::optional<detail::WsDeflateParams>& params) {
        if (!params) return nullptr;
        try {
            return std::make_unique<detail::WsCodec>(*params);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    // Moves the WebSocket `handle` along: flushes its outbox if `writable`,
    // drains what has arrived if `readable`, then watches its socket for
    // whatever it needs next. Called with `mutex` held; may close it.
    void service_ws(const Handle handle, const bool readable, const bool writable) {
        const auto it = websockets.find(handle);
        if (it == websockets.end()) return;
        WsState& ws = it->second;
        if (writable && !flush_ws(ws)) return;
        if (readable && !receive_ws(ws)) return;
        waker.attach_socket(ws.fd, ws.outbox.empty() ? waker::Interest::Read : waker::Interest::ReadWrite);
    }

    // Hands the outbox to libcurl until it is empty or the socket is full.
    // Returns false if that closed the WebSocket.
    bool flush_ws(WsState& ws) {
        while (!ws.outbox.empty()) {
            // By name: CLion would then count none of WsFrame's fields as used.
            // ReSharper disable once CppUseStructuredBinding
            WsFrame& frame = ws.outbox.front();
            if (ws.codec && !frame.encoded) {
                frame.data = ws.codec->encode(ws_opcode(frame.flags), frame.data);
                frame.encoded = true;
            }
            std::size_t sent = 0;
            const char* data = frame.data.data() + frame.offset;
            const std::size_t size = frame.data.size() - frame.offset;
            const CURLcode rc = ws.codec ? curl_easy_send(ws.easy, data, size, &sent)
                                         : curl_ws_send(ws.easy, data, size, &sent, 0, frame.flags);
            if (rc == CURLE_AGAIN) return true; // the same call again once writable
            if (rc != CURLE_OK) {
                close_ws(ws, Error{ErrorCode::TransportError, curl_easy_strerror(rc)}, kWsAbnormalClosure, {});
                return false;
            }
            frame.offset += sent;
            if (frame.offset < frame.data.size()) continue;
            const bool was_close = (frame.flags & CURLWS_CLOSE) != 0;
            ws.outbox.pop_front();
            if (was_close) {
                close_ws(ws, Error{}, ws.close_code, ws.close_reason);
                return false;
            }
        }
        return true;
    }

    // Reads every frame libcurl has for `ws`, reporting each complete data
    // message. Returns false if that closed the WebSocket.
    bool receive_ws(WsState& ws) {
        if (ws.codec) return receive_ws_raw(ws);
        char buffer[64 * 1024];
        while (true) {
            std::size_t received = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode rc = curl_ws_recv(ws.easy, buffer, sizeof buffer, &received, &meta);
            if (rc == CURLE_AGAIN) return true;
            if (rc != CURLE_OK || !meta) {
                const std::string message =
                    rc == CURLE_GOT_NOTHING ? "the connection closed without a close frame" : curl_easy_strerror(rc);
                close_ws(ws, Error{ErrorCode::TransportError, message}, kWsAbnormalClosure, {});
                return false;
            }

            if (meta->flags & CURLWS_CLOSE) {
                ws.close_frame.append(buffer, received);
                if (meta->bytesleft > 0) continue;
                std::uint16_t code = kWsNoStatus;
                std::string reason;
                if (ws.close_frame.size() >= 2) {
                    code = static_cast<std::uint16_t>((static_cast<unsigned char>(ws.close_frame[0]) << 8) |
                                                      static_cast<unsigned char>(ws.close_frame[1]));
                    reason = ws.close_frame.substr(2);
                }
                // Echo the close, as RFC 6455 asks; best effort, the
                // connection goes either way.
                const std::string payload = ws_close_payload(code, {});
                std::size_t sent = 0;
                curl_ws_send(ws.easy, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
                close_ws(ws, Error{}, code, std::move(reason));
                return false;
            }
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue; // libcurl answers pings itself

            if (!ws.in_message) {
                ws.in_message = true;
                ws.message_type = meta->flags & CURLWS_BINARY ? WsMessageType::Binary : WsMessageType::Text;
                ws.message.clear();
            }
            ws.message.append(buffer, received);
            // The message ends with the last chunk of a frame that is not
            // followed by a continuation.
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                WsEvent event{WsEvent::Kind::Message, ws.callbacks};
                event.type = ws.message_type;
                event.data = std::move(ws.message);
                ws_events.push_back(std::move(event));
                ws.message = std::string();
                ws.in_message = false;
            }
        }
    }

    // The opcode of an outbox frame, from its CURLWS_* flags.
    static detail::WsOpcode ws_opcode(const unsigned int flags) {
        if (flags & CURLWS_CLOSE) return detail::WsOpcode::Close;
        if (flags & CURLWS_PONG) return detail::WsOpcode::Pong;
        if (flags & CURLWS_PING) return detail::WsOpcode::Ping;
        if (flags & CURLWS_BINARY) return detail::WsOpcode::Binary;
        return detail::WsOpcode::Text;
    }

    // receive_ws for a WebSocket with a codec: reads the connection's raw
    // bytes and decodes them here. Returns false if that closed it.
    bool receive_ws_raw(WsState& ws) {
        char buffer[64 * 1024];
        std::vector<detail::WsIncoming> incoming;
        while (true) {
            std::size_t received = 0;
            const CURLcode rc = curl_easy_recv(ws.easy, buffer, sizeof buffer, &received);
            if (rc == CURLE_AGAIN) return true;
            if (rc != CURLE_OK || received == 0) {
                const std::string message =
                    rc == CURLE_OK ? "the connection closed without a close frame" : curl_easy_strerror(rc);
                close_ws(ws, Error{ErrorCode::TransportError, message}, kWsAbnormalClosure, {});
                return false;
            }

            incoming.clear();
            std::string error;
            const bool valid = ws.codec->decode(buffer, received, incoming, error);
            for (auto& [opcode, payload] : incoming) {
                switch (opcode) {
                    case detail::WsOpcode::Text:
                    case detail::WsOpcode::Binary: {
                        WsEvent event{WsEvent::Kind::Message, ws.callbacks};
                        event.type = opcode == detail::WsOpcode::Binary ? WsMessageType::Binary : WsMessageType::Text;
                        event.data = std::move(payload);
                        ws_events.push_back(std::move(event));
                        break;
                    }
                    case detail::WsOpcode::Ping:
                        // Answered after whatever is already queued; never
                        // after our own close.
                        if (!ws.closing) ws.outbox.push_back(WsFrame{CURLWS_PONG, std::move(payload)});
                        break;
                    case detail::WsOpcode::Close: {
                        std::uint16_t code = kWsNoStatus;
                        std::string reason;
                        if (payload.size() >= 2) {
                            code = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
                                                              static_cast<unsigned char>(payload[1]));
                            reason = payload.substr(2);
                        }
                        // Echo it, best effort, as in receive_ws -- after what
                        // was queued before it, e.g. the pong to a ping that
                        // came just ahead, unless the socket cannot take that.
                        if (!flush_ws(ws)) return false;
                        if (ws.outbox.empty()) {
                            const std::string reply =
                                ws.codec->encode(detail::WsOpcode::Close, ws_close_payload(code, {}));
                            std::size_t sent = 0;
                            curl_easy_send(ws.easy, reply.data(), reply.size(), &sent);
                        }
                        close_ws(ws, Error{}, code, std::move(reason));
                        return false;
                    }
                    default: break; // a pong
                }
            }
            if (!valid) {
                // RFC 6455's "protocol error", best effort.
                const std::string reply = ws.codec->encode(detail::WsOpcode::Close, ws_close_payload(1002, {}));
                std::size_t sent = 0;
                curl_easy_send(ws.easy, reply.data(), reply.size(), &sent);
                close_ws(ws, Error{ErrorCode::TransportError, "the server sent " + error}, kWsAbnormalClosure, {});
                return false;
            }
        }
    }

    // Ends the WebSocket `ws` -- closing its connection and forgetting it --
    // and queues its on_close. Called with `mutex` held; `ws` is gone after.
    void close_ws(const WsState& ws, Error error, const std::uint16_t code, std::string reason) {
        waker.detach_socket(ws.fd);
        ws_handles_by_fd.erase(ws.fd);
        // For a CONNECT_ONLY transfer, this is what closes the connection.
        curl_multi_remove_handle(multi, ws.easy);
        curl_easy_cleanup(ws.easy);
        ws_events.push_back(close_event(ws.callbacks, std::move(error), code, std::move(reason)));
        websockets.erase(ws.handle); // last: `ws` is gone after
    }

    // Starts the transfer `initial` describes (send() and open_websocket()).
    Handle submit(RequestState initial);

    // libcurl's callbacks below take the parameters its prototypes for them
    // say, even those they could take as const.

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    static std::size_t write_trampoline(char* ptr, const std::size_t size, const std::size_t nmemb, void* userdata) {
        auto* state = static_cast<RequestState*>(userdata);
        const std::size_t total = size * nmemb;
        if (state->body_cb) {
            const Action action = state->body_cb(state->context, ptr, total);
            return action == Action::Continue ? total : 0;
        }
        state->response.body.append(ptr, total);
        return total;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    static std::size_t header_trampoline(char* buffer, const std::size_t size, const std::size_t nitems,
                                         void* userdata) {
        auto* state = static_cast<RequestState*>(userdata);
        const std::size_t total = size * nitems;

        std::string_view line(buffer, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);

        if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
            // A new status line: this is a fresh hop (initial response, or
            // one CURLOPT_FOLLOWLOCATION redirect further) -- keep only
            // the final hop's headers.
            state->response.headers.clear();
            state->response.status_code = 0;
            if (const auto space = line.find(' '); space != std::string_view::npos) {
                // "HTTP/1.1 200 OK" / "HTTP/2 200": the code follows the first space.
                const auto digits = line.substr(space + 1);
                long status = 0;
                std::from_chars(digits.data(), digits.data() + digits.size(), status);
                state->response.status_code = status;
            }
            return total;
        }
        if (line.empty()) {
            // Blank line ending a header block. For the final hop this is the
            // moment the caller can see the status and headers, before any
            // body data -- so the headers are settled first.
            if (is_final_hop(state->response, state->request.follow_redirects)) {
                drop_headers_describing_the_encoded_body(state->response.headers);
                if (state->head_cb && state->head_cb(state->context, state->response) == Action::Abort) {
                    return 0; // CURLE_WRITE_ERROR, which map_curl_code reports as Aborted
                }
            }
            return total;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return total;

        std::string name(line.substr(0, colon));
        std::size_t value_start = colon + 1;
        while (value_start < line.size() && line[value_start] == ' ')
            ++value_start;
        std::string value(line.substr(value_start));
        state->response.headers.push_back(Header{std::move(name), std::move(value)});
        return total;
    }

    // Always 0: anything else would make libcurl fail the transfer.
    // ReSharper disable CppParameterMayBeConstPtrOrRef
    // ReSharper disable once CppDFAConstantFunctionResult
    static int socket_callback(CURL* easy, const curl_socket_t fd, const int what, void* userp, void* socketp) {
        // ReSharper restore CppParameterMayBeConstPtrOrRef
        (void)easy;
        (void)socketp;
        auto* self = static_cast<Impl*>(userp);
        // An open WebSocket's socket is watched by service_ws, not libcurl.
        if (self->ws_handles_by_fd.count(fd)) return 0;
        switch (what) {
            case CURL_POLL_IN: self->waker.attach_socket(fd, waker::Interest::Read); break;
            case CURL_POLL_OUT: self->waker.attach_socket(fd, waker::Interest::Write); break;
            case CURL_POLL_INOUT: self->waker.attach_socket(fd, waker::Interest::ReadWrite); break;
            case CURL_POLL_REMOVE: self->waker.detach_socket(fd); break;
            default: break;
        }
        return 0;
    }

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    static int timer_callback(CURLM* multi_handle, const long timeout_ms, void* userp) {
        (void)multi_handle;
        auto* self = static_cast<Impl*>(userp);
        if (timeout_ms < 0) {
            self->curl_deadline.reset();
        } else {
            self->curl_deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        }
        self->arm_waker();
        return 0;
    }

    // Arms the waker's timer for the earliest of libcurl's deadline and the
    // timers', or disarms it. `mutex` must be held: libcurl calls
    // timer_callback only from calls made under it.
    void arm_waker() {
        std::optional<Clock::time_point> earliest = curl_deadline;
        if (!timer_queue.empty() && (!earliest || timer_queue.begin()->first < *earliest)) {
            earliest = timer_queue.begin()->first;
        }
        if (!earliest) {
            waker.cancel_timer();
            return;
        }
        // Rounded up: waking before the deadline would only arm it again.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*earliest - Clock::now());
        waker.wake_after(std::max(left, std::chrono::milliseconds(0)));
    }

    // Moves the timers that are due to `fired_timers`. `mutex` must be held.
    void collect_due_timers() {
        const auto now = Clock::now();
        while (!timer_queue.empty() && timer_queue.begin()->first <= now) {
            const Handle handle = timer_queue.begin()->second;
            timer_queue.erase(timer_queue.begin());
            const auto it = timers.find(handle);
            fired_timers.push_back(FiredTimer{it->second.context, it->second.callback, false});
            timers.erase(it);
        }
    }

    static void dispatch_timers(const std::vector<FiredTimer>& fired) {
        // By name: CLion would then count none of FiredTimer's fields as used.
        // ReSharper disable once CppUseStructuredBinding
        for (const auto& timer : fired) {
            if (timer.callback) timer.callback(timer.context, timer.cancelled);
        }
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

Client::Handle Client::send(Request request, void* context, const BodyCallback body_cb,
                            const CompletionCallback done_cb, const HeadCallback head_cb) {
    Impl::RequestState state;
    state.request = std::move(request);
    state.context = context;
    state.head_cb = head_cb;
    state.body_cb = body_cb;
    state.done_cb = done_cb;
    return impl_->submit(std::move(state));
}

Client::Handle Client::open_websocket(Request request, void* context, const WsOpenCallback on_open,
                                      const WsMessageCallback on_message, const WsCloseCallback on_close,
                                      const WsOptions options) {
    Impl::RequestState state;
    request.method = Method::Get;
    request.body.clear();
    request.follow_redirects = false;
    // The extensions are ours to offer: libcurl's frames take none, and the
    // codec only permessage-deflate.
    request.headers.erase(
        std::remove_if(request.headers.begin(), request.headers.end(),
                       [](const Header& header) { return name_equals(header.name, "sec-websocket-extensions"); }),
        request.headers.end());
    if (options.deflate) request.headers.push_back(Header{"Sec-WebSocket-Extensions", detail::kWsDeflateOffer});
    state.request = std::move(request);
    state.ws = Impl::WsCallbacks{context, on_open, on_message, on_close};
    state.ws_options = options;
    return impl_->submit(std::move(state));
}

bool Client::send_websocket(const Handle handle, const WsMessageType type, const char* data, const std::size_t size) {
    Impl::WsFrame frame{type == WsMessageType::Binary ? static_cast<unsigned int>(CURLWS_BINARY)
                                                      : static_cast<unsigned int>(CURLWS_TEXT),
                        std::string(data, size)};
    std::scoped_lock lock(impl_->mutex);
    if (const auto ws = impl_->websockets.find(handle); ws != impl_->websockets.end()) {
        if (ws->second.closing) return false;
        ws->second.outbox.push_back(std::move(frame));
        // Flushed by run() as soon as the socket is writable.
        impl_->waker.attach_socket(ws->second.fd, waker::Interest::ReadWrite);
        return true;
    }
    const auto easy = impl_->handles.find(handle);
    if (easy == impl_->handles.end()) return false;
    Impl::RequestState& state = impl_->requests.at(easy->second);
    if (!state.ws) return false;
    state.ws_outbox.push_back(std::move(frame)); // sent once the handshake is done
    return true;
}

void Client::close_websocket(const Handle handle, const std::uint16_t code, const std::string& reason) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (const auto it = impl_->websockets.find(handle); it != impl_->websockets.end()) {
            Impl::WsState& ws = it->second;
            if (ws.closing) return;
            ws.closing = true;
            ws.close_code = code;
            ws.close_reason = reason.substr(0, kWsMaxCloseReason);
            ws.outbox.push_back(Impl::WsFrame{CURLWS_CLOSE, ws_close_payload(code, reason)});
            impl_->waker.attach_socket(ws.fd, waker::Interest::ReadWrite);
            return;
        }
    }
    // Still in the handshake (or unknown): abort it, as cancel() would.
    cancel(handle);
}

Client::Handle Client::Impl::submit(RequestState initial) {
    CURL* easy = curl_easy_init();
    if (!easy) throw std::runtime_error("curl_easy_init failed");

    std::scoped_lock lock(mutex);

    const Handle handle = next_handle++;
    const auto [it, inserted] = requests.emplace(easy, std::move(initial));
    (void)inserted; // curl_easy_init() always returns a fresh, distinct CURL*

    RequestState& state = it->second;
    state.handle = handle;
    const bool websocket = state.ws.has_value();

    curl_easy_setopt(easy, CURLOPT_URL, state.request.url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, state.request.follow_redirects ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    if (websocket) {
        // The transfer ends once the upgrade is done, leaving the connection
        // to curl_ws_send/curl_ws_recv. The upgrade itself is HTTP/1.1 only.
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    } else {
        // HTTP/3 first; falls back to HTTP/2 or 1.1 for servers without it.
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3);
        // Empty string = "accept and auto-decompress whatever this libcurl
        // build supports" (gzip/deflate/br); Content-Encoding is stripped and
        // Response::body arrives already decoded.
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    }
    if (state.request.timeout.count() > 0) {
        const auto timeout_ms = static_cast<long>(state.request.timeout.count());
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
        // CURLOPT_TIMEOUT_MS bounds the whole transfer, but on some
        // curl/QUIC-backend builds a CURL_HTTP_VERSION_3 attempt whose UDP
        // packets are silently dropped (no response, no ICMP, no TLS alert --
        // common for networks that specifically block/throttle QUIC while
        // plain TCP to the same host works fine) never reaches the point
        // where that accounting kicks in, and the connect phase hangs
        // indefinitely instead of falling back to HTTP/2 or 1.1. Bounding the
        // connect phase on its own, the same length, is a safe pairing
        // regardless of protocol -- a well-behaved connection establishes in
        // milliseconds, far under any real `timeout`.
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    }
    if (state.request.pinned_public_key) {
        curl_easy_setopt(easy, CURLOPT_PINNEDPUBLICKEY, state.request.pinned_public_key->c_str());
    }

    switch (state.request.method) {
        case Method::Get: break;
        case Method::Head: curl_easy_setopt(easy, CURLOPT_NOBODY, 1L); break;
        case Method::Options: curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method_verb(state.request.method)); break;
        case Method::Post:
        case Method::Put:
        case Method::Patch:
        case Method::Delete:
            if (state.request.method != Method::Post) {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method_verb(state.request.method));
            } else {
                curl_easy_setopt(easy, CURLOPT_POST, 1L);
            }
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, state.request.body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(state.request.body.size()));
            break;
    }

    for (const auto& [name, value] : state.request.headers) {
        std::string line = name;
        line.append(": ").append(value);
        state.header_list = curl_slist_append(state.header_list, line.c_str());
    }
    // The headers reqwest sends by default, and no others. A caller's own
    // header of the same name wins, so each default is added only if absent.
    // libcurl sends its own User-Agent, Accept and Expect unless told
    // otherwise; "Name:" (no value) switches one of those off.
    if (!websocket && !has_header(state.request.headers, "accept")) {
        state.header_list = curl_slist_append(state.header_list, "Accept: */*");
    }
    if (!websocket && !has_header(state.request.headers, "accept-encoding")) {
        const std::string accept_encoding = accept_encoding_value();
        const std::string line = accept_encoding.empty() ? "Accept-Encoding:" : "Accept-Encoding: " + accept_encoding;
        state.header_list = curl_slist_append(state.header_list, line.c_str());
    }
    if (!has_header(state.request.headers, "user-agent")) {
        state.header_list = curl_slist_append(state.header_list, "User-Agent:");
    }
    if (!has_header(state.request.headers, "expect")) {
        state.header_list = curl_slist_append(state.header_list, "Expect:");
    }
    if (state.header_list) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, state.header_list);

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Impl::write_trampoline);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Impl::header_trampoline);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &state);

    handles.emplace(handle, easy);

    if (const CURLMcode mc = curl_multi_add_handle(multi, easy); mc != CURLM_OK) {
        if (state.header_list) curl_slist_free_all(state.header_list);
        requests.erase(it);
        handles.erase(handle);
        curl_easy_cleanup(easy);
        throw std::runtime_error(std::string("curl_multi_add_handle: ") + curl_multi_strerror(mc));
    }

    return handle;
}

void Client::cancel(const Handle handle) {
    Impl::Finished finished{};
    bool found = false;
    std::vector<Impl::WsEvent> ws_closed;

    {
        std::scoped_lock lock(impl_->mutex);
        if (const auto timer = impl_->timers.find(handle); timer != impl_->timers.end()) {
            impl_->timer_queue.erase({timer->second.deadline, handle});
            impl_->fired_timers.push_back(Impl::FiredTimer{timer->second.context, timer->second.callback, true});
            impl_->timers.erase(timer);
            impl_->waker.wake(); // run() calls it
            return;
        }
        const auto handle_it = impl_->handles.find(handle);
        if (handle_it == impl_->handles.end()) return;

        CURL* easy = handle_it->second;
        if (const auto req_it = impl_->requests.find(easy); req_it != impl_->requests.end()) {
            Impl::RequestState state = std::move(req_it->second);
            impl_->requests.erase(req_it);
            found = true;

            if (state.header_list) curl_slist_free_all(state.header_list);

            finished.context = state.context;
            finished.done_cb = state.done_cb;
            finished.response = std::move(state.response);
            finished.error = Error{ErrorCode::Aborted, "cancelled"};
            if (state.ws) {
                ws_closed.push_back(Impl::close_event(*state.ws, finished.error, kWsAbnormalClosure, {}));
            }
        }
        impl_->handles.erase(handle_it);
        curl_multi_remove_handle(impl_->multi, easy);
        curl_easy_cleanup(easy);
    }

    if (found && finished.done_cb) finished.done_cb(finished.context, std::move(finished.response), finished.error);
    Impl::dispatch_ws_events(std::move(ws_closed));
}

void Client::run() {
    // The first pass drives whatever was sent before run() started, without
    // waiting: libcurl may finish such a transfer right there (e.g. a URL it
    // cannot parse) and cancel its timer, leaving nothing to wake on.
    auto reason = waker::WakeReason::Timeout;
    while (true) {
        std::vector<Impl::Finished> finished;
        std::vector<Impl::WsEvent> ws_events;
        std::vector<Impl::FiredTimer> fired_timers;
        {
            std::scoped_lock lock(impl_->mutex);
            if (!impl_->running) break;

            int running_handles = 0;
            switch (reason) {
                case waker::WakeReason::Manual: break; // stop(), or a poke from send()/cancel(); state re-checked below
                case waker::WakeReason::Timeout:
                    // The waker's timer is libcurl's or a set_timer() one;
                    // libcurl's is cleared before driving it, so that it
                    // re-arms through timer_callback if it needs to.
                    if (!impl_->curl_deadline || *impl_->curl_deadline <= Impl::Clock::now()) {
                        impl_->curl_deadline.reset();
                        curl_multi_socket_action(impl_->multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
                    }
                    break;
                case waker::WakeReason::Socket:
                    for (const auto& [fd, ready] : impl_->waker.take_ready_sockets()) {
                        if (const auto ws = impl_->ws_handles_by_fd.find(fd); ws != impl_->ws_handles_by_fd.end()) {
                            impl_->service_ws(ws->second, waker::has_read(ready), waker::has_write(ready));
                            continue;
                        }
                        int mask = 0;
                        if (waker::has_read(ready)) mask |= CURL_CSELECT_IN;
                        if (waker::has_write(ready)) mask |= CURL_CSELECT_OUT;
                        curl_multi_socket_action(impl_->multi, fd, mask, &running_handles);
                    }
                    break;
            }

            finished = impl_->collect_finished();
            ws_events = impl_->take_ws_events();
            impl_->collect_due_timers();
            fired_timers = std::move(impl_->fired_timers);
            impl_->fired_timers.clear();
            impl_->arm_waker();
        }

        for (auto& [context, done_cb, response, error] : finished) {
            if (done_cb) done_cb(context, std::move(response), error);
        }
        Impl::dispatch_ws_events(std::move(ws_events));
        Impl::dispatch_timers(fired_timers);

        if (!impl_->running) break;
        reason = impl_->waker.wait();
    }
}

Client::Handle Client::set_timer(const std::chrono::milliseconds delay, void* context, const TimerCallback callback) {
    std::scoped_lock lock(impl_->mutex);
    const Handle handle = impl_->next_handle++;
    const auto deadline = Impl::Clock::now() + std::max(delay, std::chrono::milliseconds(0));
    impl_->timers.emplace(handle, Impl::Timer{deadline, context, callback});
    impl_->timer_queue.emplace(deadline, handle);
    impl_->arm_waker();
    return handle;
}

void Client::stop() {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->running = false;
    }
    impl_->waker.wake();
}

} // namespace c_share::http
