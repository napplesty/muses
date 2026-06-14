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

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#ifndef MUSES_LRU_CACHE_HPP
#define MUSES_LRU_CACHE_HPP

namespace muses {

// A bounded LRU (least-recently-used) cache, thread-safe by default.
//
// All public methods acquire an internal mutex, so the cache can be shared
// across worker threads without external locking. get() returns nullopt on a
// miss; the caller is responsible for computing/loading the value on a miss
// and then calling put(). Note this means N concurrent misses on the same key
// will each compute the value (cache stampede); for static files that is an
// acceptable performance trade-off. A compute_if_absent variant can be added
// later if stampede protection is needed.
//
// Implementation: a doubly-linked list (std::list) ordered MRU-front .. LRU-back
// paired with an unordered_map from key to list iterator. Both splice/erase on
// std::list and iterator stability are O(1), so get/put/erase are all O(1).
template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(std::size_t max_entries)
    : max_entries_(max_entries) {}

    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    // Returns the value on a hit (and promotes the entry to MRU), or nullopt.
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        promote(it->second);
        return it->second->second;
    }

    // Insert or update. Updating an existing key replaces its value and
    // promotes it to MRU. On overflow the LRU (back) entry is evicted.
    // `value` is taken by value so both lvalues and rvalues work (rvalues
    // move into the parameter, avoiding a large copy for e.g. file contents).
    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lock(mutex_);
        put_locked(key, std::move(value));
    }

    // Same as put(key, value) but does not cache anything when capacity is 0.
    // (put() above already handles that via put_locked.)

    // Peek without promoting MRU.
    bool contains(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return index_.find(key) != index_.end();
    }

    // Erase a specific entry. Returns true if it was present.
    bool erase(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        entries_.erase(it->second);
        index_.erase(it);
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        index_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    std::size_t capacity() const { return max_entries_; }

private:
    using ListIt = typename std::list<std::pair<K, V>>::iterator;

    // Caller must hold mutex_.
    void promote(ListIt it) {
        // Move to front (MRU). splice is O(1) and keeps `it` valid.
        entries_.splice(entries_.begin(), entries_, it);
    }

    // Caller must hold mutex_.
    void put_locked(const K& key, V value) {
        if (max_entries_ == 0) return;  // caching disabled
        auto it = index_.find(key);
        if (it != index_.end()) {
            // Update in place and promote.
            it->second->second = std::move(value);
            promote(it->second);
            return;
        }
        // New entry. Evict LRU if at capacity.
        if (entries_.size() >= max_entries_) {
            evict_lru_locked();
        }
        entries_.emplace_front(key, std::move(value));
        index_[key] = entries_.begin();
    }

    // Caller must hold mutex_.
    void evict_lru_locked() {
        if (entries_.empty()) return;
        index_.erase(entries_.back().first);
        entries_.pop_back();
    }

    mutable std::mutex mutex_;
    std::list<std::pair<K, V>> entries_;   // front = MRU, back = LRU
    std::unordered_map<K, ListIt> index_;
    std::size_t max_entries_;
};

}  // namespace muses

#endif  // MUSES_LRU_CACHE_HPP
