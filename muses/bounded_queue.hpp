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

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

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
// The two condition variables plus a stop() flag guarantee that no waiter can
// block forever: destructors call stop() so wait_pop() returns false on
// shutdown. This is the fix for the old logger's wait_and_pop() deadlock.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity, OverflowPolicy policy = OverflowPolicy::DropNew)
    : capacity_(capacity == 0 ? 1 : capacity),
      policy_(policy),
      stopped_(false),
      dropped_(0) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Returns true if the item was enqueued; false if it was dropped/rejected
    // (DropNew when full) or if the queue has been stopped.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) return false;

        if (policy_ == OverflowPolicy::Block) {
            not_full_.wait(lock, [this] { return stopped_ || buffer_.size() < capacity_; });
            if (stopped_) return false;
        } else if (buffer_.size() >= capacity_) {
            if (policy_ == OverflowPolicy::DropNew) {
                ++dropped_;
                return false;
            }  // DropOldest: fall through, evict below.
            buffer_.pop_front();
            ++dropped_;
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

    // Block until an item is available or the queue is stopped/times out.
    // Returns false on timeout or stop (out is left untouched).
    bool wait_pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool got = not_empty_.wait_for(lock, timeout, [this] {
            return stopped_ || !buffer_.empty();
        });
        if (!got || buffer_.empty()) {
            return false;  // timeout, or stopped while empty
        }
        out = std::move(buffer_.front());
        buffer_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // Block indefinitely until an item is available or the queue is stopped.
    bool wait_pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return stopped_ || !buffer_.empty(); });
        if (buffer_.empty()) {
            return false;  // stopped
        }
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

    std::size_t capacity() const { return capacity_; }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> buffer_;
    std::size_t capacity_;
    OverflowPolicy policy_;
    bool stopped_;
    std::size_t dropped_;
};

}  // namespace muses

#endif  // MUSES_BOUNDED_QUEUE_HPP
