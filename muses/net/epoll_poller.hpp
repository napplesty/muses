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
#include <flat_map>
#include <vector>
#include <span>

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
        // Use a sentinel fd value to recognize the wakeup eventfd in wait().
        ev.data.fd = kWakeFdSentinel;
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
        // Stash the fd in data.fd (not data.ptr) so wait() can recover it
        // directly from the returned event. userdata is tracked separately.
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
        fds_[fd] = userdata;
        return true;
    }

    bool mod(int fd, EventMask mask, void* userdata) override {
        if (epfd_ == -1) return false;
        struct epoll_event ev{};
        ev.events = to_epoll(mask);
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0) return false;
        auto it = fds_.find(fd);
        if (it != fds_.end()) it->second = userdata;
        return true;
    }

    bool del(int fd) override {
        if (epfd_ == -1) return false;
        fds_.erase(fd);
        return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    int wait(std::span<PollEvent> out, int timeout_ms) override {
        if (epfd_ == -1) return -1;
        if (out.empty()) return 0;
        events_.clear();
        events_.resize(out.size());

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
            if (ee.data.fd == kWakeFdSentinel) {
                // Drain the eventfd counter and swallow the wakeup.
                uint64_t val;
                ::read(wakefd_, &val, sizeof(val));
                continue;
            }
            EventMask m = EventMask::Empty;
            if ((ee.events & (EPOLLIN | EPOLLHUP)) != 0)  m = m | EventMask::Readable;
            if ((ee.events & (EPOLLOUT | EPOLLHUP)) != 0) m = m | EventMask::Writable;
            if ((ee.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) m = m | EventMask::Closed;
            int fd = ee.data.fd;
            void* ud = nullptr;
            if (auto it = fds_.find(fd); it != fds_.end()) ud = it->second;
            out[out_count++] = PollEvent{fd, m, ud};
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
        // EPOLLET makes all registrations edge-triggered: the reactor gets one
        // event per state transition and must drain read/write/accept to
        // EAGAIN. Cuts epoll_wait re-notifications under load.
        e |= EPOLLET;
        return e;
    }

    // Sentinel fd value stored in epoll_event.data.fd to identify the wakeup
    // eventfd. Real fds are non-negative, so -1 is unambiguous.
    static constexpr int kWakeFdSentinel = -1;

    int epfd_;
    int wakefd_;
    // fd → userdata mirror so wait() can return both fd and userdata (epoll's
    // data union is used for the fd to let us recover it directly).
    std::flat_map<int, void*> fds_;
    std::vector<struct epoll_event> events_;
};

}  // namespace muses

#endif  // MUSES_NET_EPOLL_POLLER_HPP
#endif  // MUSES_PLATFORM_EPOLL
