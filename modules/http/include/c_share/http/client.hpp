#pragma once

// What this header uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::http {

enum class Method {
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Head,
    Options,
};

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string url;
    Method method = Method::Get;
    std::vector<Header> headers;
    std::string body;                                                     // ignored for Get/Head/Options unless set
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000); // 0 = no timeout

    // Whether libcurl follows 301/302/303/307/308 responses that carry a
    // Location (CURLOPT_FOLLOWLOCATION, at most 10 hops). Off, such a
    // response is final and comes back as-is, so the caller can do the
    // redirect itself -- e.g. to resend cookies per hop. Note that with it
    // on, libcurl re-sends a custom Cookie header on each hop, even to
    // another host.
    bool follow_redirects = true;

    // Optional certificate/public-key pinning for this request. Left
    // unset (the default), pinning is disabled and normal CA validation
    // applies. When set, forwarded verbatim to libcurl's
    // CURLOPT_PINNEDPUBLICKEY: either a path to a file holding the
    // public key, or one or more "sha256//<base64-hash>" entries
    // separated by ';' (the hash of the DER-encoded SubjectPublicKeyInfo,
    // e.g. as printed by `openssl x509 -pubkey ... | openssl pkey
    // -pubin -outform der | openssl dgst -sha256 -binary | base64`). A
    // mismatch fails the request with ErrorCode::TlsError.
    std::optional<std::string> pinned_public_key;
};

// A completed HTTP response. Only populated when Error is empty (see
// CompletionCallback) -- an HTTP-level error status (404, 500, ...) is
// still a successful transport-level exchange and comes back here with no
// Error, exactly like any other status code; only transport failures
// (DNS, connect, TLS, timeout, cancellation) populate Error instead.
struct Response {
    long status_code = 0;
    std::vector<Header> headers;
    std::string body; // left empty if a BodyCallback was supplied to send()
};

enum class ErrorCode {
    None,
    InvalidUrl,
    CouldNotResolveHost,
    CouldNotConnect,
    TlsError,
    Timeout,
    Aborted,        // BodyCallback returned Action::Abort, or cancel() was called
    TransportError, // anything else -- see Error::message for detail
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message; // human-readable detail, for logs; not meant to be parsed

    explicit operator bool() const { return code != ErrorCode::None; }
};

enum class Action {
    Continue, // keep the transfer going
    Abort,    // abort it now
};

// Optional: called once per final response, as soon as its header block
// ends -- before any body data, and long before CompletionCallback. `head`
// carries the status and headers of that response (its body is always
// empty). Only the final hop after redirects is reported; 1xx interim
// responses and 3xx redirects that libcurl will follow are skipped. May
// return Action::Abort to cancel the transfer right there, reported to
// CompletionCallback as ErrorCode::Aborted. If null, no headers are
// reported early (they still arrive in Response::headers on completion).
using HeadCallback = Action (*)(void* context, const Response& head);

// Optional: called with each chunk of response body data as it arrives.
// If supplied, Response::body is left empty (the caller owns
// accumulating/consuming the data itself) and may return Action::Abort to
// cancel the transfer, reported to CompletionCallback as
// ErrorCode::Aborted. If null, Client buffers the whole body into
// Response::body for you.
//
// Do not call Client::cancel() or Client::send() from inside this
// callback, nor from a HeadCallback: it runs on Client::run()'s thread
// while that thread holds Client's internal lock, so either call would
// deadlock on the same (non-recursive) lock. Return Action::Abort instead;
// cancel() is for another thread, or a different handle from inside
// CompletionCallback.
using BodyCallback = Action (*)(void* context, const char* data, std::size_t size);

// Called exactly once per handle returned by send(): with a populated
// Response and an empty Error on completion (any HTTP status), or a
// default Response and a populated Error on transport failure/abort/
// cancel. Safe to call Client::send() or Client::cancel() (for a
// different handle) from inside this callback -- it always runs outside
// Client's internal lock.
using CompletionCallback = void (*)(void* context, Response response, Error error);

// Called exactly once per set_timer(): when the delay has passed, or with
// `cancelled` true once cancel() or the Client's destruction ended it
// first. Runs on run()'s thread, outside Client's internal lock, like
// CompletionCallback.
using TimerCallback = void (*)(void* context, bool cancelled);

// The kind of a WebSocket data message.
enum class WsMessageType {
    Text,
    Binary,
};

// The close code a WebSocket reports when the connection ended without a
// close frame -- RFC 6455's "abnormal closure".
constexpr std::uint16_t kWsAbnormalClosure = 1006;

// How a WebSocket is opened (see Client::open_websocket).
struct WsOptions {
    // Offer permessage-deflate (RFC 7692), as browsers do: if the server
    // accepts, every data message is compressed both ways. The frames are
    // then handled by this module rather than libcurl's WebSocket support,
    // which takes no compressed frame. If the server declines, messages just
    // go uncompressed. Off, the WebSocket is plain libcurl.
    bool deflate = false;
};

