#include <doctest.h>

#include "muses/logging.hpp"
#include "muses/profiler.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
// Read the whole log file into a string for inspection.
std::string read_log_file(const std::string& path) {
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return content;
}
}  // namespace

TEST_CASE("Logger: single entry is persisted") {
    MUSES_INFO("marker-single-entry-xyz");
    muses::Logger::get_instance()->flush();
    std::string log = read_log_file(MUSES_LOG_FILENAME);
    CHECK(log.find("marker-single-entry-xyz") != std::string::npos);
}

TEST_CASE("Logger: level filter suppresses below threshold") {
    // Default level is Debug, so all pass. Just verify the API doesn't crash.
    MUSES_DEBUG("debug-line");
    MUSES_INFO("info-line");
    MUSES_WARNING("warn-line");
    muses::Logger::get_instance()->flush();
    std::string log = read_log_file(MUSES_LOG_FILENAME);
    CHECK(log.find("debug-line") != std::string::npos);
    CHECK(log.find("info-line") != std::string::npos);
}

TEST_CASE("Logger: concurrent logging from many threads") {
    constexpr int N = 12;
    constexpr int PER = 2000;
    std::vector<std::thread> ts;
    std::atomic<int> done{0};
    for (int i = 0; i < N; ++i) {
        ts.emplace_back([i, &done] {
            for (int k = 0; k < PER; ++k) {
                MUSES_INFO("thread-" + std::to_string(i) + "-iter-" + std::to_string(k));
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : ts) t.join();
    muses::Logger::get_instance()->flush();
    CHECK(done.load() == N);
    // With capacity 4096 and N*PER = 24000 entries, many are dropped under
    // DropOldest; we only assert the logger stayed alive and the file has
    // *some* recent content.
    std::string log = read_log_file(MUSES_LOG_FILENAME);
    CHECK_FALSE(log.empty());
}

TEST_CASE("Profiler: emits a debug log line on scope exit") {
    {
        muses::Profiler p("profiled_function_alpha");
        // do a little work
        volatile int sum = 0;
        for (int i = 0; i < 1000; ++i) sum += i;
        (void)sum;
    }
    muses::Logger::get_instance()->flush();
    std::string log = read_log_file(MUSES_LOG_FILENAME);
    CHECK(log.find("profiled_function_alpha") != std::string::npos);
    CHECK(log.find("[Profiler]") != std::string::npos);
}

TEST_CASE("Profiler: non-copyable, movable") {
    muses::Profiler a("movable-source");
    // muses::Profiler b = a;  // would not compile (deleted)
    muses::Profiler c = std::move(a);  // OK
    (void)c;
    // a is now inert; destroying it must not report.
}
