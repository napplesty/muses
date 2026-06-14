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

#include "muses/config.hpp"

#ifdef MUSES_PLATFORM_EPOLL

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include "muses/logging.hpp"
#include "muses/net/poller.hpp"

#ifndef MUSES_NET_EPOLL_POLLER_HPP
#define MUSES_NET_EPOLL_POLLER_HPP

namespace muses {

// epoll-backed Poller. Wakeup uses an eventfd registered at EPOLLIN.
class EpollPoller : public Poller {
public:
    EpollPoller() : epfd_(::epoll_create1(0)), wakefd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if (epfd_ == -1) {
            MUSES_ERROR("epoll_create1 failed");
            return;
        }
        if (wakefd_ == -1) {
            MUSES_ERROR("eventfd failed");
            return;
        }
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = kWakeTag;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev) == -1) {
            MUSES_ERROR("epoll_ctl add eventfd failed");
        }
    }

    ~EpollPoller() override {
        if (wakefd_ != -1) ::close(wakefd_);
        if (epfd_ != -1) ::close(epfd_);
    }

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    bool add(int fd, EventMask mask, void* userdata) override {
        if (epfd_ == -1) return false;
        struct epoll_event ev{};
        ev.events = to_epoll(mask);
        ev.data.ptr = userdata;
        return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool mod(int fd, EventMask mask, void* userdata) override {
        if (epfd_ == -1) return false;
        struct epoll_event ev{};
        ev.events = to_epoll(mask);
        ev.data.ptr = userdata;
        return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    bool del(int fd) override {
        if (epfd_ == -1) return false;
        return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    int wait(PollEvent* out, int max, int timeout_ms) override {
        if (epfd_ == -1) return -1;
        events_.clear();
        events_.resize(static_cast<std::size_t>(max > 0 ? max : 0));
        if (events_.empty()) return 0;

        int n = ::epoll_wait(epfd_, events_.data(),
                             static_cast<int>(events_.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) return 0;
            MUSES_ERROR("epoll_wait failed");
            return -1;
        }
        int out_count = 0;
        for (int i = 0; i < n; ++i) {
            const struct epoll_event& ee = events_[i];
            if (ee.data.ptr == kWakeTag) {
                // Drain the eventfd counter and swallow the wakeup.
                uint64_t val;
                ::read(wakefd_, &val, sizeof(val));
                continue;
            }
            EventMask m = EventMask::Empty;
            if ((ee.events & (EPOLLIN | EPOLLHUP)) != 0)  m = m | EventMask::Readable;
            if ((ee.events & (EPOLLOUT | EPOLLHUP)) != 0) m = m | EventMask::Writable;
            if ((ee.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) m = m | EventMask::Closed;
            // Note: fd is not stored (we use data.ptr). Callers that need fd
            // should embed it in their userdata.
            out[out_count++] = PollEvent{-1, m, ee.data.ptr};
        }
        return out_count;
    }

    void wakeup() override {
        if (wakefd_ == -1) return;
        uint64_t one = 1;
        ::write(wakefd_, &one, sizeof(one));
    }

private:
    static uint32_t to_epoll(EventMask m) {
        uint32_t e = 0;
        if (has(m, EventMask::Readable)) e |= EPOLLIN;
        if (has(m, EventMask::Writable)) e |= EPOLLOUT;
        return e;
    }

    // Sentinel data.ptr identifying the wakeup eventfd.
    static inline void* kWakeTag = reinterpret_cast<void*>(0x1);

    int epfd_;
    int wakefd_;
    std::vector<struct epoll_event> events_;
};

}  // namespace muses

#endif  // MUSES_NET_EPOLL_POLLER_HPP
#endif  // MUSES_PLATFORM_EPOLL
