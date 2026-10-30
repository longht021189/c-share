#include "c_share/waker/waker.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
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

uint32_t epoll_events_for(const Interest interest) {
    uint32_t events = 0;
    if (has_read(interest)) events |= EPOLLIN;
    if (has_write(interest)) events |= EPOLLOUT;
    return events;
}

} // namespace

struct Waker::Impl {
    int epoll_fd = -1;
    int wake_fd = -1;  // eventfd, triggered by wake()
    int timer_fd = -1; // timerfd, triggered by wake_after()
    std::unordered_map<int, Interest> sockets;
    std::vector<ReadySocket> ready;

    Impl() : epoll_fd(epoll_create1(EPOLL_CLOEXEC)) {
        if (epoll_fd < 0) throw_errno("epoll_create1");
        // A constructor that throws gets no destructor call: close what is
        // already open here.
        try {
            wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (wake_fd < 0) throw_errno("eventfd");

            timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            if (timer_fd < 0) throw_errno("timerfd_create");

            add_control_fd(wake_fd);
            add_control_fd(timer_fd);
        } catch (...) {
            close_fds();
            throw;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { close_fds(); }

    void close_fds() const {
        if (epoll_fd >= 0) close(epoll_fd);
        if (wake_fd >= 0) close(wake_fd);
        if (timer_fd >= 0) close(timer_fd);
    }

    void add_control_fd(int fd) const {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl(ADD)");
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

    epoll_event events[16];
    int n = 0;
    do {
        n = epoll_wait(impl_->epoll_fd, events, 16, -1);
    } while (n < 0 && errno == EINTR);
    if (n < 0) throw_errno("epoll_wait");

    bool manual = false;
    bool timeout = false;

    for (int i = 0; i < n; ++i) {
        const int fd = events[i].data.fd;
        const uint32_t flags = events[i].events;

        if (fd == impl_->wake_fd) {
            uint64_t v = 0;
            while (::read(impl_->wake_fd, &v, sizeof(v)) > 0) {
            }
            manual = true;
        } else if (fd == impl_->timer_fd) {
            uint64_t v = 0;
            // Only resets the timerfd; how often it expired does not matter.
            [[maybe_unused]] const auto expirations = ::read(impl_->timer_fd, &v, sizeof(v));
            timeout = true;
        } else if (flags & (EPOLLERR | EPOLLHUP)) {
            // Surface both directions on error/hangup so the caller's next
            // read or write call is the one that observes the actual error.
            impl_->push_ready(fd, Interest::ReadWrite);
        } else {
            if (flags & EPOLLIN) impl_->push_ready(fd, Interest::Read);
            if (flags & EPOLLOUT) impl_->push_ready(fd, Interest::Write);
        }
    }

    if (manual) return WakeReason::Manual;
    if (timeout) return WakeReason::Timeout;
    return WakeReason::Socket;
}

void Waker::wake() {
    const uint64_t one = 1;
    // Fails only when the eventfd's counter is full, which still wakes wait().
    // (GCC's warn_unused_result ignores a (void) cast.)
    [[maybe_unused]] const auto written = ::write(impl_->wake_fd, &one, sizeof(one));
}

void Waker::wake_after(const std::chrono::milliseconds delay) {
    itimerspec spec{};
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(delay);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay - secs);
    spec.it_value.tv_sec = secs.count();
    spec.it_value.tv_nsec = ns.count();
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        // A zero it_value disarms the timer; fire on the next tick instead.
        spec.it_value.tv_nsec = 1;
    }
    if (timerfd_settime(impl_->timer_fd, 0, &spec, nullptr) < 0) throw_errno("timerfd_settime");
}

void Waker::cancel_timer() {
    itimerspec spec{}; // all-zero disarms the timer
    timerfd_settime(impl_->timer_fd, 0, &spec, nullptr);
}

void Waker::attach_socket(const socket_t fd, const Interest interest) {
    epoll_event ev{};
    ev.events = epoll_events_for(interest);
    ev.data.fd = fd;

    const auto it = impl_->sockets.find(fd);
    if (it == impl_->sockets.end()) {
        if (epoll_ctl(impl_->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl(ADD)");
        impl_->sockets.emplace(fd, interest);
    } else if (it->second != interest) {
        if (epoll_ctl(impl_->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) throw_errno("epoll_ctl(MOD)");
        it->second = interest;
    }
}

void Waker::detach_socket(const socket_t fd) {
    if (impl_->sockets.erase(fd) == 0) return;
    epoll_ctl(impl_->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

const std::vector<ReadySocket>& Waker::take_ready_sockets() const {
    return impl_->ready;
}

} // namespace c_share::waker
