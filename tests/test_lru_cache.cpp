#include <doctest.h>

#include "muses/lru_cache.hpp"

#include <string>
#include <thread>
#include <vector>

TEST_CASE("LruCache: basic put/get") {
    muses::LruCache<std::string, int> cache(4);
    CHECK_FALSE(cache.get("a").has_value());
    cache.put("a", 1);
    cache.put("b", 2);
    CHECK(cache.get("a").value() == 1);
    CHECK(cache.get("b").value() == 2);
    CHECK(cache.size() == 2);
}

TEST_CASE("LruCache: evicts LRU on overflow") {
    muses::LruCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    cache.put(4, 40);  // evicts 1 (oldest, never accessed)
    CHECK_FALSE(cache.get(1).has_value());
    CHECK(cache.get(2).value() == 20);
    CHECK(cache.get(3).value() == 30);
    CHECK(cache.get(4).value() == 40);
    CHECK(cache.size() == 3);
}

TEST_CASE("LruCache: get promotes to MRU, affecting eviction order") {
    muses::LruCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    // Access 1 → it becomes MRU, so eviction order is now 2 (LRU), 3, 1.
    CHECK(cache.get(1).value() == 10);
    cache.put(4, 40);  // evicts 2
    CHECK_FALSE(cache.get(2).has_value());
    CHECK(cache.get(1).value() == 10);  // still present
    CHECK(cache.get(3).value() == 30);
    CHECK(cache.get(4).value() == 40);
}

TEST_CASE("LruCache: put on existing key updates value and promotes") {
    muses::LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(1, 999);  // update existing, 1 promoted to MRU
    CHECK(cache.get(1).value() == 999);
    cache.put(3, 30);   // evicts 2 (now the LRU), not 1
    CHECK_FALSE(cache.get(2).has_value());
    CHECK(cache.get(1).value() == 999);
    CHECK(cache.get(3).value() == 30);
}

TEST_CASE("LruCache: contains peeks without promoting") {
    muses::LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK(cache.contains(1));
    CHECK_FALSE(cache.contains(99));
    // contains(1) did NOT promote 1, so 1 is still LRU.
    cache.put(3, 30);   // evicts 1
    CHECK_FALSE(cache.contains(1));
    CHECK(cache.contains(2));
    CHECK(cache.contains(3));
}

TEST_CASE("LruCache: erase and clear") {
    muses::LruCache<int, int> cache(4);
    cache.put(1, 10);
    cache.put(2, 20);
    CHECK(cache.erase(1));
    CHECK_FALSE(cache.contains(1));
    CHECK_FALSE(cache.erase(1));  // already gone
    CHECK(cache.size() == 1);
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.get(2).has_value());
}

TEST_CASE("LruCache: capacity 0 caches nothing") {
    muses::LruCache<int, int> cache(0);
    cache.put(1, 10);
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.get(1).has_value());
    CHECK_FALSE(cache.contains(1));
}

TEST_CASE("LruCache: multithreaded stress, no corruption") {
    muses::LruCache<int, int> cache(64);
    constexpr int NTHREADS = 8;
    constexpr int ITERS = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < NTHREADS; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < ITERS; ++i) {
                int key = (t * 7 + i) % 128;
                cache.put(key, i);
                cache.get(key);
                if (i % 17 == 0) cache.erase(key);
            }
        });
    }
    for (auto& th : threads) th.join();
    // After stress: size must respect the capacity invariant.
    CHECK(cache.size() <= 64);
    // Every contained key must be gettable.
    for (int k = 0; k < 128; ++k) {
        if (cache.contains(k)) {
            CHECK(cache.get(k).has_value());
        }
    }
}
