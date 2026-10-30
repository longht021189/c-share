#include "c_share/waker/waker.hpp"

// winsock2.h must be included before windows.h.
#include <windows.h>
#include <winsock2.h>

#include <stdexcept>
#include <system_error>
#include <unordered_map>

// Waker's methods all change it through impl_, where CLion does not look, so
// it would make each of them const.
// ReSharper disable CppMemberFunctionMayBeConst

namespace c_share::waker {

namespace {

[[noreturn]] void throw_last_error(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

// Slots 0 and 1 of the wait array are always the wake event and the timer;
// registered sockets fill the rest. WaitForMultipleObjects caps the total
// array at MAXIMUM_WAIT_OBJECTS (64).
constexpr size_t kReservedSlots = 2;
constexpr size_t kMaxSockets = MAXIMUM_WAIT_OBJECTS - kReservedSlots;

long network_events_for(const Interest interest) {
    long events = FD_CLOSE;
    if (has_read(interest)) events |= FD_READ;
    if (has_write(interest)) events |= FD_WRITE;
    return events;
}

} // namespace

struct Waker::Impl {
    struct SocketState {
        HANDLE event;
        Interest interest;
    };

    HANDLE wake_event = nullptr; // auto-reset event, signaled by wake()
    HANDLE timer = nullptr;      // auto-reset waitable timer, armed by wake_after()
    std::unordered_map<socket_t, SocketState> sockets;
    std::vector<ReadySocket> ready;

    Impl() {
        wake_event = CreateEventW(nullptr, /*bManualReset=*/FALSE, FALSE, nullptr);
        if (!wake_event) throw_last_error("CreateEvent");

        // Auto-reset so a fired timer behaves as one-shot without extra
        // bookkeeping, matching the epoll/kqueue backends.
        timer = CreateWaitableTimerW(nullptr, /*bManualReset=*/FALSE, nullptr);
        if (!timer) throw_last_error("CreateWaitableTimer");
    }

    ~Impl() {
        for (auto& [fd, state] : sockets) {
            WSAEventSelect(static_cast<SOCKET>(fd), nullptr, 0);
            WSACloseEvent(state.event);
        }
        if (timer) CloseHandle(timer);
        if (wake_event) CloseHandle(wake_event);
    }

    void collect_ready(HANDLE ev, const socket_t fd) {
        WSANETWORKEVENTS net_events;
        if (WSAEnumNetworkEvents(static_cast<SOCKET>(fd), ev, &net_events) != 0) return;

        Interest ready_bits = Interest::None;
        if (net_events.lNetworkEvents & (FD_READ | FD_CLOSE)) ready_bits = ready_bits | Interest::Read;
        if (net_events.lNetworkEvents & (FD_WRITE | FD_CLOSE)) ready_bits = ready_bits | Interest::Write;
        if (static_cast<uint8_t>(ready_bits) != 0) {
            ready.push_back({fd, ready_bits});
        }
    }
};

Waker::Waker() : impl_(std::make_unique<Impl>()) {}
Waker::~Waker() = default;

WakeReason Waker::wait() {
    impl_->ready.clear();

    std::vector<HANDLE> handles;
    std::vector<socket_t> handle_sockets;
    handles.reserve(kReservedSlots + impl_->sockets.size());
    handle_sockets.reserve(kReservedSlots + impl_->sockets.size());

    handles.push_back(impl_->wake_event);
    handle_sockets.push_back(0);
    handles.push_back(impl_->timer);
    handle_sockets.push_back(0);
    for (const auto& [fd, state] : impl_->sockets) {
        handles.push_back(state.event);
        handle_sockets.push_back(fd);
    }

    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
    if (result == WAIT_FAILED) throw_last_error("WaitForMultipleObjects");

    const DWORD idx = result - WAIT_OBJECT_0;
    if (idx == 0) return WakeReason::Manual;
    if (idx == 1) return WakeReason::Timeout;

    // A socket became ready. Collect it, then poll (0 timeout) the rest so
    // a single wait() call reports every socket that is ready right now,
    // not just the one WaitForMultipleObjects happened to pick.
    impl_->collect_ready(handles[idx], handle_sockets[idx]);
    for (size_t i = kReservedSlots; i < handles.size(); ++i) {
        if (i == idx) continue;
        if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) {
            impl_->collect_ready(handles[i], handle_sockets[i]);
        }
    }

    return WakeReason::Socket;
}

void Waker::wake() {
    SetEvent(impl_->wake_event);
}

void Waker::wake_after(const std::chrono::milliseconds delay) {
    LARGE_INTEGER due_time;
    // Negative = relative time, in 100-nanosecond units.
    due_time.QuadPart = -static_cast<LONGLONG>(delay.count()) * 10'000;
    if (!SetWaitableTimer(impl_->timer, &due_time, 0, nullptr, nullptr, FALSE)) {
        throw_last_error("SetWaitableTimer");
    }
}

void Waker::cancel_timer() {
    CancelWaitableTimer(impl_->timer);
}

void Waker::attach_socket(const socket_t fd, const Interest interest) {
    const auto it = impl_->sockets.find(fd);
    if (it != impl_->sockets.end()) {
        if (it->second.interest == interest) return;
        if (WSAEventSelect(static_cast<SOCKET>(fd), it->second.event, network_events_for(interest)) != 0) {
            throw_last_error("WSAEventSelect");
        }
        it->second.interest = interest;
        return;
    }

    if (impl_->sockets.size() >= kMaxSockets) {
        throw std::length_error("c_share::waker::Waker: too many attached sockets (Windows caps at "
                                "MAXIMUM_WAIT_OBJECTS)");
    }

    HANDLE ev = WSACreateEvent();
    if (ev == WSA_INVALID_EVENT) throw_last_error("WSACreateEvent");
    if (WSAEventSelect(static_cast<SOCKET>(fd), ev, network_events_for(interest)) != 0) {
        WSACloseEvent(ev);
        throw_last_error("WSAEventSelect");
    }
    impl_->sockets.emplace(fd, Impl::SocketState{ev, interest});
}

void Waker::detach_socket(const socket_t fd) {
    const auto it = impl_->sockets.find(fd);
    if (it == impl_->sockets.end()) return;
    WSAEventSelect(static_cast<SOCKET>(fd), nullptr, 0);
    WSACloseEvent(it->second.event);
    impl_->sockets.erase(it);
}

const std::vector<ReadySocket>& Waker::take_ready_sockets() const {
    return impl_->ready;
}

} // namespace c_share::waker
