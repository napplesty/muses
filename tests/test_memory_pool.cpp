#include <doctest.h>

#include "muses/memory_pool.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("MemoryPool: allocate/release round-trip") {
    muses::MemoryPool pool;
    void* p = pool.allocate(100);
    REQUIRE(p != nullptr);
    auto s = pool.stats();
    CHECK(s.live == 1);
    std::memset(p, 0xAB, 100);  // writable
    pool.release(p);
    s = pool.stats();
    CHECK(s.live == 0);
    CHECK(s.recycled >= 1);
}

TEST_CASE("MemoryPool: same size-class recycles the slot") {
    muses::MemoryPool pool(1 << 16, {64});
    void* a = pool.allocate(50);
    pool.release(a);
    void* b = pool.allocate(50);
    CHECK(b == a);  // popped from the free-list
    pool.release(b);
}

TEST_CASE("MemoryPool: retain/release keeps allocation live") {
    muses::MemoryPool pool;
    void* a = pool.allocate(32);
    pool.retain(a);   // refcount 2
    pool.release(a);  // refcount 1, still live
    CHECK(pool.stats().live == 1);
    pool.release(a);  // refcount 0, recycled
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: oversized allocations escape") {
    muses::MemoryPool pool(1 << 16, {64, 128, 256});
    void* big = pool.allocate(10'000);  // > max class → escape
    REQUIRE(big != nullptr);
    CHECK(pool.stats().escaped == 1);
    std::memset(big, 1, 10'000);
    pool.release(big);
    CHECK(pool.stats().escaped == 0);
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: arena exhaustion escapes gracefully") {
    // Tiny arena, large class so carve fails quickly.
    muses::MemoryPool pool(256, {256});
    std::vector<void*> ptrs;
    for (int i = 0; i < 50; ++i) {
        ptrs.push_back(pool.allocate(200));
        REQUIRE(ptrs.back() != nullptr);
    }
    // Mix of pooled and escaped; all must release without corruption.
    for (void* p : ptrs) pool.release(p);
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: PoolPtr RAII") {
    muses::MemoryPool pool;
    {
        auto p = muses::make_pool<std::string>(pool, "hello");
        CHECK(*p == "hello");
        CHECK(pool.stats().live == 1);
        // copy retains
        muses::PoolPtr<std::string> q = p;
        CHECK(pool.stats().live == 1);
        // move transfers
        muses::PoolPtr<std::string> r = std::move(p);
        CHECK(pool.stats().live == 1);
        CHECK(p.get() == nullptr);
        CHECK(r.get() != nullptr);
    }
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: PoolPtr vector releases all") {
    muses::MemoryPool pool;
    {
        std::vector<muses::PoolPtr<std::string>> v;
        for (int i = 0; i < 100; ++i) {
            v.push_back(muses::make_pool<std::string>(pool, std::string(20, 'x')));
        }
        CHECK(pool.stats().live == 100);
    }
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: multithreaded stress, live returns to 0") {
    muses::MemoryPool pool(1 << 18, {32, 64, 128, 256});
    constexpr int NTHREADS = 16;
    constexpr int ITERS = 2000;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    for (int t = 0; t < NTHREADS; ++t) {
        threads.emplace_back([&pool, &errors]() {
            std::vector<void*> local;
            local.reserve(64);
            for (int i = 0; i < ITERS; ++i) {
                void* p = pool.allocate(1 + (i % 200));  // varied sizes
                if (p == nullptr) { errors++; continue; }
                std::memset(p, (i & 0xFF), 1);
                local.push_back(p);
                if (local.size() == 32) {
                    for (void* q : local) pool.release(q);
                    local.clear();
                }
            }
            for (void* q : local) pool.release(q);
        });
    }
    for (auto& th : threads) th.join();
    CHECK(errors.load() == 0);
    CHECK(pool.stats().live == 0);
}

TEST_CASE("MemoryPool: no cross-talk between allocations") {
    // Two concurrent allocations must not overlap.
    muses::MemoryPool pool;
    void* a = pool.allocate(1000);
    void* b = pool.allocate(1000);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    char* ca = static_cast<char*>(a);
    char* cb = static_cast<char*>(b);
    bool no_overlap = (cb >= ca + 1000) || (ca >= cb + 1000);
    CHECK(no_overlap);
    std::memset(a, 1, 1000);
    std::memset(b, 2, 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(static_cast<unsigned char*>(a)[i] == 1);
    }
    pool.release(a);
    pool.release(b);
}
