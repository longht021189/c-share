# c_share::http

`client.hpp`'s `Client` is a non-blocking HTTP client. For Rust (or any
other C FFI) there is also `client_c.h`: a plain C shim over the same
`Client`, in the same spirit as [`crypto`](../crypto/README.md)'s
`crypto.h` -- `client.hpp`'s `std::string`/`std::vector`/classes have no
stable C ABI for bindgen to consume, so `client_c.cpp` translates between
them and flat structs (pointer + length, no NUL-termination assumed) and
an opaque `c_share_http_client_t*`. It also takes care of
`curl_global_init`/`curl_global_cleanup` (reference-counted across however
many clients are alive), so a C/Rust caller never needs `<curl/curl.h>`
either. The C API owns the event loop's thread: `c_share_http_client_create`
starts it, and `c_share_http_client_destroy` stops it, joins it, and frees the
client (it must not be called from inside a callback of that client). See
`r-share/src/http_client` for the Rust side: an async wrapper that bridges
each completion callback to a `tokio::sync::oneshot`.

`Client` is a non-blocking HTTP client. Internally it wraps libcurl's
multi-socket API (`CURLMOPT_SOCKETFUNCTION` / `CURLMOPT_TIMERFUNCTION` /
`curl_multi_socket_action`), driven by
[`c_share::waker::Waker`](../waker/README.md) instead of
`curl_multi_poll()`/`select()` -- but libcurl is strictly an
implementation detail of `client.cpp`. **No libcurl type appears anywhere
in `client.hpp`**, so consumers of this module never need
`<curl/curl.h>` themselves; they work entirely with `Request`/`Response`/
`Error`.

## Usage

```cpp
#include "c_share/http/client.hpp"
#include <curl/curl.h> // only for curl_global_init/cleanup -- see below

curl_global_init(CURL_GLOBAL_DEFAULT); // once, before any Client

c_share::http::Client client;
std::thread runner([&client] { client.run(); });

c_share::http::Request req;
req.url = "https://example.com/api/items";
req.method = c_share::http::Method::Post;
req.headers.push_back({"Content-Type", "application/json"});
req.body = R"({"name":"widget"})";
req.timeout = std::chrono::seconds(10);

auto done_cb = [](void* context, c_share::http::Response response, c_share::http::Error error) {
    if (error) {
        std::cerr << "transport error: " << error.message << "\n";
        return;
    }
    std::cout << "status " << response.status_code << ", " << response.body.size() << " bytes\n";
};

auto handle = client.send(std::move(req), nullptr, nullptr, done_cb);

// client.cancel(handle); // to cancel from another thread

// later, at shutdown:
client.stop();
runner.join();
curl_global_cleanup();
```

`BodyCallback`/`CompletionCallback` are plain function pointers (`void*`
context, no captures) so this API stays usable from a C-style FFI
boundary; a capture-less lambda converts to one implicitly, as above.

## Request / Response / Error

- `Request::method` covers `Get`/`Post`/`Put`/`Patch`/`Delete`/`Head`/
  `Options`. `Request::body` is sent for `Post`/`Put`/`Patch`, and for
  `Delete` only if non-empty; it's ignored for `Get`/`Head`/`Options`.
- Redirects: `Request::follow_redirects` (default on) lets libcurl follow
  301/302/303/307/308 responses with a `Location`, capped at 10 hops, and
  `Response::headers` then reflects only the final hop. Off, such a
  response is final and comes back as-is -- the caller does the redirect
  itself. The C API takes it as `follow_redirects` (nonzero to follow). Note
  that with it on, libcurl re-sends a custom `Cookie` header on every hop,
  even to another host; `r-share` turns it off and follows redirects itself
  so each hop gets its own cookies and headers.
