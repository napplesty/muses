#include <doctest.h>

#include "muses/task.hpp"
#include "muses/net/poller_factory.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace {

// Set a socket fd non-blocking.
int set_nb(int fd) {
    int f = ::fcntl(fd, F_GETFL, 0);
    return f >= 0 ? ::fcntl(fd, F_SETFL, f | O_NONBLOCK) : -1;
}

}  // namespace

TEST_CASE("Task: basic coroutine returns a value") {
    using namespace muses;

    Task<int> t = []() -> Task<int> {
        co_return 42;
    }();

    // initial_suspend is suspend_always, so it hasn't run yet.
    CHECK_FALSE(t.done());
    // resume() runs it to completion (no I/O await inside).
    bool finished = t.resume();
    CHECK(finished);
    CHECK(t.done());
    CHECK(t.handle().promise().value() == 42);
}

// A coroutine factory kept out of the TEST_CASE scope so the coroutine frame's
// captured reference outlives the frame. (A temporary `[&](){...}()` lambda is
// destroyed before the suspended coroutine is resumed, dangling the capture.)
static muses::Task<void> make_void_task(bool& flag) {
    flag = true;
    co_return;
}

TEST_CASE("Task<void>: void coroutine runs to completion") {
    using namespace muses;

    bool side_effect = false;
    Task<void> t = make_void_task(side_effect);

    CHECK_FALSE(side_effect);
    t.resume();
    CHECK(side_effect);
    CHECK(t.done());
}

TEST_CASE("Task: move transfers ownership, source is inert") {
    using namespace muses;

    Task<int> a = []() -> Task<int> { co_return 7; }();
    a.resume();
    Task<int> b = std::move(a);
    CHECK_FALSE(a.valid());      // moved-from is inert
    CHECK(b.valid());
    CHECK(b.done());
    CHECK(b.handle().promise().value() == 7);
}

// Coroutine factory (kept out of TEST_CASE scope so the frame's captures
// outlive the frame). Reads from `read_fd` after awaiting Readable.
static muses::Task<std::string> make_reader(muses::Poller* poller, int read_fd) {
    muses::IoAwaiter aw{poller, read_fd, muses::EventMask::Readable};
    co_await aw;
    char buf[64];
    ssize_t r = ::read(read_fd, buf, sizeof(buf));
    std::string out;
    if (r > 0) out.assign(buf, static_cast<std::size_t>(r));
    co_return out;
}

// End-to-end: an echo coroutine on a socketpair, driven by a minimal event
// loop using the real Poller. Verifies IoAwaiter registers interest, the loop
// resumes the coroutine on readiness, and data round-trips.
TEST_CASE("Task: IoAwaiter echo over socketpair + poller event loop") {
    using namespace muses;

    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nb(sv[0]);
    set_nb(sv[1]);

    auto poller = muses::make_poller();
    REQUIRE(poller != nullptr);

    Task<std::string> echo_task = make_reader(poller.get(), sv[0]);

    // The coroutine is suspended at initial_suspend. Resume it once: it will
    // hit the co_await and register Readable interest on sv[0], then suspend.
    CHECK_FALSE(echo_task.resume());
    CHECK_FALSE(echo_task.done());  // suspended on the awaiter

    // Now write to sv[1] so sv[0] becomes readable.
    const char* msg = "hello-coroutine";
    REQUIRE(::write(sv[1], msg, std::strlen(msg)) == static_cast<ssize_t>(std::strlen(msg)));

    // Run a minimal event loop: wait for the registered event, extract the
    // handle from userdata, resume it.
    PollEvent evs[4];
    bool got_event = false;
    for (int spin = 0; spin < 50 && !got_event; ++spin) {
        int n = poller->wait(evs, 100);  // 100ms timeout
        for (int i = 0; i < n; ++i) {
            auto h = muses::handle_from_event(evs[i]);
            if (h) {
                // This is our suspended coroutine. Resume it.
                h.resume();
                got_event = true;
            }
        }
    }
    CHECK(got_event);
    CHECK(echo_task.done());
    CHECK(echo_task.handle().promise().value() == "hello-coroutine");

    ::close(sv[0]);
    ::close(sv[1]);
}
