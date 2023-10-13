#include "muses/queue.hpp"
#include "muses/memory_pool.hpp"
#include "muses/thread_pool.hpp"
#include "muses/logging.hpp"
#include <iostream>
#include <vector>
#include <future>

void thread_func(size_t id) {
    MUSES_DEBUG("func");
    //std::cout << "hello from thread" << id << std::endl;
}

int main() {
    muses::ThreadPool pool(8);
    std::vector<std::future<void> > results;
    for (int i = 0; i < 10; i++) {
        results.emplace_back(pool.enqueue(thread_func, i));
    }
    for (auto&& result: results) {
        result.get();
    }
    MUSES_DEBUG("time");
    return 0;
}