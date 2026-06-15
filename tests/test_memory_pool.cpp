#include <doctest.h>

#include "muses/memory_pool.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
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

// A block allocated on thread A can be released on thread B. This is the key
// contract for any future thread-local cache layer: since the size class lives
// in the BlockHeader (not in thread state), release() always routes the block
// back to the correct global bin regardless of which thread frees it.
TEST_CASE("MemoryPool: cross-thread allocate/release") {
    muses::MemoryPool pool;
    constexpr int N_THREADS = 8;
    constexpr int PER = 2000;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    // Each thread allocates PER blocks and hands them to a shared list; a
    // single reaper thread releases them all (cross-thread frees).
    std::mutex mu;
    std::vector<void*> pending;
    std::atomic<int> allocated{0};
    std::atomic<bool> done{false};

    std::thread reaper([&] {
        for (;;) {
            std::vector<void*> grab;
            {
                std::lock_guard<std::mutex> lk(mu);
                grab.swap(pending);
            }
            for (void* p : grab) pool.release(p);
            // Exit only once all allocators are done AND the shared list is
            // drained. Re-check pending under the lock to avoid racing with a
            // producer that pushes between our swap and the empty() read.
            bool can_stop;
            {
                std::lock_guard<std::mutex> lk(mu);
                can_stop = done.load(std::memory_order_acquire) && pending.empty();
            }
            if (can_stop) break;
            std::this_thread::yield();
        }
    });

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER; ++i) {
                void* p = pool.allocate(1 + (i % 200));
                if (p == nullptr) { errors.fetch_add(1); continue; }
                std::memset(p, 0xAB, 1 + (i % 200));  // touch to catch overlap
                allocated.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(mu);
                    pending.push_back(p);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    done.store(true, std::memory_order_release);
    reaper.join();
    // Defensive: after the reaper is joined, drain anything that slipped
    // through (shouldn't happen, but makes the live==0 assertion robust).
    std::vector<void*> leftover;
    {
        std::lock_guard<std::mutex> lk(mu);
        leftover.swap(pending);
    }
    for (void* p : leftover) pool.release(p);

    CHECK(errors.load() == 0);
    CHECK(allocated.load() == N_THREADS * PER);
    CHECK(pool.stats().live == 0);  // all released back
}

// PooledBuffer: grow, reuse, and the keep-alive reset pattern.
TEST_CASE("PooledBuffer: append, grow, reset_for_reuse keeps capacity") {
    muses::MemoryPool pool;
    muses::PooledBuffer buf(pool, 64);
    CHECK(buf.capacity() >= 64);
    CHECK(buf.empty());

    // Fill exactly to capacity (no grow).
    std::string chunk(64, 'A');
    CHECK(buf.append(chunk.data(), chunk.size()));
    CHECK(buf.size() == 64);
    CHECK(buf.capacity() == 64);

    // Append beyond capacity → grow.
    CHECK(buf.append("HELLO", 5));
    CHECK(buf.size() == 69);
    CHECK(buf.capacity() >= 69);
    // Contents preserved across the grow.
    const char* d = static_cast<const char*>(buf.data());
    CHECK(d[0] == 'A');
    CHECK(d[63] == 'A');
    CHECK(d[64] == 'H');

    // Reset for the next request on the same connection: length and scan_pos
    // clear, but capacity is retained (block reuse).
    buf.reset_for_reuse();
    CHECK(buf.size() == 0);
    CHECK(buf.scan_pos() == 0);
    CHECK(buf.capacity() >= 69);  // block retained

    // Re-append works after reset.
    CHECK(buf.append("world", 5));
    CHECK(buf.size() == 5);
    CHECK(static_cast<const char*>(buf.data())[0] == 'w');
}

// PooledBuffer: scan_pos bookkeeping (caller-managed delimiter cursor).
TEST_CASE("PooledBuffer: scan_pos get/set") {
    muses::MemoryPool pool;
    muses::PooledBuffer buf(pool, 128);
    CHECK(buf.scan_pos() == 0);
    buf.set_scan_pos(42);
    CHECK(buf.scan_pos() == 42);
    buf.reset_for_reuse();
    CHECK(buf.scan_pos() == 0);
}

// PooledBuffer: move transfers ownership; source is empty.
TEST_CASE("PooledBuffer: move semantics") {
    muses::MemoryPool pool;
    muses::PooledBuffer a(pool, 128);
    a.append("data", 4);
    std::size_t cap_before = a.capacity();
    muses::PooledBuffer b(std::move(a));
    CHECK(a.data() == nullptr);
    CHECK(b.size() == 4);
    CHECK(b.capacity() == cap_before);
    CHECK(static_cast<const char*>(b.data())[0] == 'd');
    // b's destructor releases the block back to the pool.
}
