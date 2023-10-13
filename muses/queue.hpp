#pragma once

#ifndef _QUEUE_HPP
#define _QUEUE_HPP

#include <functional>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace muses {

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() {}

    void push(T new_value) {
        std::lock_guard<std::mutex> lock(mutex);
        data_queue.push(std::move(new_value));
        data_control_condition.notify_one();
    }

    bool try_pop(T &value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (data_queue.empty()) {
            return false;
        }
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }

    void wait_and_pop(T &value) {
        std::unique_lock<std::mutex> lock(mutex);
        data_control_condition.wait(lock, [this]{return !this->data_queue.empty();});
        value = std::move(data_queue.front());
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
    mutable std::mutex mutex;
    std::queue<T> data_queue;
    std::condition_variable data_control_condition;
};
};

#endif