#include <doctest.h>

#include "muses/bounded_queue.hpp"

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("BoundedQueue: DropNew rejects when full") {
    muses::BoundedQueue<int> q(2, muses::OverflowPolicy::DropNew);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK_FALSE(q.push(3));  // full → dropped
    CHECK(q.dropped() == 1);
    int v;
    CHECK(q.try_pop(v)); CHECK(v == 1);
    CHECK(q.try_pop(v)); CHECK(v == 2);
}

TEST_CASE("BoundedQueue: DropOldest evicts the front") {
    muses::BoundedQueue<int> q(2, muses::OverflowPolicy::DropOldest);
    q.push(1);
    q.push(2);
    CHECK(q.push(3));          // evicts 1
    CHECK(q.dropped() == 1);
    int v;
    CHECK(q.try_pop(v)); CHECK(v == 2);  // 1 was dropped
    CHECK(q.try_pop(v)); CHECK(v == 3);
}

TEST_CASE("BoundedQueue: Block unblocks on consume") {
    muses::BoundedQueue<int> q(1, muses::OverflowPolicy::Block);
    CHECK(q.push(1));
    std::thread consumer([&] {
        int v;
        CHECK(q.wait_pop(v, std::chrono::seconds(2)));
        CHECK(v == 1);
    });
    CHECK(q.push(2));  // blocks until consumer pops 1
    consumer.join();
    int v;
    CHECK(q.try_pop(v)); CHECK(v == 2);
}

TEST_CASE("BoundedQueue: wait_pop timeout returns false") {
    muses::BoundedQueue<int> q(4);
    int v = -1;
    auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(q.wait_pop(v, std::chrono::milliseconds(60)));
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= std::chrono::milliseconds(50));
    CHECK(v == -1);
}

TEST_CASE("BoundedQueue: stop unblocks wait_pop") {
    muses::BoundedQueue<int> q(4);
    std::thread waiter([&] {
        int v;
        // indefinite wait
        CHECK_FALSE(q.wait_pop(v));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    q.stop();
    waiter.join();
}

TEST_CASE("BoundedQueue: MPSC stress, no lost wakeups") {
    muses::BoundedQueue<int> q(256, muses::OverflowPolicy::Block);
    constexpr int N_PRODUCERS = 8;
    constexpr int PER = 4000;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < N_PRODUCERS; ++i) {
        producers.emplace_back([&] {
            for (int k = 0; k < PER; ++k) {
                if (q.push(k)) produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::thread consumer([&] {
        int v;
        while (consumed.load() < N_PRODUCERS * PER) {
            if (q.wait_pop(v, std::chrono::milliseconds(5))) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    for (auto& t : producers) t.join();
    consumer.join();
    CHECK(produced.load() == N_PRODUCERS * PER);
    CHECK(consumed.load() == N_PRODUCERS * PER);
}
