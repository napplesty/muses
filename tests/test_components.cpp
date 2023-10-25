#include "muses/queue.hpp"
#include "muses/memory_pool.hpp"
#include "muses/thread_pool.hpp"
#include "muses/logging.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <future>

void thread_func(size_t id) {
    std::stringstream ss;
    ss << "hello from thread " << id;
    MUSES_DEBUG(ss.str());
    ss.str("");
    ss.clear();
    std::cout << "hello from thread" << id << std::endl;
}

int main() {
    muses::ThreadPool pool(7);
    muses::MemoryPool mp(sizeof(int)*100, 10);
    mp.initialize();
    std::cout << mp.allocate() << std::endl;
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