- Default headers match reqwest's: `Accept: */*` and an `Accept-Encoding`
  listing what this libcurl can decode, in reqwest's order (`gzip,deflate,br`).
  libcurl's own `User-Agent`, `Expect` and `Accept` are switched off unless
  the caller sets them. Each default is skipped when the caller sets a header
  of that name.
- Response compression is negotiated and decoded transparently
  (`CURLOPT_ACCEPT_ENCODING` set to `""`, letting libcurl offer and
  auto-decode whatever it was built with); `Response::body` always arrives
  already decompressed. When libcurl decoded it (`gzip`, `deflate` or `br`,
  exact lowercase), the `Content-Encoding` and `Content-Length` that described
  the compressed bytes are dropped from the headers, as reqwest does.
  Advertised codings are limited to what the build supports: the vcpkg
  `curl` feature set in vcpkg-share's `vcpkg.json` does not include `brotli` yet, so
  `Accept-Encoding` is `gzip,deflate` there until `curl[brotli]` is added.
- `Request::pinned_public_key` is optional certificate/public-key pinning
  (`CURLOPT_PINNEDPUBLICKEY`) -- unset by default (normal CA validation),
  set it to a file path or `"sha256//<base64-hash>[;sha256//<hash2>]"` to
  require the server's key match. A mismatch fails with
  `ErrorCode::TlsError`.
- `Response` is only populated on success (no `Error`) -- **and an HTTP
  4xx/5xx status is still success at this layer**: the transport
  completed the exchange, so check `Response::status_code` yourself.
  `Error` is reserved for transport-level failures: DNS, connect, TLS,
  timeout, or cancellation -- see `ErrorCode`.
- By default `Client` buffers the whole response body into
  `Response::body`. Pass a `BodyCallback` to `send()` instead to stream
  chunks as they arrive (e.g. for large downloads); `Response::body` is
  then left empty, since the caller owns consuming the data.
- Pass a `HeadCallback` to `send()` to get the status and headers **as
  soon as the header block ends**, before any body data and long before
  `CompletionCallback`. It receives a `Response` whose `body` is empty.
  Only the final hop is reported: 1xx interim responses and 3xx redirects
  that libcurl will follow are skipped. Return `Action::Abort` from it to
  cancel the transfer right there (reported as `ErrorCode::Aborted`), e.g.
  to skip a body you do not want. The C API takes the same thing as
  `c_share_http_head_cb`.

  Known gap: behind an HTTP proxy, libcurl also hands the `CONNECT`
  response's header block to the header callback, which can look like a
  final response to `HeadCallback`. Not handled yet; there is no test
  setup for a proxy in this module.

## WebSockets

`open_websocket()` opens a `ws://`/`wss://` connection: the upgrade is a
plain GET over HTTP/1.1 with the request's headers (Origin, Cookie,
Sec-WebSocket-Protocol, ...), done by libcurl with `CURLOPT_CONNECT_ONLY` set
to `2`. Once it succeeds, the transfer stays in the multi handle (removing it
would close a connect-only connection) but its socket is watched by the
client itself, which talks to it through `curl_ws_send`/`curl_ws_recv`.

```cpp
const auto ws = client.open_websocket(std::move(request), context, on_open, on_message, on_close);
client.send_websocket(ws, http::WsMessageType::Text, "hi", 2); // queued; fine before on_open too
client.close_websocket(ws, 1000, "bye");                        // on_close follows
```

- `on_open` gets the 101 response's status and headers (e.g. `Set-Cookie`).
- `on_message` gets each complete data message, fragments reassembled; pings
  are answered by libcurl and never reported.
- `on_close` runs exactly once, last: with an empty `Error` and the close
  frame's code/reason after a close handshake from either side, or with an
  `Error` and code 1006 when the upgrade is refused (the HTTP status is in
  the message), the connection drops, or `close_websocket()` aborts the
  handshake.

Like the HTTP callbacks, these run on the event-loop thread, outside the
client's lock. Needs libcurl built with WebSockets: the `websockets` feature
of the `curl` port, which vcpkg-share's `vcpkg.json` asks for (the overlay port keeps every
non-HTTP protocol out of it).

