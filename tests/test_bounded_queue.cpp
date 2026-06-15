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

// High-contention true-MPMC stress (many producers AND many consumers) on the
// lock-free ring. Verifies no item is double-consumed under the Vyukov ring,
// which is the path the logger and reactor outbox use in production. Uses
// Block policy so every produced item must be delivered (no drops), making the
// accounting exact.
TEST_CASE("BoundedQueue: MPMC stress, no duplication") {
    // Block policy → producers block on full, so all items are delivered.
    // Consumers exit once all producers have finished AND the queue is drained.
    muses::BoundedQueue<int> q(512, muses::OverflowPolicy::Block);
    constexpr int N_PRODUCERS = 8;
    constexpr int N_CONSUMERS = 8;
    constexpr int PER = 2000;
    constexpr int TOTAL = N_PRODUCERS * PER;

    std::vector<std::thread> producers;
    std::atomic<int> producers_done{0};
    // Each producer p emits unique values [p*PER, (p+1)*PER) so we can assert
    // exact delivery with no overlaps.
    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    auto any_left = [&] {
        // Done when all producers finished and the ring is empty.
        return producers_done.load(std::memory_order_acquire) < N_PRODUCERS ||
               q.size() > 0;
    };

    std::vector<std::thread> consumers;
    for (int i = 0; i < N_CONSUMERS; ++i) {
        consumers.emplace_back([&] {
            int v;
            while (any_left()) {
                if (q.wait_pop(v, std::chrono::milliseconds(2))) {
                    seen[static_cast<std::size_t>(v)].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            // Final drain after the gate flipped.
            int w;
            while (q.try_pop(w)) {
                seen[static_cast<std::size_t>(w)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }
    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&q, p, PER, &producers_done] {
            for (int k = 0; k < PER; ++k) {
                q.push(p * PER + k);
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    // Every value delivered exactly once (no loss, no duplication).
    int missing = 0, duplicated = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(TOTAL); ++i) {
        int c = seen[i].load(std::memory_order_relaxed);
        if (c == 0) ++missing;
        else if (c > 1) ++duplicated;
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
}

// Lock-free DropNew/DropOldest path: under heavy contention some items are
// dropped (DropNew) or evicted (DropOldest), but a delivered item must never
// be delivered twice. This exercises the Vyukov ring's CAS loops.
TEST_CASE("BoundedQueue: DropNew high-contention, no duplication") {
    muses::BoundedQueue<int> q(256, muses::OverflowPolicy::DropNew);
    constexpr int N_PRODUCERS = 8;
    constexpr int PER = 3000;

    std::vector<std::thread> producers;
    std::atomic<int> producers_done{0};
    std::vector<std::atomic<int>> seen(N_PRODUCERS * PER);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    std::vector<std::thread> consumers;
    std::atomic<bool> stop_flag{false};
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            int v;
            while (!stop_flag.load(std::memory_order_acquire)) {
                if (q.try_pop(v)) {
                    seen[static_cast<std::size_t>(v)].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            int w;
            while (q.try_pop(w)) {
                seen[static_cast<std::size_t>(w)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }
    for (int p = 0; p < N_PRODUCERS; ++p) {
        producers.emplace_back([&q, p, PER, &producers_done] {
            for (int k = 0; k < PER; ++k) {
                q.push(p * PER + k);
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (auto& t : producers) t.join();
    // Wait for the queue to drain before stopping consumers.
    while (q.size() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop_flag.store(true, std::memory_order_release);
    for (auto& t : consumers) t.join();

    // No delivered value was delivered more than once.
    int duplicated = 0;
    for (auto& s : seen) {
        if (s.load(std::memory_order_relaxed) > 1) ++duplicated;
    }
    CHECK(duplicated == 0);
}
