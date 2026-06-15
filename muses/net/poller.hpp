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

#include <cstdint>
#include <memory>
#include <span>

#ifndef MUSES_NET_POLLER_HPP
#define MUSES_NET_POLLER_HPP

namespace muses {

// Bit flags describing which I/O events a fd is interested in / has fired.
enum class EventMask : uint16_t {
    Empty    = 0,
    Readable = 1u << 0,
    Writable = 1u << 1,
    Closed   = 1u << 2,   // hangup / error
};

constexpr EventMask operator|(EventMask a, EventMask b) {
    return static_cast<EventMask>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr EventMask operator&(EventMask a, EventMask b) {
    return static_cast<EventMask>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr bool has(EventMask a, EventMask b) {
    return (static_cast<uint16_t>(a) & static_cast<uint16_t>(b)) != 0;
}

// A single ready event returned by Poller::wait.
struct PollEvent {
    int fd;
    EventMask mask;
    void* userdata;   // opaque per-fd pointer (e.g. a Connection*)
};

// Abstract reactor-side multiplexer. Implementations wrap kqueue (macOS) or
// epoll (Linux). All fds are expected to be non-blocking and are registered
// EDGE-TRIGGERED (kqueue EV_CLEAR / epoll EPOLLET): the reactor gets one event
// per state transition and MUST drain read/write/accept loops to EAGAIN,
// otherwise readiness is lost. wakeup() lets another thread interrupt a
// blocked wait() (used by the reactor to process worker handback via an
// outbox).
class Poller {
public:
    virtual ~Poller() = default;

    // Register interest in `mask` on `fd`. Returns false on failure.
    virtual bool add(int fd, EventMask mask, void* userdata) = 0;
    // Change the interest set of an already-registered fd.
    virtual bool mod(int fd, EventMask mask, void* userdata) = 0;
    // Deregister a fd.
    virtual bool del(int fd) = 0;

    // Block up to timeout_ms (-1 = forever) for events; fill `out` and return
    // the count written. Returns -1 on error. The span carries its own size so
    // callers cannot overrun the buffer.
    virtual int wait(std::span<PollEvent> out, int timeout_ms) = 0;

    // Interrupt a blocked wait() from another thread. Idempotent.
    virtual void wakeup() = 0;
};

// Factory: returns a kqueue-backed Poller on macOS, epoll-backed on Linux.
std::unique_ptr<Poller> make_poller();

}  // namespace muses

#endif  // MUSES_NET_POLLER_HPP
