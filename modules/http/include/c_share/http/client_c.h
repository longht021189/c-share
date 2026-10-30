#ifndef C_SHARE_HTTP_CLIENT_C_H
#define C_SHARE_HTTP_CLIENT_C_H

/*
 * Plain C API around c_share::http::Client (see client.hpp), so it can be
 * consumed directly by bindgen from Rust, or by any other language with a
 * C FFI, without a C++ ABI in the way -- same reasoning as crypto.h. No
 * C++ type (std::string, std::vector, the Client class itself) crosses
 * this boundary; strings/arrays are passed as pointer + length pairs
 * (never NUL-terminated by convention -- always use the accompanying
 * *_len), and the client is an opaque handle.
 *
 * Ownership: c_share_http_client_send() copies everything it needs out of
 * `request` synchronously before returning, so the caller's buffers only
 * need to stay alive for that call. `response`/`error` handed to
 * c_share_http_completion_cb are, in turn, only valid for the duration of
 * that one call -- copy anything needed out of them before returning.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c_share_http_client c_share_http_client_t;

typedef enum {
    C_SHARE_HTTP_METHOD_GET = 0,
    C_SHARE_HTTP_METHOD_POST,
    C_SHARE_HTTP_METHOD_PUT,
    C_SHARE_HTTP_METHOD_PATCH,
    C_SHARE_HTTP_METHOD_DELETE,
    C_SHARE_HTTP_METHOD_HEAD,
    C_SHARE_HTTP_METHOD_OPTIONS,
} c_share_http_method_t;

typedef struct {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
} c_share_http_header_t;

typedef struct {
    const char* url;
    size_t url_len;
    c_share_http_method_t method;
    const c_share_http_header_t* headers;
    size_t headers_len;
    const uint8_t* body; /* ignored for Get/Head/Options unless set */
    size_t body_len;
    int64_t timeout_ms; /* 0 = no timeout */
    /* Optional certificate/public-key pinning -- see Request::pinned_public_key
     * in client.hpp. NULL disables pinning (normal CA validation). */
    const char* pinned_public_key;
    size_t pinned_public_key_len;
    /* Nonzero to let libcurl follow 301/302/303/307/308 redirects, zero to
     * return them as final -- see Request::follow_redirects in client.hpp.
     * Note a zeroed struct therefore does not follow. */
    int follow_redirects;
} c_share_http_request_t;

typedef struct {
    long status_code;
    const c_share_http_header_t* headers;
    size_t headers_len;
    const uint8_t* body; /* empty if a body callback was supplied to send() */
    size_t body_len;
} c_share_http_response_t;

typedef enum {
    C_SHARE_HTTP_ERROR_NONE = 0,
    C_SHARE_HTTP_ERROR_INVALID_URL,
    C_SHARE_HTTP_ERROR_COULD_NOT_RESOLVE_HOST,
    C_SHARE_HTTP_ERROR_COULD_NOT_CONNECT,
    C_SHARE_HTTP_ERROR_TLS_ERROR,
    C_SHARE_HTTP_ERROR_TIMEOUT,
    C_SHARE_HTTP_ERROR_ABORTED,         /* a body callback aborted, or cancel() was called */
    C_SHARE_HTTP_ERROR_TRANSPORT_ERROR, /* anything else -- see message */
} c_share_http_error_code_t;

typedef struct {
    c_share_http_error_code_t code;
    const char* message;
    size_t message_len;
} c_share_http_error_t;

typedef enum {
    C_SHARE_HTTP_ACTION_CONTINUE = 0,
    C_SHARE_HTTP_ACTION_ABORT,
} c_share_http_action_t;

/* Called once per final response, as soon as its header block ends -- before
 * any body data; see HeadCallback in client.hpp. `headers` is only valid for
 * the duration of this call. Return C_SHARE_HTTP_ACTION_ABORT to abort the
 * transfer right there. May be NULL. */
typedef c_share_http_action_t (*c_share_http_head_cb)(void* context, long status_code,
                                                      const c_share_http_header_t* headers, size_t headers_len);

/* Called with each chunk of response body data as it arrives; see
 * BodyCallback in client.hpp. May be NULL, in which case the body is
 * buffered whole into c_share_http_response_t::body for the completion
 * callback. `data` is only valid for the duration of this call. */
typedef c_share_http_action_t (*c_share_http_body_cb)(void* context, const uint8_t* data, size_t size);

/* Called exactly once per c_share_http_client_send() call that returns a
 * non-zero handle (see c_share_http_client_send). */
typedef void (*c_share_http_completion_cb)(void* context, c_share_http_response_t response, c_share_http_error_t error);

/* Creates a client and starts its event loop on a thread of its own. Returns
 * NULL on failure (e.g. curl_multi_init or thread creation failed). The first
 * client also does process-wide libcurl setup (curl_global_init); the matching
 * curl_global_cleanup runs once the last client is destroyed. */
c_share_http_client_t* c_share_http_client_create(void);

