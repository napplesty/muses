// MIT License

// Copyright (c) 2023 nastyapple

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#ifndef MUSES_BLOOM_FILTER_HPP
#define MUSES_BLOOM_FILTER_HPP

namespace muses {

// A counting Bloom filter with 4-bit-per-slot counters, supporting removal and
// probabilistic expiry via global decay.
//
// Why counting (not standard): a standard Bloom filter can only add and query —
// flipping a shared bit on remove would un-corrupt other elements that map to
// the same bit. Counting Bloom filters keep a small counter per slot so an
// element can be removed without affecting others (up to counter saturation).
//
// Why decay-based expiry (not per-entry TTL): storing a deadline per logical
// element would require a hashmap keyed by the element, defeating the whole
// point of a Bloom filter (constant, element-independent memory). Instead the
// caller drives time: every T seconds call decay(1) and every counter is right-
// shifted by one bit (halved). An element that stops being re-added sees its
// counters decay to zero within ~T*log2(initial count) and "expires". This
// matches the semantics of a rate limiter / IP blacklist ("was this IP seen
// recently") far better than precise per-entry timers.
//
// Saturated counters: each 4-bit counter caps at 15. Once saturated it won't
// grow further, so a single remove won't drop it back to zero — the element
// stays "present" until decay shrinks the counter. This is the standard
// counting-Bloom trade-off: bounded counters vs. perfect removal. For a
// blacklist/rate-limiter this is the desired behavior (a repeatedly-offending
// IP should stay banned across a few decays).
//
// Thread safety: every public method takes an internal mutex, so the filter can
// be shared across worker threads without external locking (same contract as
// LruCache). Operations are O(k) constant time; lock contention is negligible
// at reasonable concurrency.
template <typename T>
class CountingBloomFilter {
public:
    // Construct for an expected element count at a target false-positive rate.
    // The slot count m and hash count k are derived from the standard formulas:
    //   m = ceil(-n * ln(p) / (ln2)^2)
    //   k = round(m / n * ln2)
    // m is rounded up to an even number so it packs cleanly into bytes
    // (two 4-bit counters per byte).
    CountingBloomFilter(std::size_t expected_items, double fp_rate = 0.01)
    : expected_items_(expected_items == 0 ? 1 : expected_items) {
        if (fp_rate <= 0.0) fp_rate = 1e-9;
        if (fp_rate >= 1.0) fp_rate = 0.9999;
        const double ln2 = std::log(2.0);
        double m_d = std::ceil(-static_cast<double>(expected_items_) *
                               std::log(fp_rate) / (ln2 * ln2));
        std::size_t m = static_cast<std::size_t>(m_d);
        if (m < 2) m = 2;
        if (m % 2 != 0) ++m;  // even → whole-byte packing
        num_slots_ = m;
        double k_d = std::round(static_cast<double>(m) /
                                static_cast<double>(expected_items_) * ln2);
        num_hashes_ = k_d < 1 ? 1 : static_cast<std::size_t>(k_d);
        // One byte holds two 4-bit counters; num_slots_ is even so bytes_ is exact.
        bytes_.assign(num_slots_ / 2, 0);
    }

    CountingBloomFilter(const CountingBloomFilter&) = delete;
    CountingBloomFilter& operator=(const CountingBloomFilter&) = delete;

    // Insert: bump the k mapped counters by one (saturating at kCounterMax).
    void add(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [h1, h2] = hashes(item);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            std::size_t idx = slot_of(h1, h2, i);
            set_counter(idx, saturate_inc(get_counter(idx)));
        }
    }

