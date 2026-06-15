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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <flat_map>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "muses/bloom_filter.hpp"
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
    // --- Asynchronous write state (reactor-thread-only) ---
    // When a response can't be fully written in one go (EAGAIN on a full send
    // buffer), the remainder is parked here and the fd is armed for Writable
    // so the reactor drains it incrementally. Empty/!writing means idle.
    std::string write_buf;    // remaining response bytes to send
    std::size_t write_off = 0;  // next byte to write within write_buf
    bool keep_alive_after_write = false;  // keep-alive decision for this response
    bool writing = false;     // true while a partial write is parked
    // --- DoS hardening (reactor-thread-only) ---
    // Last time this connection showed activity (data read, accepted, or a
    // response cycle completed). The reactor sweep closes connections idle for
    // longer than idle_timeout (slowloris defense). steady_clock is monotonic.
    std::chrono::steady_clock::time_point last_active;
    // Client IPv4 (host byte order) captured at accept, for accounting.
    std::uint32_t client_ip = 0;
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

// What a worker hands back to the reactor via the outbox. The worker computes
// the response and returns it; the reactor (which owns the fd and the poller)
// does the actual non-blocking write. This keeps the write on the reactor
// thread so a full send buffer parks the response on the Connection instead of
// burning a worker core in a busy write loop.
struct WorkerHandback {
    int fd;
    bool keep_alive;       // reactor re-arms the fd for reading if true
    std::string response;  // bytes the reactor must write to the fd
};

