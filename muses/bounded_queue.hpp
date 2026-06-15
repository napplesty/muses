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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <semaphore>
#include <utility>

#ifndef MUSES_BOUNDED_QUEUE_HPP
#define MUSES_BOUNDED_QUEUE_HPP

namespace muses {

// What push() does when the queue is full.
enum class OverflowPolicy {
    DropNew,     // reject the new item (push returns false)
    DropOldest,  // evict the front to make room (never blocks producers)
    Block,       // block the producer until space is available
};

// A thread-safe bounded queue with a configurable overflow policy.
//
// Used as the logging backend (MPSC, DropOldest so the hot path never blocks)
// and as the reactor outbox (worker→reactor handoff).
//
// Implementation strategy is chosen once at construction by OverflowPolicy:
//   - DropNew / DropOldest → lock-free Vyukov MPMC bounded ring (the hot path
//     for both logger and reactor). Each slot carries an atomic sequence
//     counter; producer/consumer cursors each live on their own cache line to
//     avoid false sharing. Blocking consumers (wait_pop) are parked on a
//     counting_semaphore that is decoupled from the data path, so try_push /
//     try_pop stay fully lock-free.
//   - Block → a proven mutex + std::deque + two condition variables (the
//     original implementation). Block is only exercised by tests today; keeping
//     the well-tested blocking path avoids hand-rolling a parking producer on
//     top of the ring.
//
// Both paths honor stop(): destructors call stop() so wait_pop() returns false
// on shutdown. This is the fix for the old logger's wait_and_pop() deadlock.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity,
                          OverflowPolicy policy = OverflowPolicy::DropNew)
    : capacity_(capacity == 0 ? 1 : capacity),
      policy_(policy) {
        if (policy_ == OverflowPolicy::Block) {
            blocking_ = std::make_unique<BlockingImpl>(capacity_);
        } else {
            ring_ = std::make_unique<RingImpl>(capacity_, policy_);
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Returns true if the item was enqueued; false if it was dropped/rejected
    // (DropNew when full) or if the queue has been stopped.
    bool push(T item) {
        if (policy_ == OverflowPolicy::Block) {
            return blocking_->push(std::move(item));
        }
        return ring_->push(std::move(item));
    }

    bool try_pop(T& out) {
        if (policy_ == OverflowPolicy::Block) {
            return blocking_->try_pop(out);
        }
        return ring_->try_pop(out);
    }

    // Block until an item is available or the queue is stopped/times out.
    // Returns false on timeout or stop (out is left untouched).
    bool wait_pop(T& out, std::chrono::milliseconds timeout) {
        if (policy_ == OverflowPolicy::Block) {
            return blocking_->wait_pop(out, timeout);
        }
        return ring_->wait_pop(out, timeout);
    }

    // Block indefinitely until an item is available or the queue is stopped.
    bool wait_pop(T& out) {
        if (policy_ == OverflowPolicy::Block) {
            return blocking_->wait_pop(out);
        }
        return ring_->wait_pop(out);
    }

    void stop() {
        if (policy_ == OverflowPolicy::Block) {
            blocking_->stop();
        } else {
            ring_->stop();
        }
    }

    std::size_t size() const {
        if (policy_ == OverflowPolicy::Block) return blocking_->size();
        return ring_->size();
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t dropped() const {
        if (policy_ == OverflowPolicy::Block) return blocking_->dropped();
        return ring_->dropped();
    }

private:
    // ---- Lock-free ring for DropNew / DropOldest ---------------------------

    // A cache-line pad so adjacent atomics don't false-share.
    template <typename Tag>
    struct alignas(64) Padded {
        std::atomic<std::size_t> value{0};
        char pad_[64 - sizeof(std::atomic<std::size_t>)];
    };

    // A ring slot. Holds an atomic sequence (the Vyukov turn counter) and the
    // payload. Cells are NOT movable (atomics aren't), so the ring is a
    // unique_ptr<Cell[]> array — default-constructed in place, never relocated.
    struct Cell {
        Cell() : sequence(0) {}
        std::atomic<std::size_t> sequence;
        std::optional<T> data;
    };

    struct RingImpl {
        RingImpl(std::size_t cap, OverflowPolicy policy) : policy_(policy) {
            // Round capacity up to a power of two for cheap mask-based wrap.
            std::size_t p = 1;
            while (p < cap) p <<= 1;
            mask_ = p - 1;
            // Default-construct p cells in place (no relocation: Cell isn't
            // movable due to its atomic member).
            ring_ = std::unique_ptr<Cell[]>(new Cell[p]{});
            for (std::size_t i = 0; i < p; ++i) {
                ring_[i].sequence.store(i, std::memory_order_relaxed);
            }
            enqueue_pos_.value.store(0, std::memory_order_relaxed);
            dequeue_pos_.value.store(0, std::memory_order_relaxed);
            dropped_.store(0, std::memory_order_relaxed);
            stopped_.store(false, std::memory_order_relaxed);
        }

        bool push(T item) {
            if (stopped_.load(std::memory_order_acquire)) return false;
            for (;;) {
                std::size_t pos = enqueue_pos_.value.load(std::memory_order_relaxed);
                Cell& c = ring_[pos & mask_];
                std::size_t seq = c.sequence.load(std::memory_order_acquire);
                std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) -
                                      static_cast<std::ptrdiff_t>(pos);
                if (diff == 0) {
                    // Slot is empty and ours to claim.
                    if (enqueue_pos_.value.compare_exchange_weak(
                            pos, pos + 1, std::memory_order_relaxed)) {
                        c.data.emplace(std::move(item));
                        c.sequence.store(pos + 1, std::memory_order_release);
                        // Wake one blocked consumer.
                        sem_.release();
                        return true;
                    }
                    // Lost the CAS; retry with the refreshed pos.
                } else if (diff < 0) {
                    // Queue is full.
                    if (policy_ == OverflowPolicy::DropNew) {
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    // DropOldest: evict the front item, then retry the push.
                    if (evict_one()) {
                        continue;  // made room, retry enqueue
                    }
                    // A concurrent consumer drained; retry.
                } else {
                    // Another producer advanced enqueue_pos under us; retry.
                    pos = enqueue_pos_.value.load(std::memory_order_relaxed);
                    (void)pos;
                }
            }
        }

        // Remove and discard the front item (DropOldest overflow). Returns
        // true on success, false if the queue was empty.
        bool evict_one() {
            for (;;) {
                std::size_t pos = dequeue_pos_.value.load(std::memory_order_relaxed);
                Cell& c = ring_[pos & mask_];
                std::size_t seq = c.sequence.load(std::memory_order_acquire);
                std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) -
                                      static_cast<std::ptrdiff_t>(pos + 1);
                if (diff == 0) {
                    // Front slot holds a produced item; claim it for eviction.
                    if (dequeue_pos_.value.compare_exchange_weak(
                            pos, pos + 1, std::memory_order_relaxed)) {
                        c.data.reset();
                        // Mark the slot empty again at its next "empty" seq.
                        c.sequence.store(pos + capacity(), std::memory_order_release);
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                } else if (diff < 0) {
                    // No item at the front (empty); nothing to evict.
                    return false;
                } else {
                    // seq > pos+1: a producer is mid-publish; re-read dequeue_pos.
                    continue;
                }
            }
        }

        bool try_pop(T& out) {
            for (;;) {
                std::size_t pos = dequeue_pos_.value.load(std::memory_order_relaxed);
                Cell& c = ring_[pos & mask_];
                std::size_t seq = c.sequence.load(std::memory_order_acquire);
                std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) -
                                      static_cast<std::ptrdiff_t>(pos + 1);
                if (diff == 0) {
                    if (dequeue_pos_.value.compare_exchange_weak(
                            pos, pos + 1, std::memory_order_relaxed)) {
                        out = std::move(*c.data);
                        c.data.reset();
                        c.sequence.store(pos + capacity(), std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false;  // empty
                } else {
                    // Producer mid-publish; reload and retry.
                }
            }
        }

        bool wait_pop(T& out, std::chrono::milliseconds timeout) {
            // Fast (lock-free) attempt first.
            if (try_pop(out)) return true;
            // Park on the semaphore until data arrives or we time out / stop.
            // try_acquire_for can wake spuriously; loop until a real pop or a
            // genuine stop/timeout. (stop() releases the semaphore to wake us.)
            while (!stopped_.load(std::memory_order_acquire)) {
                if (!sem_.try_acquire_for(timeout)) {
                    // Timed out. One last lock-free check in case a push raced
                    // with the timeout.
                    return try_pop(out);
                }
                if (try_pop(out)) return true;
            }
            return try_pop(out);  // stopped: drain anything left
        }

        bool wait_pop(T& out) {
            if (try_pop(out)) return true;
            while (!stopped_.load(std::memory_order_acquire)) {
                // Block indefinitely for data; stop() breaks us out via the
                // large release + the stopped flag check above.
                sem_.acquire();
                if (try_pop(out)) return true;
            }
            return try_pop(out);
        }

        void stop() {
            stopped_.store(true, std::memory_order_release);
            // Wake every blocked consumer so they observe stopped_ and return.
            sem_.release(std::numeric_limits<int>::max() / 4);
        }

        std::size_t size() const {
            std::size_t enq = enqueue_pos_.value.load(std::memory_order_relaxed);
            std::size_t deq = dequeue_pos_.value.load(std::memory_order_relaxed);
            return enq >= deq ? enq - deq : 0;
        }

        std::size_t dropped() const {
            return dropped_.load(std::memory_order_relaxed);
        }

        std::size_t capacity() const { return mask_ + 1; }

        OverflowPolicy policy_;
        std::unique_ptr<Cell[]> ring_;
        std::size_t mask_;
        Padded<struct EnqTag> enqueue_pos_;
        Padded<struct DeqTag> dequeue_pos_;
        std::atomic<std::size_t> dropped_;
        std::atomic<bool> stopped_;
        // Parking lot for wait_pop consumers. Released by push (one permit per
        // item) and over-released by stop() to flush all waiters.
        std::counting_semaphore<INT_MAX> sem_{0};
    };

    // ---- Mutex + deque for Block -------------------------------------------

    struct BlockingImpl {
        BlockingImpl(std::size_t cap) : capacity_(cap), stopped_(false), dropped_(0) {}

        bool push(T item) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_) return false;
            if (policy_is_block()) {
                not_full_.wait(lock, [this] {
                    return stopped_ || buffer_.size() < capacity_;
                });
                if (stopped_) return false;
            }
            buffer_.push_back(std::move(item));
            not_empty_.notify_one();
            return true;
        }

        bool try_pop(T& out) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (buffer_.empty()) return false;
            out = std::move(buffer_.front());
            buffer_.pop_front();
            not_full_.notify_one();
            return true;
        }

        bool wait_pop(T& out, std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mutex_);
            bool got = not_empty_.wait_for(lock, timeout, [this] {
                return stopped_ || !buffer_.empty();
            });
            if (!got || buffer_.empty()) return false;
            out = std::move(buffer_.front());
            buffer_.pop_front();
            not_full_.notify_one();
            return true;
        }

        bool wait_pop(T& out) {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this] { return stopped_ || !buffer_.empty(); });
            if (buffer_.empty()) return false;
            out = std::move(buffer_.front());
            buffer_.pop_front();
            not_full_.notify_one();
            return true;
        }

        void stop() {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            not_full_.notify_all();
            not_empty_.notify_all();
        }

        std::size_t size() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return buffer_.size();
        }

        std::size_t dropped() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return dropped_;
        }

        bool policy_is_block() const { return true; }

        mutable std::mutex mutex_;
        std::condition_variable not_full_;
        std::condition_variable not_empty_;
        std::deque<T> buffer_;
        std::size_t capacity_;
        bool stopped_;
        std::size_t dropped_;
    };

    std::size_t capacity_;
    OverflowPolicy policy_;
    std::unique_ptr<RingImpl> ring_;
    std::unique_ptr<BlockingImpl> blocking_;
};

}  // namespace muses

#endif  // MUSES_BOUNDED_QUEUE_HPP
