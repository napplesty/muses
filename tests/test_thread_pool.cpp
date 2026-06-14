#include <doctest.h>

#include "muses/thread_pool.hpp"

#include <atomic>
#include <future>
#include <vector>
#include <stdexcept>

TEST_CASE("ThreadPool: tasks run and futures resolve") {
    muses::ThreadPool pool(4);
    std::vector<std::future<int>> results;
    for (int i = 0; i < 100; ++i) {
        results.emplace_back(pool.enqueue([](int x) { return x * 2; }, i));
    }
    for (int i = 0; i < 100; ++i) {
        CHECK(results[i].get() == i * 2);
    }
}

TEST_CASE("ThreadPool: concurrent counter under stress") {
    muses::ThreadPool pool(8);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> tasks;
    for (int i = 0; i < 1000; ++i) {
        tasks.emplace_back(pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
    }
    for (auto& t : tasks) {
        t.get();
    }
    CHECK(counter.load() == 1000);
}

TEST_CASE("ThreadPool: wait_empty drains the queue") {
    muses::ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 500; ++i) {
        pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_empty();
    CHECK(counter.load() == 500);
}

TEST_CASE("ThreadPool: enqueue after stop returns invalid future") {
    muses::ThreadPool pool(2);
    pool.stop_pool();
    auto fut = pool.enqueue([] { return 42; });
    CHECK_FALSE(fut.valid());
}

TEST_CASE("ThreadPool: exception in task does not kill the worker") {
    muses::ThreadPool pool(2);
    auto bad = pool.enqueue([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(bad.get(), std::runtime_error);
    // Pool must still be usable.
    auto good = pool.enqueue([] { return 7; });
    CHECK(good.get() == 7);
}