class Reactor {
public:
    // DoS-hardening knobs (all optional, sensible defaults):
    //   idle_timeout       — close a connection idle longer than this (slowloris
    //                        defense). 0 disables the sweep. Default 30s.
    //   max_connections    — reject new accepts above this live count (fd-exhaustion
    //                        defense). 0 disables. Default 10000.
    //   ip_rate_per_sec    — max new connections per source IP per second before it
    //                        is blacklisted (connection-flood defense). 0 disables.
    //                        Default 0 (off) so localhost benchmarks aren't tripped.
    Reactor(int listen_fd, RequestHandler handler,
            unsigned worker_threads = 4,
            std::size_t outbox_capacity = 1024,
            std::chrono::seconds idle_timeout = std::chrono::seconds(30),
            std::size_t max_connections = 10000,
            std::size_t ip_rate_per_sec = 0)
    : listen_fd_(listen_fd),
      handler_(std::move(handler)),
      workers_(worker_threads),
      outbox_(outbox_capacity, OverflowPolicy::DropNew),
      running_(false),
      idle_timeout_(idle_timeout),
      max_connections_(max_connections),
      ip_rate_per_sec_(ip_rate_per_sec) {
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
            // Periodic housekeeping: close idle connections (slowloris defense)
            // and age the per-IP blacklist. Runs roughly every 100ms (the wait
            // timeout), cheap relative to the wait itself.
            sweep_idle_connections();
            sweep_ip_rate();
        }
    }

    // Close any connection that has been idle (no read / accept / response
    // activity) for longer than idle_timeout_. Slowloris defense: a client
    // that opens a connection and trickles bytes (or nothing) gets dropped.
    void sweep_idle_connections() {
        if (idle_timeout_ == std::chrono::seconds(0)) return;
        auto now = std::chrono::steady_clock::now();
        // Collect first, then close: closing erases from connections_ and
        // invalidates iterators, so we can't close while iterating.
        std::vector<int> to_close;
        for (const auto& [fd, conn] : connections_) {
            if (now - conn.last_active > idle_timeout_) {
                to_close.push_back(fd);
            }
        }
        for (int fd : to_close) {
            close_connection(fd);
        }
    }

    // Periodically (a) decay the IP blacklist so a one-off offender is
    // un-banned after a while, and (b) prune expired entries from the
    // sliding-window counters so the map doesn't grow unbounded.
    void sweep_ip_rate() {
        if (ip_rate_per_sec_ == 0) return;
        auto now = std::chrono::steady_clock::now();
        if (now - last_ip_decay_ < std::chrono::seconds(10)) return;
        last_ip_decay_ = now;
        ip_blacklist_.decay(1);
        // Prune counter entries older than the 2s window.
        std::vector<std::uint32_t> stale;
        for (const auto& [ip, win] : ip_connect_windows_) {
            if (now - win.window_start > std::chrono::seconds(2)) {
                stale.push_back(ip);
            }
        }
        for (std::uint32_t ip : stale) ip_connect_windows_.erase(ip);
    }

    void accept_all() {
        // Cap accepts per call so a connection storm can't starve servicing of
        // existing connections. Under edge-triggered I/O the listen fd is
        // drained to EAGAIN; remaining pending connections re-edge on the next
        // accept batch. 128 is large enough to amortize the kevent/epoll_wait
        // wakeup while bounding per-iteration work.
        constexpr int kAcceptBatch = 128;
        auto now = std::chrono::steady_clock::now();
        for (int accepted = 0; accepted < kAcceptBatch; ++accepted) {
            // DoS gate 1: max live connections. Stop accepting once at the cap
            // so a connection flood can't exhaust the fd table or memory.
            if (max_connections_ != 0 && connections_.size() >= max_connections_) {
                break;
            }
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                MUSES_ERROR("Reactor: accept failed");
                break;
            }
            // Capture the client IPv4 (host order) for rate-limit accounting.
            std::uint32_t ip = ntohl(addr.sin_addr.s_addr);
            // DoS gate 2: per-IP connection-rate limit. A blacklisted IP (one
            // that previously exceeded the rate) is rejected without further
            // work. The blacklist ages out via sweep_ip_rate's decay.
            if (ip_rate_per_sec_ != 0 && ip != 0) {
                if (ip_blacklist_.contains(ip)) {
                    ::close(fd);
                    continue;
                }
                if (ip_rate_exceeded(ip, now)) {
                    // Just crossed the threshold → blacklist this IP.
                    ip_blacklist_.add(ip);
                    MUSES_WARNING(std::format("Reactor: IP {:08x} blacklisted for connection flood", ip));
                    ::close(fd);
                    continue;
                }
            }
            set_nonblocking(fd);
            Connection conn;
            conn.fd = fd;
            conn.last_active = now;
            conn.client_ip = ip;
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

    // Sliding-window per-IP new-connection counter. Returns true if `ip` has
    // exceeded the configured rate within the current 1s window, in which case
    // the caller blacklists it. The window auto-advances on access; stale
    // windows are pruned by sweep_ip_rate.
    struct RateWindow {
        std::chrono::steady_clock::time_point window_start;
        std::size_t count = 0;
    };
    bool ip_rate_exceeded(std::uint32_t ip, std::chrono::steady_clock::time_point now) {
        auto& win = ip_connect_windows_[ip];
        if (now - win.window_start >= std::chrono::seconds(1)) {
            // Advance the window.
            win.window_start = now;
            win.count = 0;
        }
        ++win.count;
        return win.count > ip_rate_per_sec_;
    }

    void on_client_event(int fd, const PollEvent& ev) {
        if (has(ev.mask, EventMask::Closed)) {
            close_connection(fd);
            return;
        }
        // Async write drain: a prior response parked its remainder on the
        // connection; the fd is now writable again.
        if (has(ev.mask, EventMask::Writable)) {
            auto it = connections_.find(fd);
            if (it != connections_.end() && it->second.writing) {
                continue_write(it->second);
            }
            // Note: a Writable event with no parked write is unexpected; ignore.
            return;
        }
        if (!has(ev.mask, EventMask::Readable)) return;

        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        Connection& conn = it->second;
        if (conn.dispatched) return;  // already being processed

        char buf[4096];
        bool got_complete = false;  // saw a "\r\n\r\n" this round
        for (;;) {
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0) {
                // Data arrived → connection is active (refresh idle deadline).
                conn.last_active = std::chrono::steady_clock::now();
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
                    got_complete = true;
                    // IMPORTANT (edge-triggered): keep draining the socket to
                    // EAGAIN before dispatching. Any pipelined bytes for the
                    // NEXT request are already in the socket buffer; if we
                    // dispatch now and return, those bytes are consumed from
                    // the kernel but never re-edge under ET. By continuing the
                    // read loop we pull them into read_buf; dispatch_to_worker
                    // then carves off only the first request and leaves the
                    // rest, which finish_response re-checks after the worker
                    // returns.
                    continue;
                }
                // Not yet complete: advance scan_pos to just past the new tail
                // (minus the overlap) so the next read resumes the search.
                conn.read_buf.set_scan_pos(
                    conn.read_buf.size() >= (needle_len - 1)
                        ? conn.read_buf.size() - (needle_len - 1)
                        : 0);
                continue;  // keep draining
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
        // Socket drained to EAGAIN. If we saw a complete request, dispatch it
        // now (with any pipelined bytes safely buffered in read_buf).
        if (got_complete) {
            dispatch_to_worker(fd, conn);
        }
    }

    void dispatch_to_worker(int fd, Connection& conn) {
        conn.dispatched = true;
        // NOTE: the fd is intentionally LEFT armed (Readable) during dispatch.
        // The `dispatched` flag is the re-entry guard (on_client_event checks
        // it first), so there's no need for a poller del — saving one syscall
        // per request. The edge-triggered concern (a Readable edge that fires
        // while dispatched is swallowed, losing the readiness) is handled by a
        // speculative drain in finish_response: when the worker hands back, we
        // read the socket to EAGAIN before re-checking the buffer, so any data
        // that arrived during dispatch is pulled in explicitly.
        // Copy only the COMPLETE first request (bytes [0, scan_pos)) into a
        // std::string for the handler. Any pipelined bytes after scan_pos stay
        // in read_buf for the next round (critical under edge-triggered I/O:
        // those bytes were already read off the socket, so no new edge will
        // fire for them — finish_response must re-check the buffer).
        std::size_t req_len = conn.read_buf.scan_pos();
        if (req_len == 0) req_len = conn.read_buf.size();
        std::string request(static_cast<const char*>(conn.read_buf.data()), req_len);
        // Shift the leftover pipelined bytes to the front of the buffer and
        // reset scan_pos so the next scan starts fresh.
        std::size_t leftover = conn.read_buf.size() - req_len;
        if (leftover > 0 && req_len > 0) {
            std::memmove(conn.read_buf.data(),
                         static_cast<const char*>(conn.read_buf.data()) + req_len,
                         leftover);
        }
        conn.read_buf.set_size(leftover);
        conn.read_buf.set_scan_pos(0);
        // Capture handler_ by reference (stable for the reactor's lifetime).
        workers_.enqueue([this, fd, request = std::move(request)]() mutable {
            HandlerResult hr;
            bool handler_ok = true;
            try {
                hr = handler_(request);
            } catch (const std::exception& e) {
                MUSES_ERROR(std::string("handler threw: ") + e.what());
                hr.keep_alive = false;
                handler_ok = false;
            }
            // Hand the response bytes (empty on handler failure) to the
            // reactor; the reactor does the write on its own thread. This
            // avoids the old busy-spin in write_all() on EAGAIN.
            bool pushed = outbox_.push(WorkerHandback{
                fd,
                handler_ok && hr.keep_alive,
                std::move(hr.response)});
            if (!pushed) {
                // Outbox full (DropNew): the reactor will never see this fd's
                // handback, so close the fd here to avoid a write-side leak.
                // The fd is still armed in the poller (we no longer del on
                // dispatch), so the reactor will get a Readable edge for it;
                // on the next event it reads, gets EBADF/0, and
                // close_connection cleans up the Connection entry + poller
                // registration. The double-close is harmless.
                ::close(fd);
                return;
            }
            // Wake the reactor so it drains the outbox promptly instead of
            // waiting for the next poller timeout (which would add up to
            // 100ms of latency per keep-alive request).
            poller_->wakeup();
        });
    }

    // Attempt a non-blocking write of [data.data()+off, data.data()+size).
    // Returns the number of bytes written (>=0), or SIZE_MAX on a hard error
    // (caller closes the fd). Does NOT busy-spin on EAGAIN — returns the count
    // written so far so the caller can park the remainder.
    static std::size_t try_write(int fd, const std::string& data,
                                 std::size_t off) {
        std::size_t total = off;
        while (total < data.size()) {
            ssize_t w = ::write(fd, data.data() + total, data.size() - total);
            if (w > 0) {
                total += static_cast<std::size_t>(w);
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;  // send buffer full; park the remainder
            }
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return static_cast<std::size_t>(-1);  // hard error
        }
        return total;
    }

    // Begin writing a fresh response on the reactor thread. Writes as much as
    // possible non-blockingly; on a partial write, parks the remainder on the
    // connection and arms Writable so the loop drains it later.
    void begin_write(Connection& conn, std::string response, bool keep_alive) {
        conn.write_buf = std::move(response);
        conn.write_off = 0;
        conn.keep_alive_after_write = keep_alive;
        continue_write(conn);
    }

    // Continue (or start) draining conn.write_buf. Fully written → finish the
    // response (re-arm Readable for keep-alive, else close). Partial → arm
    // Writable. Hard error → close.
    void continue_write(Connection& conn) {
        if (conn.write_off < conn.write_buf.size()) {
            std::size_t wrote = try_write(conn.fd, conn.write_buf, conn.write_off);
            if (wrote == static_cast<std::size_t>(-1)) {
                conn.writing = false;
                close_connection(conn.fd);
                return;
            }
            // Bytes flowed out → connection is active.
            conn.last_active = std::chrono::steady_clock::now();
            conn.write_off = wrote;
        }
        if (conn.write_off >= conn.write_buf.size()) {
            // Fully drained.
            conn.writing = false;
            conn.write_buf.clear();
            conn.write_off = 0;
            finish_response(conn);
            return;
        }
        // Still have bytes to send: park and arm Writable. Switch interest in
        // ONE poller call (mod) instead of del+add (two calls). Read interest
        // is dropped because the next request can't start until this response
        // finishes.
        conn.writing = true;
        poller_->mod(conn.fd, EventMask::Writable, nullptr);
    }

    // A response finished writing successfully: either close or re-arm for the
    // next request.
    void finish_response(Connection& conn) {
        if (conn.keep_alive_after_write) {
            // A response just completed → refresh the idle deadline so a
            // keep-alive connection isn't swept between requests.
            conn.last_active = std::chrono::steady_clock::now();
            conn.dispatched = false;
            const int fd = conn.fd;  // capture before any erase could invalidate conn
            // Switch interest back to Readable in ONE poller call (mod) instead
            // of del+add. The fd was never del'd on dispatch (Tier 1), so it's
            // still registered — mod is valid here.
            poller_->mod(fd, EventMask::Readable, nullptr);
            // Edge-triggered correctness (two cases):
            //  1. Pipelined bytes read during the PREVIOUS round are sitting in
            //     read_buf — no edge will ever fire for them.
            //  2. Bytes that arrived DURING dispatch fired a Readable edge that
            //     on_client_event swallowed (dispatched==true). That edge is
            //     consumed, so we must pull the data in ourselves.
            // A speculative drain covers case 2; check_buffered_request covers
            // both by scanning whatever ends up in the buffer.
            bool alive = drain_into_buffer(conn);
            // drain_into_buffer may have closed the fd (peer closed / error),
            // invalidating conn (the Connection was erased). Re-lookup by fd;
            // only proceed if the connection still exists.
            if (!alive || connections_.find(fd) == connections_.end()) return;
            check_buffered_request(connections_.at(fd));
        } else {
            close_connection(conn.fd);
        }
    }

    // Speculatively read the socket to EAGAIN into read_buf (no scan, no
    // dispatch). Used by finish_response to recover data whose Readable edge
    // was consumed during dispatch. Returns false on a hard error / peer-closed
    // (caller closes the connection).
    bool drain_into_buffer(Connection& conn) {
        char buf[4096];
        for (;;) {
            ssize_t r = ::read(conn.fd, buf, sizeof(buf));
            if (r > 0) {
                if (!conn.read_buf.append(buf, static_cast<std::size_t>(r))) {
                    close_connection(conn.fd);
                    return false;
                }
                if (conn.read_buf.size() > Connection_max_request()) {
                    close_connection(conn.fd);
                    return false;
                }
                continue;
            }
            if (r == 0) {
                // Client closed during/after the response.
                close_connection(conn.fd);
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close_connection(conn.fd);
            return false;
        }
        return true;
    }

    // If read_buf already contains a complete "\r\n\r\n"-terminated request,
    // dispatch it without waiting for a poller event. This is mandatory under
    // edge-triggered I/O: bytes already consumed from the socket won't produce
    // a new readiness edge. Returns true if a request was dispatched.
    bool check_buffered_request(Connection& conn) {
        if (conn.dispatched) return false;
        if (conn.read_buf.size() < 4) return false;
        const char* base = static_cast<const char*>(conn.read_buf.data());
        std::size_t n = conn.read_buf.size();
        for (std::size_t i = 0; i + 4 <= n; ++i) {
            if (base[i] == '\r' && base[i + 1] == '\n' &&
                base[i + 2] == '\r' && base[i + 3] == '\n') {
                conn.read_buf.set_scan_pos(i + 4);
                dispatch_to_worker(conn.fd, conn);
                return true;
            }
        }
        return false;
    }

    void drain_outbox() {
        WorkerHandback hb;
        while (outbox_.try_pop(hb)) {
            auto it = connections_.find(hb.fd);
            if (it == connections_.end()) continue;
            Connection& conn = it->second;
            if (hb.response.empty() && !hb.keep_alive) {
                // Handler failure (no response, no keep-alive): just close.
                close_connection(hb.fd);
                continue;
            }
            // Begin the (possibly async) write of the response on this thread.
            // begin_write writes non-blockingly, parking the remainder on conn
            // + arming Writable if the send buffer fills.
            begin_write(conn, std::move(hb.response), hb.keep_alive);
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
    // --- DoS hardening (reactor-thread-only) ---
    std::chrono::seconds idle_timeout_;        // 0 = sweep disabled
    std::size_t max_connections_;              // 0 = cap disabled
    std::size_t ip_rate_per_sec_;              // 0 = rate limit disabled
    // Per-IP blacklist (connection flooders). Decayed by sweep_ip_rate so a
    // one-off offender is auto-unbanned. Sized for ~10k distinct offenders.
    CountingBloomFilter<std::uint32_t> ip_blacklist_{10000, 0.001};
    // Sliding-window new-connection counters (per IP, 1s window). Pruned by
    // sweep_ip_rate. Small (only IPs active in the last 2s are present).
    std::unordered_map<std::uint32_t, RateWindow> ip_connect_windows_;
    std::chrono::steady_clock::time_point last_ip_decay_;
};

// A pool of independent Reactor shards sharing one listen fd. SO_REUSEPORT
// (set in TCPListener) lets the kernel hand each incoming connection to exactly
// one shard; that shard owns the fd for its whole lifetime — it appears in that
// shard's connections_, is served by that shard's worker pool, and its response
// handback routes back through that shard's outbox. No cross-shard routing or
// locking is needed.
//
// Each shard has its own poller, MemoryPool, outbox, connections map, and a
// private worker pool (workers_per_shard threads). This is the multi-core
// scaling story: N shards → N reactor threads doing I/O in parallel, with N×W
// worker threads for compute. For static-file serving (near-zero compute) the
// win is the N parallel I/O loops; the per-shard worker count can stay small.
class ReactorPool {
public:
    // shard_count reactors; each runs workers_per_shard worker threads. The
    // DoS-hardening knobs are forwarded to every shard (each enforces them
    // independently against the connections it owns).
    ReactorPool(int listen_fd, RequestHandler handler,
                unsigned shard_count, unsigned workers_per_shard,
                std::size_t outbox_capacity = 1024,
                std::chrono::seconds idle_timeout = std::chrono::seconds(30),
                std::size_t max_connections = 10000,
                std::size_t ip_rate_per_sec = 0)
    : listen_fd_(listen_fd),
      handler_(std::move(handler)),
      workers_per_shard_(workers_per_shard == 0 ? 1 : workers_per_shard),
      outbox_capacity_(outbox_capacity) {
        if (shard_count == 0) shard_count = 1;
        shards_.reserve(shard_count);
        for (unsigned i = 0; i < shard_count; ++i) {
            // Each shard gets its own Reactor with an independent worker pool.
            // The RequestHandler is copied into each shard (it's a
            // std::function; the typical handler is stateless/captures by ref
            // to long-lived state, so copying is cheap and safe).
            shards_.push_back(std::make_unique<Reactor>(
                listen_fd_, handler_, workers_per_shard_, outbox_capacity_,
                idle_timeout, max_connections, ip_rate_per_sec));
        }
    }

    ~ReactorPool() { stop(); }

    ReactorPool(const ReactorPool&) = delete;
    ReactorPool& operator=(const ReactorPool&) = delete;

    void start() {
        if (started_.exchange(true)) return;
        for (auto& shard : shards_) shard->start();
        MUSES_INFO(std::format("ReactorPool started: {} shards x {} workers",
                               shards_.size(), workers_per_shard_));
    }

    void stop() {
        if (!started_.exchange(false)) return;
        // Stop in reverse order (cosmetic; each shard is independent).
        for (auto it = shards_.rbegin(); it != shards_.rend(); ++it) {
            (*it)->stop();
        }
    }

    std::size_t shard_count() const { return shards_.size(); }

private:
    int listen_fd_;
    RequestHandler handler_;
    unsigned workers_per_shard_;
    std::size_t outbox_capacity_;
    std::vector<std::unique_ptr<Reactor>> shards_;
    std::atomic<bool> started_{false};
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
        // SO_REUSEPORT lets multiple sockets (one per reactor shard) bind the
        // same port and each accept independently; the kernel load-balances
        // incoming connections across them. Required for the multi-reactor
        // (sharded) deployment. Failure here is non-fatal — single-reactor use
        // works without it — but log so a missing-shard scenario is diagnosable.
        if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            MUSES_WARNING(std::format("TCPListener: SO_REUSEPORT failed (multi-reactor will not work): {}",
                                      std::strerror(errno)));
        }
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
