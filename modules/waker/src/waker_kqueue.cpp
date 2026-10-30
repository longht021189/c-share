#include "c_share/waker/waker.hpp"

#include <sys/event.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <system_error>
#include <unordered_map>

// Waker's methods all change it through impl_, where CLion does not look, so
// it would make each of them const.
// ReSharper disable CppMemberFunctionMayBeConst

namespace c_share::waker {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

constexpr uintptr_t kWakeIdent = 1;
constexpr uintptr_t kTimerIdent = 2;

} // namespace

struct Waker::Impl {
    int kq = -1;
    std::unordered_map<int, Interest> sockets;
    std::vector<ReadySocket> ready;

    Impl() : kq(kqueue()) {
        if (kq < 0) throw_errno("kqueue");

        struct kevent ev{};
        // EV_CLEAR resets the user event after each delivery, so wake()
        // behaves as a one-shot signal rather than staying "readable".
        EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) throw_errno("kevent(wake init)");
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        if (kq >= 0) close(kq);
    }

    void push_ready(const int fd, const Interest bit) {
        for (auto& [ready_fd, ready_interest] : ready) {
            if (ready_fd == fd) {
                ready_interest = ready_interest | bit;
                return;
            }
        }
        ready.push_back({fd, bit});
    }
};

Waker::Waker() : impl_(std::make_unique<Impl>()) {}
Waker::~Waker() = default;

WakeReason Waker::wait() {
    impl_->ready.clear();

    struct kevent events[16];
    int n = 0;
    do {
        n = kevent(impl_->kq, nullptr, 0, events, 16, nullptr);
    } while (n < 0 && errno == EINTR);
    if (n < 0) throw_errno("kevent");

    bool manual = false;
    bool timeout = false;

    for (int i = 0; i < n; ++i) {
        if (const struct kevent& e = events[i]; e.filter == EVFILT_USER && e.ident == kWakeIdent) {
            manual = true;
        } else if (e.filter == EVFILT_TIMER && e.ident == kTimerIdent) {
            timeout = true;
        } else if (e.filter == EVFILT_READ) {
            // EV_EOF means the peer closed or an error occurred; surface
            // both directions so the caller's next read or write is the one
            // that observes it.
            impl_->push_ready(static_cast<int>(e.ident), e.flags & EV_EOF ? Interest::ReadWrite : Interest::Read);
        } else if (e.filter == EVFILT_WRITE) {
            impl_->push_ready(static_cast<int>(e.ident), e.flags & EV_EOF ? Interest::ReadWrite : Interest::Write);
        }
    }

    if (manual) return WakeReason::Manual;
    if (timeout) return WakeReason::Timeout;
    return WakeReason::Socket;
}

void Waker::wake() {
    struct kevent ev{};
    EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(impl_->kq, &ev, 1, nullptr, 0, nullptr);
}

void Waker::wake_after(const std::chrono::milliseconds delay) {
    struct kevent ev{};
    // EV_ONESHOT auto-removes the timer once it fires, matching the
    // epoll/timerfd backend's one-shot semantics.
    EV_SET(&ev, kTimerIdent, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, static_cast<intptr_t>(delay.count()), nullptr);
    if (kevent(impl_->kq, &ev, 1, nullptr, 0, nullptr) < 0) throw_errno("kevent(timer)");
}

void Waker::cancel_timer() {
    struct kevent ev{};
    EV_SET(&ev, kTimerIdent, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    // Ignore ENOENT: the timer may have already fired (EV_ONESHOT
    // self-removes) or never been armed.
    if (kevent(impl_->kq, &ev, 1, nullptr, 0, nullptr) < 0 && errno != ENOENT) {
        throw_errno("kevent(timer cancel)");
    }
}

void Waker::attach_socket(const socket_t fd, const Interest interest) {
    const auto it = impl_->sockets.find(fd);
    const Interest previous = it == impl_->sockets.end() ? Interest::None : it->second;
    if (previous == interest) return;

    std::array<struct kevent, 2> changes{};
    int count = 0;

    const bool want_read = has_read(interest);
    const bool want_write = has_write(interest);
    const bool had_read = has_read(previous);
    const bool had_write = has_write(previous);

    if (want_read != had_read) {
        EV_SET(&changes[count++], fd, EVFILT_READ, want_read ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }
    if (want_write != had_write) {
        EV_SET(&changes[count++], fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }

    if (count > 0 && kevent(impl_->kq, changes.data(), count, nullptr, 0, nullptr) < 0) {
        throw_errno("kevent(socket update)");
    }
    impl_->sockets[fd] = interest;
}

void Waker::detach_socket(const socket_t fd) {
    const auto it = impl_->sockets.find(fd);
    if (it == impl_->sockets.end()) return;

    std::array<struct kevent, 2> changes{};
    int count = 0;
    if (has_read(it->second)) EV_SET(&changes[count++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    if (has_write(it->second)) EV_SET(&changes[count++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    if (count > 0) kevent(impl_->kq, changes.data(), count, nullptr, 0, nullptr);

    impl_->sockets.erase(it);
}

const std::vector<ReadySocket>& Waker::take_ready_sockets() const {
    return impl_->ready;
}

} // namespace c_share::waker
