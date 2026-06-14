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
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>

#include "muses/bounded_queue.hpp"

#ifndef MUSES_LOGGING_HPP
#define MUSES_LOGGING_HPP

#ifndef MUSES_LOG_LEVEL
    #define MUSES_LOG_LEVEL ::muses::LogLevel::Debug
#endif

#ifndef MUSES_LOG_FILENAME
    #define MUSES_LOG_FILENAME "log.txt"
#endif

#ifndef MUSES_LOG_CAPACITY
    #define MUSES_LOG_CAPACITY 4096
#endif

namespace muses {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

// Asynchronous logger backed by a bounded MPMC ring (DropOldest so producers
// on the hot path are never blocked). A single background thread drains the
// queue in batches and writes to file.
//
// The old logger deadlocked because its consumer called wait_and_pop() in a
// loop whose predicate checked empty() first; once empty, the pop blocked
// forever and the destructor's join hung. Here the consumer uses
// wait_pop(timeout) and the destructor calls stop() to guarantee the waiter
// unblocks.
class Logger {
public:
    using Entry = std::tuple<LogLevel, std::string, std::string, std::time_t>;

    static Logger* get_instance() {
        static Logger instance(MUSES_LOG_LEVEL, MUSES_LOG_FILENAME, MUSES_LOG_CAPACITY);
        return &instance;
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& message, const std::string& func_name) {
        if (level < level_.load(std::memory_order_relaxed)) {
            return;
        }
        std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        queue_.push(Entry{level, func_name, message, now});
    }

    // Block until all currently-queued entries are flushed to disk. Mainly
    // for tests; not for the hot path. Forces a write regardless of the
    // batching threshold.
    void flush() {
        // Drain anything still queued into the in-memory buffer.
        Entry e;
        while (queue_.try_pop(e)) {
            std::lock_guard<std::mutex> lock(line_mu_);
            buffer_ += format(e);
        }
        // Force the buffer to disk regardless of size.
        std::string to_write;
        {
            std::lock_guard<std::mutex> lock(line_mu_);
            to_write.swap(buffer_);
        }
        if (!to_write.empty()) {
            std::ofstream file(filename_, std::ios::out | std::ios::app);
            if (file.is_open()) {
                file << to_write;
            }
        }
    }

    std::size_t dropped() const { return queue_.dropped(); }

private:
    Logger(LogLevel level, const std::string& filename, std::size_t capacity)
    : level_(level),
      filename_(filename),
      queue_(capacity, OverflowPolicy::DropOldest),
      running_(true),
      thread_(&Logger::writer_loop, this) {}

    ~Logger() {
        running_.store(false, std::memory_order_release);
        queue_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        // Final drain so nothing is lost on shutdown.
        write_all();
    }

    static const char* level_string(LogLevel l) {
        switch (l) {
            case LogLevel::Debug:   return "Debug";
            case LogLevel::Info:    return "Info";
            case LogLevel::Warning: return "Warning";
            case LogLevel::Error:   return "Error";
            case LogLevel::Fatal:   return "Fatal";
        }
        return "Unknown";
    }

    void writer_loop() {
        while (running_.load(std::memory_order_acquire) || queue_.size() > 0) {
            write_all();
            // Throttle: wake every 50ms to drain, or immediately on new data.
            Entry e;
            if (queue_.wait_pop(e, std::chrono::milliseconds(50))) {
                std::lock_guard<std::mutex> lock(line_mu_);
                buffer_ += format(e);
                // Re-inject: batch what else is immediately available.
                Entry more;
                while (queue_.try_pop(more)) {
                    buffer_ += format(more);
                }
            }
            flush_buffer_if_needed();
            {
                std::lock_guard<std::mutex> flk(flush_mu_);
                flush_cv_.notify_all();
            }
        }
        flush_buffer_if_needed();
    }

    void write_all() {
        Entry e;
        {
            std::lock_guard<std::mutex> lock(line_mu_);
            while (queue_.try_pop(e)) {
                buffer_ += format(e);
            }
        }
        flush_buffer_if_needed();
    }

    static std::string format(const Entry& e) {
        char time_buf[64];
        std::tm tm_local{};
        std::time_t t = std::get<3>(e);
        localtime_r(&t, &tm_local);
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_local);

        // std::format is noticeably faster than ostringstream (no virtual
        // dispatch, no locale, fewer allocations) and this is the hot path.
        return std::format("[{}] {} {}: {}\n",
                           level_string(std::get<0>(e)),
                           time_buf,
                           std::get<1>(e),
                           std::get<2>(e));
    }

    void flush_buffer_if_needed() {
        std::string to_write;
        {
            std::lock_guard<std::mutex> lock(line_mu_);
            if (buffer_.empty()) return;
            constexpr std::size_t kFlushThreshold = 128;
            if (buffer_.size() < kFlushThreshold && running_.load(std::memory_order_relaxed)) {
                return;
            }
            to_write.swap(buffer_);
        }
        std::ofstream file(filename_, std::ios::out | std::ios::app);
        if (file.is_open()) {
            file << to_write;
        }
    }

    std::atomic<LogLevel> level_;
    std::string filename_;
    BoundedQueue<Entry> queue_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::string buffer_;
    mutable std::mutex line_mu_;
    mutable std::mutex flush_mu_;
    std::condition_variable flush_cv_;
};

}  // namespace muses

#define MUSES_DEBUG(msg)   ::muses::Logger::get_instance()->log(::muses::LogLevel::Debug,   (msg), __func__)
#define MUSES_INFO(msg)    ::muses::Logger::get_instance()->log(::muses::LogLevel::Info,    (msg), __func__)
#define MUSES_WARNING(msg) ::muses::Logger::get_instance()->log(::muses::LogLevel::Warning, (msg), __func__)
#define MUSES_ERROR(msg)   ::muses::Logger::get_instance()->log(::muses::LogLevel::Error,   (msg), __func__)
#define MUSES_FATAL(msg)   ::muses::Logger::get_instance()->log(::muses::LogLevel::Fatal,   (msg), __func__)

#endif  // MUSES_LOGGING_HPP