### Compression

`WsOptions::deflate` offers permessage-deflate (RFC 7692), as browsers do.
libcurl cannot take compressed frames -- its decoder rejects the RSV1 bit
they carry, even with `CURLWS_RAW_MODE` -- so such a WebSocket uses libcurl
for the handshake and TLS only: its frames are encoded and decoded by
`src/ws_codec.cpp` (zlib, raw deflate) and moved with
`curl_easy_send`/`curl_easy_recv`, pings answered there too. If the server
declines the offer, the same codec just sends and receives uncompressed
frames; if it answers with anything we did not offer, the connection fails.
Without `deflate`, the WebSocket is libcurl's own (`curl_ws_send`/`curl_ws_recv`).

## Cancelling

There are two ways a request ends early:

1. **From inside `BodyCallback`**, return `Action::Abort`. This is the
   *only* safe way to cancel a request from within its own data callback
   -- see the deadlock note below.
2. **From anywhere else** (another thread, a timer, `CompletionCallback`
   for a *different* handle), call `client.cancel(handle)`.

Either way, `CompletionCallback` still runs, with
`Error::code == ErrorCode::Aborted`.

**Never call `cancel()` on a handle from inside that handle's own
`BodyCallback` or `HeadCallback`.** Both run synchronously inside
`Client::run()`'s thread while `run()` holds `Client`'s internal lock;
`cancel()` called from the same thread would block forever trying to
re-acquire that same (non-recursive) lock. Use `Action::Abort` for that
case -- it's exactly what it's for.

## Timers

`set_timer(delay, context, callback)` calls `callback(context, cancelled)`
on `run()`'s thread once `delay` has passed: a timer on the same loop as the
transfers, for work that waits between requests (the evm module polls for
receipts with it). `cancel(handle)` ends it early, and the callback still
runs, once, with `cancelled` true; so does destroying the `Client`. The C
API has `c_share_http_client_set_timer`.

## Threading model

- `run()` is meant to be called once, from a single dedicated thread; it
  blocks until `stop()` is called.
- `send()`, `set_timer()`, `cancel()`, and `stop()` are safe to call from any thread,
  and from inside `CompletionCallback` (it runs outside the lock). They
  are **not** safe from inside `BodyCallback` or `HeadCallback`: those run
  while `run()` holds the lock, so `send()` or `cancel()` there deadlocks
  the same way. Return `Action::Abort` to stop a transfer from there.
- Make sure `run()` has returned (e.g. `stop()` then `thread.join()`)
  before destroying the `Client`. The destructor does a best-effort
  cleanup of anything still in flight, but does not synchronize with a
  concurrently-running `run()`.

## libcurl global state

Even though `client.hpp` hides libcurl entirely, the *application* still
needs to manage libcurl's process-wide state, exactly once, from
`<curl/curl.h>`:

- Call `curl_global_init(CURL_GLOBAL_DEFAULT)` once before constructing
  any `Client`, and `curl_global_cleanup()` once after destroying all of
  them. libcurl documents `curl_global_init` as not thread-safe and not
  safe to pair per-instance across independent components (the last
  `curl_global_cleanup()` call tears down global state, e.g. OpenSSL,
  that any other still-alive user of libcurl -- including another
  `Client` -- may depend on), so this class deliberately does not call it
  for you.
- On Windows, `CURL_GLOBAL_DEFAULT` already includes `CURL_GLOBAL_WIN32`
  and initializes Winsock (`WSAStartup`) for you -- no separate call
  needed for this module.
- For `run()` to truly never block, the linked libcurl needs to resolve
  DNS asynchronously (the threaded resolver, or c-ares) rather than
  synchronously via `getaddrinfo()`. This is the default for most libcurl
  builds, including vcpkg's `curl` port, but is worth confirming for a
  custom build.

