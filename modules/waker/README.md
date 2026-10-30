# c_share::waker

A `Waker` blocks one thread until it is woken by one of three triggers:

- `wake()` — wake up immediately.
- `wake_after(duration)` / `cancel_timer()` — schedule or cancel a one-shot
  wake-up later.
- `attach_socket(fd, interest)` — wake up when a socket is readable,
  writable, or both.

Backend picked automatically by `CMakeLists.txt`:

| Platform        | Backend                                                                                                                                          |
|-----------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| Linux / Android | `epoll` (`EPOLLIN`/`EPOLLOUT`) + `eventfd` (wake) + `timerfd` (timer)                                                                            |
| macOS / BSD     | `kqueue` (`EVFILT_READ`/`EVFILT_WRITE`, `EVFILT_USER` for wake, `EVFILT_TIMER`)                                                                  |
| Windows         | `WaitForMultipleObjects` over an auto-reset event, a waitable timer, and one `WSAEventSelect` event per socket (`FD_READ`/`FD_WRITE`/`FD_CLOSE`) |

## Usage

```cpp
#include "c_share/waker/waker.hpp"

c_share::waker::Waker waker;
waker.attach_socket(my_socket_fd, c_share::waker::Interest::Read);
waker.wake_after(std::chrono::seconds(5)); // e.g. a heartbeat

switch (waker.wait()) {
    case c_share::waker::WakeReason::Manual:
        // someone called wake()
        break;
    case c_share::waker::WakeReason::Timeout:
        // the wake_after() timer fired
        break;
    case c_share::waker::WakeReason::Socket:
        for (const auto& ready : waker.take_ready_sockets()) {
            if (c_share::waker::has_read(ready.ready)) { /* recv from ready.fd */ }
            if (c_share::waker::has_write(ready.ready)) { /* send to ready.fd */ }
        }
        break;
}
```

`take_ready_sockets()` returns a reference into a buffer owned by the
`Waker`, not a fresh vector -- it's only valid until the next `wait()`
call (which clears and reuses the same storage), so consume it before
calling `wait()` again.

Calling `attach_socket()` again for a socket already attached *updates* its
interest set (e.g. `Read` -> `ReadWrite`) instead of adding a duplicate
registration.

`wait()` is meant to be called from a single thread in a loop. `wake()`,
`wake_after()`, `cancel_timer()`, `attach_socket()` and `detach_socket()`
are safe to call concurrently from other threads.

## Integrating with libcurl's multi-socket API (CURLM)

This API was shaped to map 1:1 onto `CURLMOPT_SOCKETFUNCTION` /
`CURLMOPT_TIMERFUNCTION`:

| curl                                       | waker                                                                                                                                                                     |
|--------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| socket callback, `what = CURL_POLL_IN`     | `attach_socket(fd, Interest::Read)`                                                                                                                                       |
| socket callback, `what = CURL_POLL_OUT`    | `attach_socket(fd, Interest::Write)`                                                                                                                                      |
| socket callback, `what = CURL_POLL_INOUT`  | `attach_socket(fd, Interest::ReadWrite)`                                                                                                                                  |
| socket callback, `what = CURL_POLL_REMOVE` | `detach_socket(fd)` -- call this **before** curl closes the fd                                                                                                            |
| timer callback, `timeout_ms >= 0`          | `wake_after(std::chrono::milliseconds(timeout_ms))`                                                                                                                       |
| timer callback, `timeout_ms == -1`         | `cancel_timer()`                                                                                                                                                          |
| `wait()` returns `Socket`                  | for each `ready`: `curl_multi_socket_action(multi, ready.fd, (has_read(ready.ready) ? CURL_CSELECT_IN : 0) \| (has_write(ready.ready) ? CURL_CSELECT_OUT : 0), &running)` |
| `wait()` returns `Timeout`                 | `curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running)`                                                                                                       |

**Do not `recv()`/`send()` the socket yourself.** curl owns the actual I/O
on sockets registered through its multi-socket API; `Waker` only tells you
*when* to call `curl_multi_socket_action()`, which is what performs the
real read/write. Draining the socket yourself first (per the general advice
below) will make curl miss data and hang or error out the transfer.

`CURLM*` handles are not safe to use from multiple threads concurrently --
if `wake()`/`attach_socket()` are called from a thread other than the one
driving `curl_multi_socket_action()`, you still need your own lock around
the actual curl calls.

## Platform notes (read before relying on socket wake-ups)

- **Level- vs edge-triggered reads/writes.** On Linux and macOS, a
  registered socket keeps reporting readiness on every `wait()` call for as
  long as it stays readable/writable (level-triggered) — safe to leave data
  unread or a write short, it will be reported again.
  **On Windows this is different.** `WSAEventSelect` only re-signals
  `FD_READ` when *new* data arrives after the last `WSAEnumNetworkEvents`
  call, and `FD_WRITE` only after a `send()` that previously returned
  `WSAEWOULDBLOCK` (effectively edge-triggered). For any code path where
  *you* (not curl) are doing the I/O, always loop `recv()`/`send()` until
  `EWOULDBLOCK`/`WSAEWOULDBLOCK` on every `Socket` wake, on every platform,
  to stay portable. (This does not apply to the CURLM integration above,
  where curl does the I/O, not you.)
- **Sockets should be non-blocking** so a drain loop can terminate on
  `EWOULDBLOCK` / `WSAEWOULDBLOCK` instead of hanging.
- **Error/hangup reporting.** All three backends report a socket error or
  peer hangup as *both* `Read` and `Write` ready, so whichever direction
  your code touches next surfaces the real error from `recv()`/`send()`.
- **Windows socket limit.** `WaitForMultipleObjects` caps out at
  `MAXIMUM_WAIT_OBJECTS` (64): 2 slots are reserved (wake event + timer),
  leaving at most **62** sockets attached to a single `Waker` at once.
  `attach_socket()` throws `std::length_error` past that limit. Linux and
  macOS have no such cap. If a single Windows deployment needs more
  sockets, shard them across multiple `Waker` instances/threads.
- **Windows requires Winsock initialized.** Call `WSAStartup()` once at
  process startup (and `WSACleanup()` at shutdown) before constructing a
  `Waker` that will use `attach_socket()` — this module does not own that
  lifecycle since it is process-wide.
- **Timers are one-shot.** `wake_after()` schedules a single future
  wake-up; call it again (from a timer callback, etc.) if you need a
  repeating tick. A new call replaces any previously scheduled, unfired
  timer; `cancel_timer()` disarms it without scheduling a new one.
