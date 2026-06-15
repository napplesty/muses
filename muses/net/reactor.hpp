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

#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <expected>
#include <flat_map>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "muses/bounded_queue.hpp"
#include "muses/logging.hpp"
#include "muses/memory_pool.hpp"
#include "muses/net/poller_factory.hpp"
#include "muses/thread_pool.hpp"

#ifndef MUSES_NET_REACTOR_HPP
#define MUSES_NET_REACTOR_HPP

namespace muses {

// Per-connection state. Owned by the reactor thread exclusively (mutated only
// on the reactor thread), so no locking is required to touch these fields.
struct Connection {
    int fd;
    // Read buffer backed by the reactor's MemoryPool. Reused across keep-alive
    // requests on the same connection (reset_for_reuse keeps the block), so N
    // pipelined requests cost ~1 pool allocation instead of N malloc/free.
    PooledBuffer read_buf;
    bool dispatched = false;  // true while a worker is processing this fd
};

// The reactor owns all fd I/O decisions; worker threads do the heavy compute
// (parse + generate response) and the blocking write. The single reactor
// thread → many workers handoff goes through a bounded outbox, which is the
// ONLY shared mutable channel. This eliminates the old net_driver's data races
// on context_map / occuping_fds.
//
// Handler signature: given the request bytes, return {response, keep_alive}.
// Default handler serves static files via HttpContext.
struct HandlerResult {
    std::string response;
    bool keep_alive;
};

using RequestHandler = std::function<HandlerResult(const std::string& request)>;

// What a worker hands back to the reactor via the outbox.
struct WorkerHandback {
    int fd;
    bool keep_alive;  // reactor re-arms the fd if true
    bool ok;          // false → close the connection
};

class Reactor {
public:
    Reactor(int listen_fd, RequestHandler handler,
            unsigned worker_threads = 4,
            std::size_t outbox_capacity = 1024)
    : listen_fd_(listen_fd),
      handler_(std::move(handler)),
      workers_(worker_threads),
      outbox_(outbox_capacity, OverflowPolicy::DropNew),
      running_(false) {
        set_nonblocking(listen_fd_);
    }

    ~Reactor() { stop(); }

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        poller_ = make_poller();
        if (!poller_ || !poller_->add(listen_fd_, EventMask::Readable, nullptr)) {
            MUSES_ERROR("Reactor: failed to add listen fd to poller");
            running_.store(false);
            return;
        }
        reactor_thread_ = std::thread(&Reactor::loop, this);
        MUSES_INFO("Reactor started");
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (poller_) poller_->wakeup();
        if (reactor_thread_.joinable()) reactor_thread_.join();
        outbox_.stop();
        // Close any remaining connections. Note: flat_map's iterator
        // dereferences to a proxy, so bind via const auto& and copy the fd.
        for (const auto& kv : connections_) {
            ::close(kv.first);
        }
        connections_.clear();
        if (poller_) poller_->del(listen_fd_);
        poller_.reset();
    }

private:
    static void set_nonblocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void loop() {
        constexpr int kMaxEvents = 64;
        PollEvent events[kMaxEvents];
        while (running_.load(std::memory_order_acquire)) {
            int n = poller_->wait(events, 100);
            if (n < 0) {
                MUSES_ERROR("Reactor: poller wait failed");
                continue;
            }
            for (int i = 0; i < n; ++i) {
                int fd = events[i].fd;
                if (fd == listen_fd_) {
                    accept_all();
                } else {
                    on_client_event(fd, events[i]);
                }
            }
            drain_outbox();
        }
    }

