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

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "muses/net/poller.hpp"

#ifndef MUSES_TASK_HPP
#define MUSES_TASK_HPP

namespace muses {

// A minimal coroutine task type that suspends on a Poller. This is the glue
// between C++20 coroutines (a language feature; C++23 has no standard async I/O
// library — std::execution is C++26) and our hand-written edge-triggered
// Poller. The Poller IS the coroutine scheduler: awaitables register fd
// interest with it, and the event loop resumes coroutines when fds become ready.
//
// Design points:
//   - final_suspend suspends (returns suspend_always): the coroutine frame is
//     NOT auto-destroyed on completion. The owner of the Task must destroy it
//     (the Task RAII handle does so in its destructor unless moved-from). This
//     prevents the event loop from resuming a destroyed handle.
//   - The Poller's per-fd `userdata` slot carries the coroutine_handle to
//     resume on readiness. IoAwaiter::await_suspend stores the handle there.
//   - Single-threaded: coroutines are created and resumed only on the event
//     loop thread. No cross-thread handle.resume().
//   - Task is move-only; the moved-from Task is inert (nullptr handle).

template <typename T = void>
class Task {
public:
    class promise_type {
    public:
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        template <typename U>
        void return_value(U&& value) {
            result_.template emplace<1>(std::forward<U>(value));
        }

        void unhandled_exception() {
            // Store the exception so the awaiter can rethrow. For simplicity in
            // a header-only learning project we don't propagate via
            // std::rethrow_exception here; callers check has_result().
            result_ = std::current_exception();
        }

        bool has_result() const noexcept {
            return result_.index() != 0;
        }
        T& value() { return std::get<1>(result_); }
        const T& value() const { return std::get<1>(result_); }

    private:
        // 0 = empty, 1 = value, 2 = exception pointer.
        std::variant<std::monostate, T, std::exception_ptr> result_;
    };

    Task() noexcept : handle_(nullptr) {}
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    // Begin/resume the coroutine. Returns true if it ran to final suspension
    // (i.e. produced a result); false if it suspended awaiting I/O.
    bool resume() {
        if (!handle_) return false;
        handle_.resume();
        return handle_.done();
    }

    bool done() const noexcept { return handle_ && handle_.done(); }
    bool valid() const noexcept { return handle_ != nullptr; }

    std::coroutine_handle<promise_type> handle() const noexcept { return handle_; }
    promise_type& promise() { return handle_.promise(); }

    // Take ownership of the underlying handle (the Task becomes inert).
    std::coroutine_handle<promise_type> take() noexcept {
        auto h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

// Specialization for void-returning coroutines.
template <>
class Task<void> {
public:
    class promise_type {
    public:
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { ep_ = std::current_exception(); }
        std::exception_ptr ep_;
    };

    Task() noexcept : handle_(nullptr) {}
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    bool resume() {
        if (!handle_) return false;
        handle_.resume();
        return handle_.done();
    }
    bool done() const noexcept { return handle_ && handle_.done(); }
    bool valid() const noexcept { return handle_ != nullptr; }
    std::coroutine_handle<promise_type> handle() const noexcept { return handle_; }
    std::coroutine_handle<promise_type> take() noexcept {
        auto h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

// An awaitable that registers fd interest on a Poller and suspends the
// coroutine until the fd is ready (Readable and/or Writable). When the event
// loop processes the poller event, it resumes the coroutine handle that was
// stored as the fd's userdata.
//
// Usage:  co_await IoAwaiter{poller, fd, EventMask::Readable};
// The Poller must be the one driven by the event loop that owns this coroutine.
class IoAwaiter {
public:
    IoAwaiter(Poller* poller, int fd, EventMask mask) noexcept
    : poller_(poller), fd_(fd), mask_(mask), result_(false) {}

    // Ready only if there's nothing to wait on (degenerate); otherwise suspend.
    bool await_ready() const noexcept { return false; }

    // Register interest and suspend. The coroutine handle becomes the poller's
    // userdata for this fd so the loop can resume us.
    void await_suspend(std::coroutine_handle<> h) noexcept {
        // Store the handle so the event loop can find it. We register it as the
        // fd's userdata; the loop extracts it on readiness.
        handle_ = h;
        poller_->add(fd_, mask_, h.address());
    }

    // Called when resumed. Returns true if the fd is still usable (the caller
    // should re-check via read/write which may still EAGAIN under ET).
    bool await_resume() noexcept { return result_; }

    // Set by the event loop before resuming (e.g. false if the fd was closed).
    void set_result(bool ok) noexcept { result_ = ok; }

    int fd() const noexcept { return fd_; }
    std::coroutine_handle<> handle() const noexcept { return handle_; }

private:
    Poller* poller_;
    int fd_;
    EventMask mask_;
    bool result_;
    std::coroutine_handle<> handle_;
};

// Convenience: the event loop uses this to extract the suspended coroutine
// handle from a PollEvent's userdata and resume it. Returns the handle (or
// nullptr if none).
inline std::coroutine_handle<> handle_from_event(const PollEvent& ev) noexcept {
    return std::coroutine_handle<>::from_address(ev.userdata);
}

}  // namespace muses

#endif  // MUSES_TASK_HPP