/* Stops the client's event loop, waits for that thread to finish, fails every
 * request still in flight (its completion callback runs with
 * C_SHARE_HTTP_ERROR_ABORTED), and frees the client. NULL is a no-op.
 *
 * Must not be called from inside any callback of this client: those run on
 * the event-loop thread, which would then wait for itself. */
void c_share_http_client_destroy(c_share_http_client_t* client);

/* Sends `request`. `context` is passed back to `head_cb`/`body_cb`
 * (each optional, may be NULL) and `done_cb` verbatim; nothing here reads,
 * copies or frees it -- that is the caller's responsibility, typically
 * inside `done_cb`. Safe to call from any thread.
 *
 * Returns an opaque non-zero handle on success (pass to
 * c_share_http_client_cancel to cancel), for which `done_cb` will run
 * exactly once. Returns 0 if the request could not even be submitted
 * (e.g. curl_easy_init failed); `done_cb` never runs in that case, so the
 * caller must free anything it stashed in `context` itself. */
uint64_t c_share_http_client_send(c_share_http_client_t* client, const c_share_http_request_t* request, void* context,
                                  c_share_http_body_cb body_cb, c_share_http_completion_cb done_cb,
                                  c_share_http_head_cb head_cb);

/* Cancels a running request: `done_cb` still runs, with
 * C_SHARE_HTTP_ERROR_ABORTED. No-op if `handle` is unknown, already
 * finished, or 0. Safe to call from any thread except from inside that
 * same request's body callback -- see client.hpp's Cancelling notes. */
void c_share_http_client_cancel(c_share_http_client_t* client, uint64_t handle);

/* Called exactly once per timer: when its delay has passed (`cancelled` 0),
 * or once c_share_http_client_cancel or the client's destruction ended it
 * first (`cancelled` 1). Runs on the client's loop thread. */
typedef void (*c_share_http_timer_cb)(void* context, int cancelled);

/* Calls `callback` on the client's loop thread once `delay_ms` has passed;
 * c_share_http_client_cancel with the returned handle ends it early.
 * Returns 0, never calling `callback`, if `client` or `callback` is NULL.
 * Safe to call from any thread. */
uint64_t c_share_http_client_set_timer(c_share_http_client_t* client, uint64_t delay_ms, void* context,
                                       c_share_http_timer_cb callback);

/* ---- WebSocket: see Client::open_websocket in client.hpp ---- */

typedef enum {
    C_SHARE_HTTP_WS_TEXT = 0,
    C_SHARE_HTTP_WS_BINARY,
} c_share_http_ws_type_t;

/* The server accepted the upgrade; `headers` are those of its 101 response,
 * only valid for the duration of the call. May be NULL. */
typedef void (*c_share_http_ws_open_cb)(void* context, long status_code, const c_share_http_header_t* headers,
                                        size_t headers_len);

/* One complete data message; `data` is only valid for the duration of the
 * call. May be NULL. */
typedef void (*c_share_http_ws_message_cb)(void* context, c_share_http_ws_type_t type, const uint8_t* data,
                                           size_t size);

/* Called exactly once per handle c_share_http_client_ws_open() returned, after
 * every other callback for it: `error.code` is C_SHARE_HTTP_ERROR_NONE for a
 * close handshake (with its `code`, 1005 if none, and `reason`), or says what
 * failed (`code` is then 1006). `reason`/`error.message` are only valid for
 * the duration of the call. */
typedef void (*c_share_http_ws_close_cb)(void* context, c_share_http_error_t error, uint16_t code, const char* reason,
                                         size_t reason_len);

/* See WsOptions in client.hpp. A zeroed struct is the default. */
typedef struct {
    /* Nonzero to offer permessage-deflate (compression), as browsers do. */
    int deflate;
} c_share_http_ws_options_t;

/* Opens a WebSocket to `request->url` (ws:// or wss://), with its headers;
 * `method`, `body` and `follow_redirects` are ignored and `timeout_ms` bounds
 * the handshake. `options` may be NULL for the defaults. Returns a non-zero
 * handle on success, for which `close_cb` (required) runs exactly once; 0 if
 * it could not be submitted, in which case no callback ever runs. Safe to
 * call from any thread. */
uint64_t c_share_http_client_ws_open(c_share_http_client_t* client, const c_share_http_request_t* request,
                                     const c_share_http_ws_options_t* options, void* context,
                                     c_share_http_ws_open_cb open_cb, c_share_http_ws_message_cb message_cb,
                                     c_share_http_ws_close_cb close_cb);

/* Queues a message, copied before returning. Returns 0 on success, -1 if
 * `handle` is unknown or already closing. Safe to call from any thread. */
int c_share_http_client_ws_send(c_share_http_client_t* client, uint64_t handle, c_share_http_ws_type_t type,
                                const uint8_t* data, size_t size);

/* Closes the WebSocket `handle` with `code`/`reason` once the messages queued
 * before it are out (aborts it if the handshake is still running). No-op if
 * `handle` is unknown or already closing. Safe to call from any thread. */
void c_share_http_client_ws_close(c_share_http_client_t* client, uint64_t handle, uint16_t code, const char* reason,
                                  size_t reason_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* C_SHARE_HTTP_CLIENT_C_H */