    void accept_all() {
        for (;;) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                MUSES_ERROR("Reactor: accept failed");
                break;
            }
            set_nonblocking(fd);
            Connection conn;
            conn.fd = fd;
            // Allocate the read buffer from the reactor's pool (4096-slot size
            // class). Reused across keep-alive requests on this connection.
            conn.read_buf = PooledBuffer(pool_, 4096);
            connections_.emplace(fd, std::move(conn));
            if (!poller_->add(fd, EventMask::Readable, nullptr)) {
                MUSES_ERROR("Reactor: poller add client failed");
                ::close(fd);
                connections_.erase(fd);
                continue;
            }
        }
    }

    void on_client_event(int fd, const PollEvent& ev) {
        if (has(ev.mask, EventMask::Closed)) {
            close_connection(fd);
            return;
        }
        if (!has(ev.mask, EventMask::Readable)) return;

        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        Connection& conn = it->second;
        if (conn.dispatched) return;  // already being processed

        char buf[4096];
        for (;;) {
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0) {
                std::size_t prev_len = conn.read_buf.size();
                if (!conn.read_buf.append(buf, static_cast<std::size_t>(r))) {
                    // Pool allocation failure on grow; drop the connection.
                    close_connection(fd);
                    return;
                }
                if (conn.read_buf.size() > Connection_max_request()) {
                    // Oversized; drop the connection.
                    close_connection(fd);
                    return;
                }
                // Incremental end-of-headers scan: only look at newly-arrived
                // bytes plus a 3-byte overlap with the prior tail, so a
                // "\r\n\r\n" straddling the old/new boundary is still found.
                // This replaces a full-buffer find() on every read (O(n²) when
                // a request arrives in many small packets).
                const std::size_t needle_len = 4;
                const char* base = static_cast<const char*>(conn.read_buf.data());
                std::size_t scan_from = prev_len >= (needle_len - 1)
                    ? prev_len - (needle_len - 1)
                    : 0;
                if (scan_from < conn.read_buf.scan_pos()) {
                    scan_from = conn.read_buf.scan_pos();
                }
                // Search [scan_from, size) for "\r\n\r\n". Inlined (no memmem
                // dependency) — short needle, hot path.
                std::size_t search_len = conn.read_buf.size() - scan_from;
                const char* found = nullptr;
                if (search_len >= needle_len) {
                    for (std::size_t i = 0; i + needle_len <= search_len; ++i) {
                        if (base[scan_from + i] == '\r' &&
                            base[scan_from + i + 1] == '\n' &&
                            base[scan_from + i + 2] == '\r' &&
                            base[scan_from + i + 3] == '\n') {
                            found = base + scan_from + i;
                            break;
                        }
                    }
                }
                if (found != nullptr) {
                    std::size_t hit_off = static_cast<std::size_t>(found - base);
                    conn.read_buf.set_scan_pos(hit_off + needle_len);
                    // Complete request: hand off to a worker. Detach from the
                    // poller while in flight to avoid re-entry.
                    dispatch_to_worker(fd, conn);
                    return;
                }
                // Not yet complete: advance scan_pos to just past the new tail
                // (minus the overlap) so the next read resumes the search.
                conn.read_buf.set_scan_pos(
                    conn.read_buf.size() >= (needle_len - 1)
                        ? conn.read_buf.size() - (needle_len - 1)
                        : 0);
                continue;  // keep draining (level-triggered)
            }
            if (r == 0) {
                // Client closed.
                close_connection(fd);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            // Hard error.
            close_connection(fd);
            return;
        }
    }

    void dispatch_to_worker(int fd, Connection& conn) {
        conn.dispatched = true;
        // Detach read interest while a worker owns the response cycle.
        poller_->del(fd);
        // Copy the accumulated request bytes into a std::string for the
        // handler (its API takes const std::string&), then reset the pooled
        // buffer for the NEXT request on this connection — keep_alive reuses
        // the same pool block (reset_for_reuse does not release it).
        std::string request(static_cast<const char*>(conn.read_buf.data()),
                            conn.read_buf.size());
        conn.read_buf.reset_for_reuse();
        // Capture handler_ by reference (stable for the reactor's lifetime).
        workers_.enqueue([this, fd, request = std::move(request)]() mutable {
            HandlerResult hr;
            try {
                hr = handler_(request);
            } catch (const std::exception& e) {
                MUSES_ERROR(std::string("handler threw: ") + e.what());
                hr.keep_alive = false;
                outbox_.push(WorkerHandback{fd, false, false});
                // Wake the reactor so it drains the outbox promptly instead of
                // waiting for the next poller timeout (which would add up to
                // 100ms of latency per keep-alive request).
                poller_->wakeup();
                return;
            }
            // Blocking write loop (fd is blocking-safe here because we own it
            // exclusively during dispatch).
            bool wrote_ok = write_all(fd, hr.response);
            outbox_.push(WorkerHandback{fd, hr.keep_alive, wrote_ok});
            poller_->wakeup();
        });
    }

    static bool write_all(int fd, const std::string& data) {
        std::size_t total = 0;
        while (total < data.size()) {
            ssize_t w = ::write(fd, data.data() + total, data.size() - total);
            if (w > 0) {
                total += static_cast<std::size_t>(w);
            } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Would block; for simplicity, retry (small responses only).
                continue;
            } else if (w < 0 && errno == EINTR) {
                continue;
            } else if (w < 0) {
                return false;
            }
        }
        return true;
    }

    void drain_outbox() {
        WorkerHandback hb;
        while (outbox_.try_pop(hb)) {
            auto it = connections_.find(hb.fd);
            if (it == connections_.end()) continue;
            if (hb.ok && hb.keep_alive) {
                // Re-arm the fd for the next request on the same connection.
                // read_buf was already reset_for_reuse() at dispatch time;
                // ensure it's clean in case the worker saw a partial next
                // request (defensive — the fd was detached during dispatch).
                it->second.dispatched = false;
                it->second.read_buf.reset_for_reuse();
                poller_->add(hb.fd, EventMask::Readable, nullptr);
            } else {
                close_connection(hb.fd);
            }
        }
    }

    void close_connection(int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        poller_->del(fd);
        ::close(fd);
        connections_.erase(it);
    }

    static constexpr std::size_t Connection_max_request() { return 64 * 1024; }

    int listen_fd_;
    RequestHandler handler_;
    ThreadPool workers_;
    BoundedQueue<WorkerHandback> outbox_;
    std::unique_ptr<Poller> poller_;
    std::thread reactor_thread_;
    std::atomic<bool> running_;
    // Memory pool backing Connection::read_buf. Owned by the reactor, touched
    // only on the reactor thread (single-threaded use → bin mutexes never
    // contend). Wiring the pool onto a real hot path; a thread-local cache
    // layer can be layered on later if this ever goes multi-threaded.
    MemoryPool pool_;
    // Reactor-thread-only state (no lock needed). flat_map keeps the active
    // connection set in a contiguous array — better cache locality than a
    // node-based unordered_map for the typically-small fd set.
    std::flat_map<int, Connection> connections_;
};

