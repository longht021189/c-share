#include "c_share/waker/waker.hpp"

#include "socket_pair.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using c_share::waker::Interest;
using c_share::waker::Waker;
using c_share::waker::WakeReason;
using namespace std::chrono_literals;

namespace {

// A connected socket pair, closed at the end of the test.
struct SocketPair {
    SocketPair() {
        const auto [c, s] = c_share::test::make_loopback_pair();
        client = c;
        server = s;
    }
    ~SocketPair() {
        c_share::test::close_socket(client);
        c_share::test::close_socket(server);
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    SocketPair(SocketPair&&) = delete;
    SocketPair& operator=(SocketPair&&) = delete;

    c_share::test::socket_handle client{};
    c_share::test::socket_handle server{};
};

} // namespace

TEST_CASE("Interest combines and tests its bits", "[waker]") {
    STATIC_REQUIRE((Interest::Read | Interest::Write) == Interest::ReadWrite);
    STATIC_REQUIRE((Interest::ReadWrite & Interest::Write) == Interest::Write);
    STATIC_REQUIRE((Interest::Read & Interest::Write) == Interest::None);
    STATIC_REQUIRE(c_share::waker::has_read(Interest::ReadWrite));
    STATIC_REQUIRE(c_share::waker::has_write(Interest::ReadWrite));
    STATIC_REQUIRE_FALSE(c_share::waker::has_write(Interest::Read));
    STATIC_REQUIRE_FALSE(c_share::waker::has_read(Interest::None));
}

TEST_CASE("wake() from another thread ends wait() with Manual", "[waker]") {
    Waker waker;
    std::thread waking([&waker] {
        std::this_thread::sleep_for(20ms);
        waker.wake();
    });
    CHECK(waker.wait() == WakeReason::Manual);
    waking.join();
}

TEST_CASE("a wake() before wait() is not lost", "[waker]") {
    Waker waker;
    waker.wake();
    CHECK(waker.wait() == WakeReason::Manual);
}

TEST_CASE("wake_after() ends wait() with Timeout, not before its delay", "[waker]") {
    Waker waker;
    const auto start = std::chrono::steady_clock::now();
    waker.wake_after(50ms);
    CHECK(waker.wait() == WakeReason::Timeout);
    CHECK(std::chrono::steady_clock::now() - start >= 45ms);
}

TEST_CASE("wake_after() replaces the timer set before", "[waker]") {
    Waker waker;
    const auto start = std::chrono::steady_clock::now();
    waker.wake_after(10s);
    waker.wake_after(20ms);
    CHECK(waker.wait() == WakeReason::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("cancel_timer() stops a timer from firing", "[waker]") {
    Waker waker;
    waker.wake_after(20ms);
    waker.cancel_timer();
    // Only the manual wake, well after the timer would have fired, ends it.
    std::thread waking([&waker] {
        std::this_thread::sleep_for(100ms);
        waker.wake();
    });
    CHECK(waker.wait() == WakeReason::Manual);
    waking.join();
    // Cancelling with no timer armed is fine too.
    waker.cancel_timer();
}

TEST_CASE("an attached socket wakes wait() once it is readable", "[waker]") {
    Waker waker;
    const SocketPair pair;
    waker.attach_socket(pair.server, Interest::Read);
    REQUIRE(c_share::test::send_all(pair.client, "hi"));

    REQUIRE(waker.wait() == WakeReason::Socket);
    const auto& ready = waker.take_ready_sockets();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].fd == pair.server);
    CHECK(c_share::waker::has_read(ready[0].ready));
    CHECK_FALSE(c_share::waker::has_write(ready[0].ready));
    CHECK(c_share::test::recv_some(pair.server) == "hi");
    waker.detach_socket(pair.server);
}

TEST_CASE("Write interest reports a connected socket as writable", "[waker]") {
    Waker waker;
    const SocketPair pair;
    waker.attach_socket(pair.client, Interest::Write);
    REQUIRE(waker.wait() == WakeReason::Socket);
    const auto& ready = waker.take_ready_sockets();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].fd == pair.client);
    CHECK(c_share::waker::has_write(ready[0].ready));
}

TEST_CASE("attaching again updates a socket's interest", "[waker]") {
    Waker waker;
    const SocketPair pair;
    // Nothing to read: with Read alone, only the manual wake ends wait().
    waker.attach_socket(pair.client, Interest::Read);
    waker.wake();
    CHECK(waker.wait() == WakeReason::Manual);

    waker.attach_socket(pair.client, Interest::ReadWrite);
    REQUIRE(waker.wait() == WakeReason::Socket);
    REQUIRE(waker.take_ready_sockets().size() == 1);
    CHECK(c_share::waker::has_write(waker.take_ready_sockets()[0].ready));
}

TEST_CASE("a detached socket no longer wakes wait()", "[waker]") {
    Waker waker;
    const SocketPair pair;
    waker.attach_socket(pair.server, Interest::Read);
    waker.detach_socket(pair.server);
    REQUIRE(c_share::test::send_all(pair.client, "hi"));
    waker.wake_after(50ms);
    CHECK(waker.wait() == WakeReason::Timeout);
    // Detaching what is not attached is a no-op.
    waker.detach_socket(pair.server);
}

TEST_CASE("a closed peer wakes a socket waiting to read", "[waker]") {
    Waker waker;
    SocketPair pair;
    waker.attach_socket(pair.server, Interest::Read);
    c_share::test::close_socket(pair.client);
    pair.client = c_share::test::make_loopback_pair().first; // something for the destructor to close
    REQUIRE(waker.wait() == WakeReason::Socket);
    REQUIRE(waker.take_ready_sockets().size() == 1);
    CHECK(c_share::test::recv_some(pair.server).empty());
    waker.detach_socket(pair.server);
}

TEST_CASE("several ready sockets come back from one wait()", "[waker]") {
    Waker waker;
    const SocketPair first;
    const SocketPair second;
    waker.attach_socket(first.server, Interest::Read);
    waker.attach_socket(second.server, Interest::Read);
    REQUIRE(c_share::test::send_all(first.client, "a"));
    REQUIRE(c_share::test::send_all(second.client, "b"));
    std::this_thread::sleep_for(20ms); // both arrive before the wait

    REQUIRE(waker.wait() == WakeReason::Socket);
    const auto& ready = waker.take_ready_sockets();
    CHECK(ready.size() == 2);
}