// The WebSocket callbacks (see Client::open_websocket). Like the HTTP ones,
// they all run on Client::run()'s thread, but outside Client's internal lock:
// each may call send_websocket()/close_websocket()/send()/cancel().
//
// `on_open`: once, when the server accepted the upgrade (status 101), with
// that response's status and headers (e.g. its Set-Cookie). Never called if
// the handshake fails. May be null.
using WsOpenCallback = void (*)(void* context, const Response& head);
// `on_message`: once per complete data message (a fragmented message is
// reassembled first). `data` is only valid for the duration of the call.
// Pings are answered and pongs ignored, without being reported. May be null.
using WsMessageCallback = void (*)(void* context, WsMessageType type, const char* data, std::size_t size);
// `on_close`: exactly once per handle open_websocket() returned, last. `error`
// is empty for a close handshake from either side, with that close frame's
// code (1005 if it carried none) and reason; otherwise it says what failed --
// a refused upgrade, a dropped connection, close_websocket() before the
// handshake finished (ErrorCode::Aborted) -- and `code` is
// kWsAbnormalClosure.
using WsCloseCallback = void (*)(void* context, Error error, std::uint16_t code, const std::string& reason);

// A non-blocking HTTP client. Internally a thin wrapper around libcurl's
// multi-socket API, driven by a c_share::waker::Waker instead of
// curl_multi_poll()/select() -- but libcurl is strictly an implementation
// detail: no libcurl type appears anywhere in this header, so consumers of
// this module never need libcurl headers themselves.
//
// The calling application still owns libcurl's global state: call
// curl_global_init(CURL_GLOBAL_DEFAULT) once (from <curl/curl.h>, only in
// whichever translation unit does process startup) before constructing
// any Client, and curl_global_cleanup() once after destroying all of them
// -- libcurl documents curl_global_init as not thread-safe and not safe to
// pair per-instance across independent components, so this class
// deliberately does not call it for you. On Windows, CURL_GLOBAL_DEFAULT
// already initializes Winsock (WSAStartup), so no separate call is needed
// for that.
class Client {
public:
    using Handle = std::uint64_t;

    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Sends `request`. `context` is passed back to `head_cb`/`body_cb`
    // (each optional, may be null) and `done_cb` verbatim; this class never
    // reads or owns it. Safe to call from any thread.
    Handle send(Request request, void* context, BodyCallback body_cb, CompletionCallback done_cb,
                HeadCallback head_cb = nullptr);

    // Cancels a running request: `done_cb` still runs, with
    // Error::code == ErrorCode::Aborted. Also cancels a timer (see
    // set_timer). No-op if `handle` is unknown or already finished. Safe to
    // call from any thread EXCEPT from inside that same request's
    // BodyCallback -- see BodyCallback's docs.
    void cancel(Handle handle);

    // Calls `callback` on run()'s thread once `delay` has passed: a timer on
    // the same loop as the transfers, for work that waits between requests
    // (e.g. polling). cancel() with the returned handle ends it early.
    // `context` is passed back verbatim. Safe to call from any thread.
    Handle set_timer(std::chrono::milliseconds delay, void* context, TimerCallback callback);

    // Opens a WebSocket to `request.url` (`ws://` or `wss://`). The upgrade
    // is a GET over HTTP/1.1 carrying `request.headers` (e.g. Origin, Cookie,
    // Sec-WebSocket-Protocol -- libcurl adds the Upgrade/Sec-WebSocket-Key
    // ones itself); `request.timeout` bounds the handshake only, and
    // redirects are never followed. A Sec-WebSocket-Extensions header in it is
    // dropped: `options.deflate` decides that one. `context` is passed back
    // to the callbacks verbatim, the last call being `on_close` (required).
    // Safe to call from any thread.
    Handle open_websocket(Request request, void* context, WsOpenCallback on_open, WsMessageCallback on_message,
                          WsCloseCallback on_close, WsOptions options = {});

    // Queues a message on the WebSocket `handle`, sent in order once the
    // socket can take it. Messages queued before the handshake finishes wait
    // for it. Returns false, sending nothing, if `handle` is unknown or
    // already closing. Safe to call from any thread.
    bool send_websocket(Handle handle, WsMessageType type, const char* data, std::size_t size);

    // Closes the WebSocket `handle`: once the messages queued before it are
    // out, sends a close frame with `code` (and `reason`, at most 123 bytes)
    // and reports `on_close` with them, without waiting for the server's own
    // close frame. Before the handshake finishes, aborts it instead
    // (`on_close` with ErrorCode::Aborted). No-op if `handle` is unknown or
    // already closing. Safe to call from any thread.
    void close_websocket(Handle handle, std::uint16_t code, const std::string& reason);

    // Runs the internal event loop until stop() is called (from another
    // thread, or from a callback). Meant to be run on a dedicated thread;
    // only one thread may call run() at a time. Make sure run() has
    // returned before destroying this Client.
    void run();

    // Makes a running run() call return. Safe to call from any thread.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace c_share::http
