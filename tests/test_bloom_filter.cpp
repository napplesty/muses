#include <doctest.h>

#include "muses/bloom_filter.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("CountingBloomFilter: add then contains") {
    muses::CountingBloomFilter<std::string> bf(1000);
    bf.add("apple");
    bf.add("banana");
    CHECK(bf.contains("apple"));
    CHECK(bf.contains("banana"));
    // A never-added element is definitively absent (no false negative possible).
    CHECK_FALSE(bf.contains("cherry-zzz-not-added-999"));
}

TEST_CASE("CountingBloomFilter: remove restores absence") {
    muses::CountingBloomFilter<std::string> bf(1000);
    bf.add("item");
    CHECK(bf.contains("item"));
    CHECK(bf.remove("item"));
    CHECK_FALSE(bf.contains("item"));
}

TEST_CASE("CountingBloomFilter: counting semantics — repeated add needs repeated remove") {
    // A standard (1-bit) Bloom can't distinguish "added once" from "added 3x".
    // A counting Bloom must: add 3x → needs 3 removes to fully clear.
    muses::CountingBloomFilter<int> bf(2000);
    for (int i = 0; i < 3; ++i) bf.add(42);
    CHECK(bf.contains(42));
    CHECK(bf.remove(42));
    CHECK(bf.contains(42));  // still 2 left
    CHECK(bf.remove(42));
    CHECK(bf.contains(42));  // still 1 left
    CHECK(bf.remove(42));
    CHECK_FALSE(bf.contains(42));  // now fully removed
}

TEST_CASE("CountingBloomFilter: remove on absent element is a safe no-op") {
    muses::CountingBloomFilter<std::string> bf(1000);
    // Never added — remove must not crash and must not underflow counters.
    CHECK_FALSE(bf.remove("ghost"));
    CHECK_FALSE(bf.contains("ghost"));
    // Adding a different element afterwards still works (no corruption).
    bf.add("real");
    CHECK(bf.contains("real"));
}

TEST_CASE("CountingBloomFilter: clear empties everything") {
    muses::CountingBloomFilter<int> bf(500);
    for (int i = 0; i < 50; ++i) bf.add(i);
    for (int i = 0; i < 50; ++i) CHECK(bf.contains(i));
    bf.clear();
    for (int i = 0; i < 50; ++i) CHECK_FALSE(bf.contains(i));
}

TEST_CASE("CountingBloomFilter: decay expires a singly-added element") {
    muses::CountingBloomFilter<std::string> bf(1000);
    bf.add("ephemeral");
    CHECK(bf.contains("ephemeral"));
    // A counter value of 1 right-shifted by >=1 becomes 0 → expired.
    bf.decay(1);
    CHECK_FALSE(bf.contains("ephemeral"));
}

TEST_CASE("CountingBloomFilter: decay halves, repeated decay eventually clears") {
    muses::CountingBloomFilter<int> bf(2000);
    // Add enough times that a single decay(1) can't zero it. Counter saturates
    // at 15, so it survives several halvings.
    for (int i = 0; i < 20; ++i) bf.add(7);
    CHECK(bf.contains(7));
    // 15 -> 7 -> 3 -> 1 -> 0 over 4 decays of shift 1.
    bf.decay(1); CHECK(bf.contains(7));
    bf.decay(1); CHECK(bf.contains(7));
    bf.decay(1); CHECK(bf.contains(7));
    bf.decay(1);
    CHECK_FALSE(bf.contains(7));  // counter reached 0
}

TEST_CASE("CountingBloomFilter: saturated counters stay through a single remove") {
    // Adding 100x saturates every touched counter at 15; one remove drops to 14,
    // so the element must still be reported present.
    muses::CountingBloomFilter<std::string> bf(1000);
    for (int i = 0; i < 100; ++i) bf.add("spammer");
    CHECK(bf.contains("spammer"));
    CHECK(bf.remove("spammer"));
    CHECK(bf.contains("spammer"));  // saturated, still present
}

TEST_CASE("CountingBloomFilter: capacity and hash count are reasonable") {
    muses::CountingBloomFilter<int> bf(1000, 0.01);
    // For n=1000, p=0.01: m ≈ 9586 bits, k ≈ 7. Allow generous bounds since the
    // formulas round and we force m even.
    CHECK(bf.capacity() >= 9000);
    CHECK(bf.capacity() <= 12000);
    CHECK(bf.hash_count() >= 5);
    CHECK(bf.hash_count() <= 12);
    // Memory: m/2 bytes.
    CHECK(bf.capacity() % 2 == 0);
}

TEST_CASE("CountingBloomFilter: false-positive rate is within target") {
    // Insert N elements, probe M distinct absent elements, measure FP rate.
    // With p=0.01 we expect roughly <= 3% empirically (statistical slack).
    muses::CountingBloomFilter<int> bf(2000, 0.01);
    for (int i = 0; i < 2000; ++i) bf.add(i);
    int fps = 0;
    constexpr int PROBES = 2000;
    for (int i = 0; i < PROBES; ++i) {
        // Probe numbers well outside the inserted range (definitely absent).
        if (bf.contains(1'000'000 + i)) ++fps;
    }
    double rate = static_cast<double>(fps) / PROBES;
    CHECK(rate < 0.05);  // generous upper bound over the 0.01 target
}

TEST_CASE("CountingBloomFilter: approximate_count tracks inserts") {
    muses::CountingBloomFilter<int> bf(2000, 0.01);
    CHECK(bf.approximate_count() == 0.0);
    for (int i = 0; i < 500; ++i) bf.add(i);
    double est = bf.approximate_count();
    // Estimate should be in the right ballpark (within 2x; it's a rough metric).
    CHECK(est > 200.0);
    CHECK(est < 1500.0);
}

TEST_CASE("CountingBloomFilter: concurrent add/remove/contains/decay is safe") {
    muses::CountingBloomFilter<int> bf(5000);
    constexpr int N_THREADS = 8;
    constexpr int OPS = 2000;
    std::atomic<int> errors{0};

    auto worker = [&](int seed) {
        for (int i = 0; i < OPS; ++i) {
            int v = seed * 1000 + (i % 500);
            switch (i % 4) {
                case 0: bf.add(v); break;
                case 1: bf.remove(v); break;
                case 2: (void)bf.contains(v); break;
                case 3:
                    if (i % 100 == 0) bf.decay(1);
                    break;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
    // Reaching here without crashing/UBSan errors under -fsanitize is the pass
    // condition for the threading contract.
    CHECK(errors.load() == 0);
}
