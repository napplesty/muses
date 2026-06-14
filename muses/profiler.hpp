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
#include <format>
#include <string>
#include <utility>
#include "muses/logging.hpp"

#ifndef MUSES_PROFILER_HPP
#define MUSES_PROFILER_HPP

namespace muses {

// RAII scope timer. Logs the elapsed time (microseconds) at Debug level when
// destroyed. Non-copyable (a timer owns a single measurement); movable so it
// can be returned/stored, though the moved-from object becomes inert.
class Profiler {
public:
    explicit Profiler(const std::string& func_name)
    : func_name(func_name),
      start_time(std::chrono::high_resolution_clock::now()),
      active(true) {}

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    Profiler(Profiler&& other) noexcept
    : func_name(std::move(other.func_name)),
      start_time(other.start_time),
      active(other.active) {
        other.active = false;
    }

    Profiler& operator=(Profiler&& other) noexcept {
        if (this != &other) {
            report();
            func_name = std::move(other.func_name);
            start_time = other.start_time;
            active = other.active;
            other.active = false;
        }
        return *this;
    }

    ~Profiler() {
        report();
    }

private:
    void report() {
        if (!active) {
            return;
        }
        active = false;
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        MUSES_DEBUG(std::format("[Profiler] func: {}, duration: {} us.", func_name, duration));
    }

    std::string func_name;
    std::chrono::high_resolution_clock::time_point start_time;
    bool active;
};

}  // namespace muses

#endif