    // Query: true only if ALL k mapped counters are non-zero. A true result is
    // probabilistic (may be a false positive); a false result is definitive.
    bool contains(const T& item) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [h1, h2] = hashes(item);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            if (get_counter(slot_of(h1, h2, i)) == 0) return false;
        }
        return true;
    }

    // Remove: decrement the k mapped counters by one (floored at 0). Returns
    // true if at least one counter was decremented. Safe to call on an absent
    // element — it simply touches counters that are already zero (no-op).
    bool remove(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [h1, h2] = hashes(item);
        bool changed = false;
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            std::size_t idx = slot_of(h1, h2, i);
            std::uint8_t c = get_counter(idx);
            if (c > 0) {
                set_counter(idx, c - 1);
                changed = true;
            }
        }
        return changed;
    }

    // Probabilistic expiry: right-shift every counter by `shift` bits. Call this
    // periodically (e.g. every T seconds from a timer) so elements that stop
    // being re-added fade out within a bounded number of periods. shift=1 halves
    // every counter (a one-bit decay); larger shifts age faster.
    void decay(unsigned shift = 1) {
        if (shift == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (shift >= 4) {
            // Any shift >= 4 clears all 4-bit counters.
            std::fill(bytes_.begin(), bytes_.end(), 0);
            return;
        }
        for (std::uint8_t& packed : bytes_) {
            std::uint8_t hi = (packed >> 4) & 0x0F;
            std::uint8_t lo = packed & 0x0F;
            hi >>= shift;
            lo >>= shift;
            packed = static_cast<std::uint8_t>((hi << 4) | lo);
        }
    }

    // Zero every counter. After this, contains() is false for everything.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::fill(bytes_.begin(), bytes_.end(), 0);
    }

    // The number of 4-bit slots (m). Determines memory use: m/2 bytes.
    std::size_t capacity() const { return num_slots_; }

    // The number of hash functions (k).
    std::size_t hash_count() const { return num_hashes_; }

    // Approximate number of elements currently in the filter, estimated from
    // the fraction of zero counters (Swamidass & Baldi's formula). Useful for
    // monitoring, not for correctness-critical decisions.
    double approximate_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t zeros = 0;
        for (std::uint8_t packed : bytes_) {
            if (((packed >> 4) & 0x0F) == 0) ++zeros;
            if ((packed & 0x0F) == 0) ++zeros;
        }
        double m = static_cast<double>(num_slots_);
        if (zeros == num_slots_) return 0.0;
        double x = static_cast<double>(zeros) / m;
        // n* = -(m / k) * ln(X)
        return -(m / static_cast<double>(num_hashes_)) * std::log(x);
    }

private:
    static constexpr std::uint8_t kCounterMax = 15;  // 4-bit saturation ceiling

    // Two base hashes via std::hash + a mixing constant; the k index hashes are
    // derived by double-hashing (h1 + i*h2). This avoids needing k independent
    // hash functions while keeping good distribution for Bloom-filter purposes.
    std::pair<std::uint64_t, std::uint64_t> hashes(const T& item) const {
        std::uint64_t h1 = std::hash<T>{}(item);
        // Second hash: mix the first with a golden-ratio-derived constant to
        // decorrelate it. 0x9E3779B97F4A7C15 is the 64-bit golden ratio.
        std::uint64_t h2 = (h1 * 0x9E3779B97F4A7C15ULL) ^ (h1 >> 32);
        if (h2 == 0) h2 = 0x517cc1b727220a95ULL;  // avoid zero stride
        return {h1, h2};
    }

    // The i-th probed slot index, reduced into [0, num_slots_).
    std::size_t slot_of(std::uint64_t h1, std::uint64_t h2, std::size_t i) const {
        return static_cast<std::size_t>((h1 + i * h2) % num_slots_);
    }

    // 4-bit packed counter accessors. Slot s lives in byte s/2; even slots in
    // the low nibble, odd slots in the high nibble.
    std::uint8_t get_counter(std::size_t slot) const {
        std::size_t byte = slot / 2;
        if (slot % 2 == 0) {
            return bytes_[byte] & 0x0F;
        }
        return (bytes_[byte] >> 4) & 0x0F;
    }

    void set_counter(std::size_t slot, std::uint8_t val) {
        std::size_t byte = slot / 2;
        if (slot % 2 == 0) {
            bytes_[byte] = (bytes_[byte] & 0xF0) | (val & 0x0F);
        } else {
            bytes_[byte] = (bytes_[byte] & 0x0F) | static_cast<std::uint8_t>((val & 0x0F) << 4);
        }
    }

    static std::uint8_t saturate_inc(std::uint8_t c) {
        return c >= kCounterMax ? kCounterMax : static_cast<std::uint8_t>(c + 1);
    }

    mutable std::mutex mutex_;
    std::vector<std::uint8_t> bytes_;  // two 4-bit counters per byte
    std::size_t num_slots_;           // m
    std::size_t num_hashes_;          // k
    std::size_t expected_items_;      // n (bookkeeping)
};

}  // namespace muses

#endif  // MUSES_BLOOM_FILTER_HPP