// Convenience: a simple synchronous TCP listener wrapping bind/listen.
class TCPListener {
public:
    TCPListener(const std::string& ip, unsigned short port)
    : ip_(ip), port_(port), listen_fd_(-1) {}

    ~TCPListener() {
        if (listen_fd_ != -1) ::close(listen_fd_);
    }

    TCPListener(const TCPListener&) = delete;
    TCPListener& operator=(const TCPListener&) = delete;

    // Returns the listen fd, creating it on first call. On failure returns an
    // unexpected with a human-readable reason (including errno). The caller
    // must check the result before using the fd.
    std::expected<int, std::string> get_listener() {
        if (listen_fd_ != -1) return listen_fd_;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ < 0) {
            return std::unexpected(std::format("TCPListener: socket() failed: {}", std::strerror(errno)));
        }
        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::inet_addr(ip_.c_str());
        addr.sin_port = htons(port_);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::string why = std::format("TCPListener: bind({}, {}) failed: {}", ip_, port_, std::strerror(errno));
            ::close(listen_fd_);
            listen_fd_ = -1;
            return std::unexpected(why);
        }
        if (::listen(listen_fd_, SOMAXCONN) < 0) {
            std::string why = std::format("TCPListener: listen() failed: {}", std::strerror(errno));
            ::close(listen_fd_);
            listen_fd_ = -1;
            return std::unexpected(why);
        }
        // Make the listen socket non-blocking for the reactor.
        int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
        MUSES_INFO("TCPListener listening");
        return listen_fd_;
    }

private:
    std::string ip_;
    unsigned short port_;
    int listen_fd_;
};

}  // namespace muses

#endif  // MUSES_NET_REACTOR_HPP
