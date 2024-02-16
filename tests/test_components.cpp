#include "muses/compiler_defs.hpp"
#include "muses/profiler.hpp"
#include "muses/queue.hpp"
#include "muses/memory_pool.hpp"
#include "muses/thread_pool.hpp"
#include "muses/logging.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <future>
#include <unistd.h>

void thread_func(size_t id) {
    std::stringstream ss;
    ss << "hello from thread " << id;
    MUSES_DEBUG(ss.str());
    ss.str("");
    ss.clear();
    std::cout << "hello from thread" << id << std::endl;
}

int main() {
    muses::ThreadPool pool(20);
    muses::MemoryPool mp(sizeof(int)*100, 10);
    mp.initialize();
    std::cout << mp.allocate(10) << std::endl;

    std::vector<std::future<void> > results;
    muses::Profiler a("test_function");
    for (int i = 0; i < 1000; i++) {
        results.emplace_back(pool.enqueue(thread_func, i));
    }
    for (auto&& result: results) {
        result.get();
    }
    MUSES_DEBUG("time");
    return 0;
}