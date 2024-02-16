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

#include <functional>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <limits>

#ifndef _MUSES_QUEUE_HPP
#define _MUSES_QUEUE_HPP

namespace muses {

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() 
    : capacity(std::numeric_limits<unsigned int>::max()),
    num_elem(0) {}

    void set_capacity(unsigned int capacity) {
        std::unique_lock<std::mutex> lock(mutex);
        this->capacity = capacity;
    }

    // Notice: If the number of the elements surpasses the capacity of the queue,
    // the push operation will be blocked until the number of elements 
    // becomes lower than the capacity
    void push(T new_value) {
        std::unique_lock<std::mutex> lock(mutex);
        data_control_condition.wait(lock, [this]{return this->capacity > this->num_elem;});
        data_queue.push(std::move(new_value));
        num_elem ++;
        data_control_condition.notify_one();
    }

    bool try_pop(T &value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        num_elem --;
        return true;
    }

    void wait_and_pop(T &value) {
        std::unique_lock<std::mutex> lock(mutex);
        data_control_condition.wait(lock, [this]{return !this->data_queue.empty();});
        value = std::move(data_queue.front());
        num_elem --;
        data_queue.pop();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mutex);
        if (data_queue.empty()) {
            return true;
        }
        return false;
    }

private:
    unsigned int capacity;
    unsigned int num_elem;
    mutable std::mutex mutex;
    std::queue<T> data_queue;
    std::condition_variable data_control_condition;
};
};

#endif