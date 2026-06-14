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

#ifndef MUSES_THREAD_POOL_HPP
#define MUSES_THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <atomic>

namespace muses {

// A fixed-size pool of worker threads draining a task queue.
//
// Tasks are submitted via enqueue() and return a std::future of the result.
// On shutdown (destruction or stop()) in-flight queued tasks are still
// executed; new enqueue() calls after stop() return an invalid future instead
// of throwing, so callers polling the future can detect rejection.
class ThreadPool {
public:
    explicit ThreadPool(unsigned int num_threads) : stop(false) {
        for (unsigned int i = 0; i < num_threads; i++) {
            workers.emplace_back([this]() -> void {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{return this->stop.load() || !this->tasks.empty();});
                        if (this->stop.load() && this->tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        --this->outstanding;
                    }
                    this->drained_condition.notify_all();
                }
            });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a callable. Returns an invalid future if the pool has been
    // stopped (caller checks future.valid()).
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result_t<F, Args...>> {
        using return_type = typename std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop.load()) {
                return std::future<return_type>{};  // invalid → caller detects rejection
            }
            tasks.emplace([task]() { (*task)(); });
            ++outstanding;
        }
        condition.notify_one();
        return res;
    }

    // Block until the queue is drained. Mainly for tests.
    void wait_empty() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        drained_condition.wait(lock, [this]{return outstanding == 0;});
    }

    void stop_pool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop.store(true);
        }
        condition.notify_all();
        drained_condition.notify_all();
    }

    ~ThreadPool() {
        stop_pool();
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable drained_condition;
    std::atomic<bool> stop;
    // Number of tasks that have been enqueued but not yet finished.
    std::size_t outstanding = 0;
};

}  // namespace muses

#endif
