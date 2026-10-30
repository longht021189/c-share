#pragma once

// What this file uses, even where CLion finds it through other headers (or,
// for <cstdint>, only on Windows).
// ReSharper disable CppUnusedIncludeDirective
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::waker {

#ifdef _WIN32
using socket_t = std::uintptr_t; // SOCKET
#else
using socket_t = int;
#endif

enum class WakeReason {
    Manual,  // wake() was called
    Timeout, // a wake_after() timer fired
    Socket,  // a registered socket became ready
};

enum class Interest : std::uint8_t {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    ReadWrite = Read | Write,
};

constexpr Interest operator|(const Interest a, const Interest b) {
    return static_cast<Interest>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

// has_read/has_write call it; CLion's data flow analysis says no call is reachable.
// ReSharper disable once CppDFAUnreachableFunctionCall
constexpr Interest operator&(const Interest a, const Interest b) {
    return static_cast<Interest>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool has_read(const Interest i) {
    return (i & Interest::Read) == Interest::Read;
}

constexpr bool has_write(const Interest i) {
    return (i & Interest::Write) == Interest::Write;
}

struct ReadySocket {
    socket_t fd;
    Interest ready; // which of Read/Write (or both) fired
};

// Blocks a single waiting thread until one of three triggers fires: an
// explicit wake(), a scheduled wake_after() timer, or a registered socket
// becoming readable/writable. Backed by epoll (Linux/Android), kqueue
// (macOS/BSD), or WaitForMultipleObjects (Windows) -- see the platform
// notes in this module's README before relying on socket wake-ups.
//
// wait() must only be called from one thread at a time. wake(),
// wake_after(), cancel_timer(), attach_socket() and detach_socket() are
// safe to call from any thread, including while another thread is blocked
// in wait().
class Waker {
public:
    Waker();
    ~Waker();

    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    Waker(Waker&&) = delete;
    Waker& operator=(Waker&&) = delete;

    // Blocks until woken. Returns the reason for the wake-up.
    WakeReason wait();

    // Wakes a blocked (or the next) wait() call immediately.
    void wake();

    // Schedules a one-shot wake-up after `delay`. Replaces any previously
    // scheduled, not-yet-fired timer.
    void wake_after(std::chrono::milliseconds delay);

    // Cancels a pending wake_after() timer, if any. No-op if none is armed
    // (e.g. it already fired, or wake_after() was never called).
    void cancel_timer();

    // Registers `fd` so wait() also returns when it satisfies `interest`
    // (readable, writable, or both). Calling this again for an
    // already-attached `fd` updates its interest set in place -- this maps
    // directly onto CURL_POLL_IN/OUT/INOUT from libcurl's
    // CURLMOPT_SOCKETFUNCTION, for example.
    // On Windows, at most 62 sockets may be attached at once (a
    // WaitForMultipleObjects limitation) -- see the module README.
    void attach_socket(socket_t fd, Interest interest = Interest::Read);

    // Unregisters a socket previously passed to attach_socket().
    void detach_socket(socket_t fd);

    // Sockets that were ready during the most recent wait() call that
    // returned WakeReason::Socket, and which interest(s) fired. The
    // returned reference points into a buffer owned by this Waker: it stays
    // valid until the next wait() call, which clears and reuses it (no
    // reallocation in steady state) rather than handing out a fresh vector.
    [[nodiscard]] const std::vector<ReadySocket>& take_ready_sockets() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace c_share::waker
