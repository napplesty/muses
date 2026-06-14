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

#include <queue>
#include <mutex>
#include <condition_variable>
#include <limits>
#include <atomic>

#ifndef MUSES_QUEUE_HPP
#define MUSES_QUEUE_HPP

namespace muses {

// Unbounded thread-safe queue with an optional soft capacity. push() blocks
// when the number of elements reaches the capacity, and unblocks as consumers
// drain it. stop() unblocks any blocked producer/consumer; after stop(), push
// returns false and wait_and_pop returns false.
//
// Note: the reactor/net layer uses BoundedQueue (see bounded_queue.hpp) which
// supports overflow policies. This unbounded queue is retained for general
// producer/consumer use and tests.
template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue()
    : capacity(std::numeric_limits<unsigned int>::max()),
      stopped(false) {}

    void set_capacity(unsigned int cap) {
        std::lock_guard<std::mutex> lock(mutex);
        this->capacity = cap;
    }

    // Returns false if the queue has been stopped.
    bool push(T new_value) {
        std::unique_lock<std::mutex> lock(mutex);
        data_control_condition.wait(lock, [this]{
            return stopped || this->capacity > this->data_queue.size();
        });
        if (stopped) {
            return false;
        }
        data_queue.push(std::move(new_value));
        data_control_condition.notify_one();
        return true;
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }

    // Returns false if the queue has been stopped.
    bool wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        data_control_condition.wait(lock, [this]{
            return stopped || !this->data_queue.empty();
        });
        if (stopped && data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        data_control_condition.notify_one();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return data_queue.empty();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
        data_control_condition.notify_all();
    }

private:
    unsigned int capacity;
    std::atomic<bool> stopped;
    mutable std::mutex mutex;
    std::queue<T> data_queue;
    std::condition_variable data_control_condition;
};

}  // namespace muses

#endif
