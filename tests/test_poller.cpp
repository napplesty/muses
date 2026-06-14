#include <doctest.h>

#include "muses/net/poller_factory.hpp"

#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
// Make a non-blocking connected socketpair; returns false on failure.
bool make_socketpair(int sv[2]) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(sv[i], F_GETFL, 0);
        ::fcntl(sv[i], F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}
}  // namespace

TEST_CASE("Poller: add and detect readable") {
    auto poller = muses::make_poller();
    REQUIRE(poller != nullptr);

    int sv[2];
    REQUIRE(make_socketpair(sv));

    int marker = 42;
    CHECK(poller->add(sv[0], muses::EventMask::Readable, &marker));

    // Write to sv[1] → sv[0] becomes readable.
    char byte = 'x';
    REQUIRE(::write(sv[1], &byte, 1) == 1);

    muses::PollEvent events[4];
    int n = poller->wait(events, 4, 200);
    CHECK(n == 1);
    CHECK(events[0].fd == sv[0]);
    CHECK(muses::has(events[0].mask, muses::EventMask::Readable));
    CHECK(events[0].userdata == &marker);

    // Drain the byte; next wait should time out (nothing readable).
    char rb;
    ::read(sv[0], &rb, 1);
    n = poller->wait(events, 4, 80);
    CHECK(n == 0);

    poller->del(sv[0]);
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE("Poller: del stops events") {
    auto poller = muses::make_poller();
    int sv[2];
    REQUIRE(make_socketpair(sv));
    CHECK(poller->add(sv[0], muses::EventMask::Readable, nullptr));
    CHECK(poller->del(sv[0]));

    char byte = 'y';
    ::write(sv[1], &byte, 1);
    muses::PollEvent events[4];
    int n = poller->wait(events, 4, 80);
    CHECK(n == 0);  // deleted → no events
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE("Poller: wakeup interrupts a blocked wait") {
    auto poller = muses::make_poller();
    int sv[2];
    REQUIRE(make_socketpair(sv));
    CHECK(poller->add(sv[0], muses::EventMask::Readable, nullptr));

    auto start = std::chrono::steady_clock::now();
    std::thread waker([&poller] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        poller->wakeup();
    });
    muses::PollEvent events[4];
    int n = poller->wait(events, 4, 5000);  // would block 5s if not woken
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start);
    waker.join();
    CHECK(n == 0);  // wakeup produces no output event
    CHECK(elapsed < std::chrono::milliseconds(1000));

    poller->del(sv[0]);
    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_CASE("Poller: timeout works") {
    auto poller = muses::make_poller();
    int sv[2];
    REQUIRE(make_socketpair(sv));
    CHECK(poller->add(sv[0], muses::EventMask::Readable, nullptr));

    auto start = std::chrono::steady_clock::now();
    muses::PollEvent events[4];
    int n = poller->wait(events, 4, 70);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start);
    CHECK(n == 0);
    CHECK(elapsed >= std::chrono::milliseconds(60));

    poller->del(sv[0]);
    ::close(sv[0]);
    ::close(sv[1]);
}
