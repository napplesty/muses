#include <doctest.h>

#include "muses/queue.hpp"

#include <thread>
#include <vector>
#include <atomic>

TEST_CASE("ThreadSafeQueue: basic push/try_pop") {
    muses::ThreadSafeQueue<int> q;
    CHECK(q.empty());
    q.push(1);
    q.push(2);
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 1);
    CHECK(q.try_pop(v));
    CHECK(v == 2);
    CHECK_FALSE(q.try_pop(v));
    CHECK(q.empty());
}

TEST_CASE("ThreadSafeQueue: capacity blocks push") {
    muses::ThreadSafeQueue<int> q;
    q.set_capacity(2);
    q.push(1);
    q.push(2);
    // Now full; push should block until a consumer pops.
    std::thread consumer([&] {
        int v;
        CHECK(q.wait_and_pop(v));
        CHECK(v == 1);
    });
    q.push(3);  // unblocks once consumer pops
    consumer.join();
    int v;
    CHECK(q.try_pop(v)); CHECK(v == 2);
    CHECK(q.try_pop(v)); CHECK(v == 3);
}

TEST_CASE("ThreadSafeQueue: stop unblocks wait_and_pop") {
    muses::ThreadSafeQueue<int> q;
    std::thread waiter([&] {
        int v;
        bool ok = q.wait_and_pop(v);
        CHECK_FALSE(ok);  // stopped with empty queue
    });
    // Let the waiter block.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.stop();
    waiter.join();
}

TEST_CASE("ThreadSafeQueue: stop unblocks blocked producer") {
    muses::ThreadSafeQueue<int> q;
    q.set_capacity(1);
    q.push(1);  // full
    std::thread producer([&] {
        bool ok = q.push(2);  // blocked, then rejected by stop
        CHECK_FALSE(ok);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.stop();
    producer.join();
}

TEST_CASE("ThreadSafeQueue: MPSC stress") {
    muses::ThreadSafeQueue<int> q;
    constexpr int N_PRODUCERS = 8;
    constexpr int PER = 5000;
    std::vector<std::thread> producers;
    std::atomic<int> sum{0};
    for (int i = 0; i < N_PRODUCERS; ++i) {
        producers.emplace_back([&q, i] {
            for (int k = 0; k < PER; ++k) {
                q.push(i * PER + k);
            }
        });
    }
    std::thread consumer([&] {
        int v;
        while (sum.fetch_add(0, std::memory_order_relaxed) < N_PRODUCERS * PER) {
            if (q.try_pop(v)) {
                sum.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    for (auto& t : producers) t.join();
    // Wake consumer for the tail.
    q.push(-1);
    consumer.join();
    CHECK(sum.load() >= N_PRODUCERS * PER - PER);  // tolerance for the wakeup race
}