## Ownership

`Client` owns the entire libcurl request lifecycle internally: it creates
the `CURL*` easy handle in `send()` and cleans it up itself right before
`CompletionCallback` runs. There is nothing for the caller to clean up.
`context` is opaque to `Client` too -- it is never read, copied, or freed.

## Known limitations

Compared to a full-featured platform HTTP stack (e.g. OkHttp on Android),
this client is deliberately just the transport layer. Gaps worth knowing
about before reaching for it:

- **No interceptor chain.** No built-in hook for auth-token refresh,
  logging, or retry policies -- implement that at the call site (e.g.
  inspect `Error`/`Response::status_code` in `CompletionCallback` and call
  `send()` again).
- **No HTTP disk cache.** `Cache-Control`/`ETag`/conditional requests are
  not interpreted; every `send()` hits the network.
- **No multipart/form upload builder.** libcurl's `curl_mime` API isn't
  wired in; only a single in-memory `Request::body` is supported.
- **WebSockets: no waiting for the server's close.** `close_websocket()`
  reports `on_close` as soon as its own close frame is out, rather than after
  the server's echo. permessage-deflate is the only extension, offered without
  `client_max_window_bits` (zlib cannot compress with an 8-bit window).
- **No connection/buffer pooling beyond what libcurl itself does.** Each
  `send()` allocates a fresh `Request`/`Response` (strings and vectors);
  under very high request-per-second churn this allocator traffic could
  become a bottleneck before libcurl's own connection reuse does --
  untested at that scale.
- **Tests cover plain HTTP/WebSocket only.** `tests/http/` runs against a
  loopback server (see the root README's Testing); TLS, pinning and HTTP/3
  were verified against real endpoints by hand, not by committed tests.
- **Not yet built through vcpkg's `curl`/`wolfssl` ports (or the
  vcpkg-share's `ports/wolfssl` overlay).** All verification so far
  (including TLS/gzip/pinning) linked the *system* libcurl + Secure
  Transport on macOS via CMake's `FindCURL` fallback, since vcpkg isn't
  installed on the machine this was developed on. vcpkg-share's `vcpkg.json`'s pinned
  `curl[wolfssl]` + `wolfssl[curve25519,ed25519]` configuration (see the
  root README's "Why `curl`'s TLS backend is pinned to wolfSSL") has not
  itself been built or exercised yet. Open questions: whether the overlay
  port's added `ed25519` feature actually builds, and whether
  `CURLOPT_PINNEDPUBLICKEY` behaves the same under wolfSSL as it did under
  OpenSSL/Secure Transport in the probes that were run.
  Rebuild and rerun this module's checks once vcpkg is available, since
  a vcpkg-built static wolfSSL is a materially different dependency
  graph than the system TLS stack this was actually tested against.
- **Only the kqueue (macOS) backend has run against real network
  traffic.** The epoll and Windows `Waker` backends this module depends
  on are implemented to the same documented behavior but have not been
  built or exercised on Linux or Windows yet -- reverify there before
  relying on this in production on those platforms.
- **Performance/memory relative to OkHttp is not benchmarked.**
  Architecturally this should have a lower per-connection memory
  footprint (no JVM/GC, no per-call pooled thread) and scale to more
  concurrent connections on one thread than OkHttp's bounded
  thread-pool `Dispatcher` -- but this is reasoning from design, not a
  measurement. If this module is driven from Kotlin/Java through JNI,
  the marshaling and `JavaVM::AttachCurrentThread` cost per
  callback likely dominates any of the above for typical
  request/response sizes; profile the actual workload before deciding
  based on architecture alone.
- **`cancel()` cannot be called on a handle from inside that handle's own
  `BodyCallback`** -- see the deadlock note under Cancelling. This is a
  sharp edge of the single-mutex design, not just a documentation
  reminder: there is no runtime detection of the mistake, it simply hangs.
