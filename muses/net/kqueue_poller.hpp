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

#ifdef MUSES_PLATFORM_KQUEUE

#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <flat_map>
#include <vector>

#include "muses/logging.hpp"
#include "muses/net/poller.hpp"

#ifndef MUSES_NET_KQUEUE_POLLER_HPP
#define MUSES_NET_KQUEUE_POLLER_HPP

namespace muses {

// kqueue-backed Poller. Tracks per-fd userdata in a map because kqueue
// userdata (udata) is per-changelist-entry but we also need it stable across
// mod() calls. Wakeup uses a user signal filter (EVFILT_USER).
class KQueuePoller : public Poller {
public:
    KQueuePoller() : kq_(::kqueue()) {
        if (kq_ == -1) {
            MUSES_ERROR("kqueue() failed");
            return;
        }
        // Register a self-wakeup filter on a synthetic identifier.
        struct kevent ev{};
        EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1) {
            MUSES_ERROR("kqueue EVFILT_USER setup failed");
        }
    }

    ~KQueuePoller() override {
        if (kq_ != -1) ::close(kq_);
    }

    KQueuePoller(const KQueuePoller&) = delete;
    KQueuePoller& operator=(const KQueuePoller&) = delete;

    bool add(int fd, EventMask mask, void* userdata) override {
        if (kq_ == -1) return false;
        fds_[fd] = userdata;
        apply(fd, mask, EV_ADD);  // buffered into pending_, flushed in wait()
        return true;
    }

    bool mod(int fd, EventMask mask, void* userdata) override {
        if (kq_ == -1) return false;
        auto it = fds_.find(fd);
        if (it == fds_.end()) return false;
        it->second = userdata;
        // Switch interest set: EV_ADD the wanted filters, EV_DELETE the rest.
        // Verified (see kq_mod_test) that a single changelist can both add and
        // delete filters on the same fd atomically. Buffered, flushed in wait().
        apply(fd, mask, EV_ADD);
        return true;
    }

    bool del(int fd) override {
        if (kq_ == -1) return false;
        if (fds_.erase(fd) == 0) return false;
        // Buffer deletion of both read and write filters.
        struct kevent c;
        EV_SET(&c, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        pending_.push_back(c);
        EV_SET(&c, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        pending_.push_back(c);
        return true;
    }

    int wait(std::span<PollEvent> out, int timeout_ms) override {
        if (kq_ == -1) return -1;
        if (out.empty()) return 0;
        // Flush any buffered interest changes in ONE kevent call before
        // blocking — this is the Tier-2 batched-changelist optimization. All
        // add/mod/del since the last wait() land here together.
        flush_pending();
        events_.clear();
        events_.resize(out.size());

        struct timespec ts{};
        struct timespec* pts = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000L * 1000L;
            pts = &ts;
        }

        int n = ::kevent(kq_, nullptr, 0, events_.data(),
                         static_cast<int>(events_.size()), pts);
        if (n < 0) {
            if (errno == EINTR) return 0;
            MUSES_ERROR("kevent wait failed");
            return -1;
        }
        int out_count = 0;
        for (int i = 0; i < n; ++i) {
            const struct kevent& kev = events_[i];
            if (kev.filter == EVFILT_USER) {
                // Wakeup marker; swallowed (no output event).
                continue;
            }
            EventMask m = EventMask::Empty;
            if (kev.filter == EVFILT_READ)  m = m | EventMask::Readable;
            if (kev.filter == EVFILT_WRITE) m = m | EventMask::Writable;
            if ((kev.flags & (EV_EOF | EV_ERROR)) != 0) m = m | EventMask::Closed;

            int fd = static_cast<int>(kev.ident);
            void* ud = nullptr;
            auto it = fds_.find(fd);
            if (it != fds_.end()) ud = it->second;

            out[out_count++] = PollEvent{fd, m, ud};
        }
        return out_count;
    }

    void wakeup() override {
        if (kq_ == -1) return;
        struct kevent ev{};
        EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

private:
    // Buffer a single kevent change for later batched submission. kflags carries
    // the operation (EV_ADD / EV_DELETE); EV_CLEAR is OR'd in for ADDs so all
    // socket filters are edge-triggered.
    void buffer_change(int fd, int16_t filter, uint16_t kflags) {
        struct kevent c;
        EV_SET(&c, fd, filter, kflags, 0, 0, nullptr);
        pending_.push_back(c);
    }

    // Record the interest-set change for `fd` into the pending buffer.
    // Wanted filters (per `mask`) get EV_ADD|EV_CLEAR; unwanted filters get
    // EV_DELETE — so a single flush atomically switches the interest set (this
    // is what makes mod() correct and lets one kevent call replace del+add).
    void apply(int fd, EventMask mask, uint16_t /*kflags*/) {
        const uint16_t add_flags = EV_ADD | EV_CLEAR;
        if (has(mask, EventMask::Readable)) {
            buffer_change(fd, EVFILT_READ, add_flags);
        } else {
            buffer_change(fd, EVFILT_READ, EV_DELETE);
        }
        if (has(mask, EventMask::Writable)) {
            buffer_change(fd, EVFILT_WRITE, add_flags);
        } else {
            buffer_change(fd, EVFILT_WRITE, EV_DELETE);
        }
    }

    // Submit all buffered changes in a single kevent() call, then clear the
    // buffer. Called once at the top of wait().
    //
    // kevent processes a changelist and, if a change fails (e.g. EV_DELETE on a
    // filter that was never registered — ENOENT), it records the error in the
    // OUTPUT event array rather than aborting the batch. We pass an output
    // buffer sized to the batch so per-change errors are reported there and do
    // NOT stop later changes in the same batch from applying. The errors we see
    // (ENOENT on a redundant EV_DELETE) are all benign, so we discard them.
    void flush_pending() {
        if (pending_.empty()) return;
        // Reuse events_ as scratch for per-change error reporting.
        if (events_.size() < pending_.size()) events_.resize(pending_.size());
        ::kevent(kq_, pending_.data(), static_cast<int>(pending_.size()),
                 events_.data(), static_cast<int>(events_.size()), nullptr);
        pending_.clear();
    }

    static constexpr uintptr_t kWakeIdent = 1;
    int kq_;
    // flat_map: contiguous storage, cache-friendly for the small fd set.
    std::flat_map<int, void*> fds_;
    std::vector<struct kevent> events_;
    // Buffered interest changes (add/mod/del) — flushed as one kevent() call at
    // the start of wait(). This is the Tier-2 batched-changelist optimization:
    // N interest changes per loop iteration cost ONE syscall instead of N.
    std::vector<struct kevent> pending_;
};

}  // namespace muses

#endif  // MUSES_NET_KQUEUE_POLLER_HPP
#endif  // MUSES_PLATFORM_KQUEUE
