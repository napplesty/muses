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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "muses/logging.hpp"
#include "muses/net/poller_factory.hpp"
#include "muses/net_components/http_handler.hpp"
#include "muses/task.hpp"

#ifndef MUSES_NET_PROXY_HPP
#define MUSES_NET_PROXY_HPP

namespace muses {

// An upstream backend: host:port plus prefix that routes to it. The first
// matching prefix (in declaration order) wins.
struct ProxyRoute {
    std::string prefix;   // URL path prefix, e.g. "/" or "/api/"
    std::string host;     // upstream host, e.g. "127.0.0.1"
    unsigned short port;  // upstream port
};

// Health state for one upstream (identified by host:port). A failed connect or
// upstream error marks it unhealthy for a cooldown period so the proxy stops
// hammering a dead backend.
struct UpstreamHealth {
    bool healthy = true;
    std::chrono::steady_clock::time_point unhealthy_since{};
};

// A coroutine-driven reverse proxy. Each client connection is a coroutine that
// reads the request, matches a route, forwards to the upstream (reusing a
// pooled keep-alive connection), reads the response, and writes it back to the
// client. Upstream I/O suspends on the poller instead of blocking a worker —
// this is what the muses/task.hpp coroutine infrastructure enables.
//
// Not a generalization of the static-file Reactor: a dedicated event loop here
// treats both client and upstream fds as first-class connections driven by
// coroutines. The static-file Reactor is untouched.
class ProxyServer {
public:
    // routes: prefix → upstream, evaluated in order. listen_fd must already be
    // bound/listening (use TCPListener). max_retries: attempts per request
    // before giving up with 502. upstream_keepalive: pool idle connections?
    ProxyServer(int listen_fd, std::vector<ProxyRoute> routes,
                unsigned max_retries = 2,
                std::chrono::seconds upstream_cooldown = std::chrono::seconds(10))
    : listen_fd_(listen_fd),
      routes_(std::move(routes)),
      max_retries_(max_retries),
      upstream_cooldown_(upstream_cooldown) {
        set_nonblocking(listen_fd_);
        // Seed health state for each distinct upstream.
        for (const auto& r : routes_) {
            health_[upstream_key(r.host, r.port)];
        }
    }

    ~ProxyServer() { stop(); }

    ProxyServer(const ProxyServer&) = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        poller_ = make_poller();
        if (!poller_ || !poller_->add(listen_fd_, EventMask::Readable, nullptr)) {
            MUSES_ERROR("Proxy: failed to add listen fd");
            running_.store(false);
            return;
        }
        loop_thread_ = std::thread(&ProxyServer::loop, this);
        MUSES_INFO("ProxyServer started");
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (poller_) poller_->wakeup();
        if (loop_thread_.joinable()) loop_thread_.join();
        // Destroy all live client coroutines (they may be suspended).
        for (auto& [fd, task] : live_tasks_) {
            (void)fd;
        }
        live_tasks_.clear();
        // Close any pooled upstream fds.
        for (auto& [key, pool] : pools_) {
            for (int fd : pool) ::close(fd);
        }
        pools_.clear();
        if (poller_) poller_->del(listen_fd_);
        poller_.reset();
    }

private:
    static void set_nonblocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static std::string upstream_key(const std::string& host, unsigned short port) {
        return std::format("{}:{}", host, port);
    }

    // --- Event loop (single thread; coroutines resume here only) -----------

    void loop() {
        constexpr int kMaxEvents = 128;
        PollEvent events[kMaxEvents];
        while (running_.load(std::memory_order_acquire)) {
            int n = poller_->wait(events, 100);
            if (n < 0) {
                if (errno == EINTR) continue;
                MUSES_ERROR("Proxy: poller wait failed");
                continue;
            }
            for (int i = 0; i < n; ++i) {
                const PollEvent& ev = events[i];
                if (ev.fd == listen_fd_ && ev.userdata == nullptr) {
                    accept_clients();
                    continue;
                }
                // A ready fd whose userdata is a coroutine handle: resume it.
                if (ev.userdata != nullptr) {
                    auto h = handle_from_event(ev);
                    if (h) resume_handle(h, ev);
                }
            }
            reap_finished_tasks();
        }
    }

    // Resume a coroutine handle. The fd that fired is deregistered BEFORE
    // resuming: we've consumed this readiness edge, and the coroutine will
    // re-register interest (via IoAwaiter) on its next co_await. Deregistering
    // before (not after) resume is critical — after resume the coroutine may
    // already have re-registered the same fd for a different event (e.g.
    // Writable→Readable on an upstream), and a post-resume del would wipe
    // that fresh registration, deadlocking the request.
    void resume_handle(std::coroutine_handle<> h, const PollEvent& ev) {
        if (ev.fd != listen_fd_) {
            poller_->del(ev.fd);
        }
        h.resume();
    }

    // Accept new client connections and spawn a coroutine for each.
    void accept_clients() {
        for (int accepted = 0; accepted < 128; ++accepted) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }
            set_nonblocking(fd);
            int ok = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ok, sizeof(ok));
            // Spawn the client coroutine. It will run until its first suspension
            // (awaiting readable data), registering interest with the poller.
            auto task = serve_client(this, fd);
            // Initial resume: runs until first co_await.
            bool done = task.resume();
            int tfd = fd;
            if (done) {
                // Completed synchronously (e.g. immediate error); drop it.
                ::close(fd);
            } else {
                live_tasks_[tfd] = std::move(task);
            }
        }
    }

    // Remove finished coroutines (their frames are destroyed by the Task dtor).
    void reap_finished_tasks() {
        for (auto it = live_tasks_.begin(); it != live_tasks_.end();) {
            if (it->second.done()) {
                it = live_tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // --- Coroutine I/O helpers (all suspend on the poller) ----------------

    // Read from `fd` until `delimiter` is found in the accumulated buffer.
    // Returns the full buffer up to and including the delimiter. On EOF/error
    // returns empty. The fd must be non-blocking; this drains to EAGAIN.
    static Task<std::string> read_until(ProxyServer* self, int fd,
                                        std::string delimiter) {
        std::string buf;
        char chunk[4096];
        for (;;) {
            // Try a non-blocking read first; only await if EAGAIN.
            ssize_t r = ::read(fd, chunk, sizeof(chunk));
            if (r > 0) {
                buf.append(chunk, static_cast<std::size_t>(r));
                if (buf.find(delimiter) != std::string::npos) {
                    co_return buf;
                }
                continue;  // keep draining (ET)
            }
            if (r == 0) { co_return buf; }  // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Need more data — await readable.
                IoAwaiter aw{self->poller_.get(), fd, EventMask::Readable};
                co_await aw;
                continue;
            }
            if (errno == EINTR) continue;
            co_return buf;  // hard error
        }
    }

    // Read exactly `n` bytes from `fd`. Returns the bytes (may be short on EOF).
    static Task<std::string> read_n(ProxyServer* self, int fd, std::size_t n) {
        std::string buf;
        buf.reserve(n);
        char chunk[4096];
        while (buf.size() < n) {
            std::size_t want = n - buf.size();
            ssize_t r = ::read(fd, chunk, want < sizeof(chunk) ? want : sizeof(chunk));
            if (r > 0) {
                buf.append(chunk, static_cast<std::size_t>(r));
                continue;
            }
            if (r == 0) { co_return buf; }  // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                IoAwaiter aw{self->poller_.get(), fd, EventMask::Readable};
                co_await aw;
                continue;
            }
            if (errno == EINTR) continue;
            co_return buf;  // hard error
        }
        co_return buf;
    }

    // Write all of `data` to `fd`, awaiting Writable on EAGAIN. Returns true on
    // full write, false on hard error.
    static Task<bool> write_all_async(ProxyServer* self, int fd,
                                      const std::string& data) {
        std::size_t off = 0;
        while (off < data.size()) {
            ssize_t w = ::write(fd, data.data() + off, data.size() - off);
            if (w > 0) {
                off += static_cast<std::size_t>(w);
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                IoAwaiter aw{self->poller_.get(), fd, EventMask::Writable};
                co_await aw;
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            co_return false;  // hard error
        }
        co_return true;
    }

    // Open a non-blocking connection to host:port and await writability (which
    // signals connect completion). Returns the fd (>=0) on success, -1 on
    // failure/timeout.
    static Task<int> connect_upstream(ProxyServer* self,
                                      const std::string& host, unsigned short port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) co_return -1;
        set_nonblocking(fd);
        int ok = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ok, sizeof(ok));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::inet_addr(host.c_str());
        addr.sin_port = htons(port);
        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
            co_return fd;  // immediate connect (localhost)
        }
        if (errno != EINPROGRESS) {
            ::close(fd);
            co_return -1;
        }
        // Await writability, then check SO_ERROR for the connect result.
        IoAwaiter aw{self->poller_.get(), fd, EventMask::Writable};
        co_await aw;
        int err = 0;
        socklen_t sl = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) < 0 || err != 0) {
            ::close(fd);
            co_return -1;
        }
        co_return fd;
    }

    // --- Connection pool ---------------------------------------------------

    // Borrow a pooled keep-alive fd for an upstream, or open a new one.
    Task<int> acquire_upstream(const std::string& host, unsigned short port) {
        std::string key = upstream_key(host, port);
        auto it = pools_.find(key);
        if (it != pools_.end() && !it->second.empty()) {
            int fd = it->second.front();
            it->second.pop_front();
            co_return fd;
        }
        co_return co_await connect_upstream(this, host, port);
    }

    // Return a still-open fd to the pool (or close it if the pool is full).
    void release_upstream(const std::string& host, unsigned short port, int fd,
                          bool reusable) {
        if (fd < 0) return;
        if (!reusable) {
            ::close(fd);
            return;
        }
        std::string key = upstream_key(host, port);
        auto& pool = pools_[key];
        if (pool.size() >= 64) {  // cap pool depth
            ::close(fd);
        } else {
            pool.push_back(fd);
        }
    }

    // --- Health ------------------------------------------------------------

    bool is_healthy(const std::string& host, unsigned short port) {
        std::string key = upstream_key(host, port);
        auto it = health_.find(key);
        if (it == health_.end()) return true;
        if (it->second.healthy) return true;
        // Cooldown expired → re-enable (optimistic; a new failure re-marks it).
        if (std::chrono::steady_clock::now() - it->second.unhealthy_since > upstream_cooldown_) {
            it->second.healthy = true;
            return true;
        }
        return false;
    }

    void mark_unhealthy(const std::string& host, unsigned short port) {
        std::string key = upstream_key(host, port);
        auto& h = health_[key];
        h.healthy = false;
        h.unhealthy_since = std::chrono::steady_clock::now();
        MUSES_WARNING(std::format("Proxy: upstream {} marked unhealthy", key));
    }

    // --- Header rewriting --------------------------------------------------

    // Case-insensitive header lookup (HttpContext stores keys verbatim).
    static std::string header_get(const std::map<std::string, std::string>& headers,
                                  const std::string& name) {
        for (const auto& [k, v] : headers) {
            if (iequals(k, name)) return v;
        }
        return "";
    }
    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    // Hop-by-hop headers that must not be forwarded (RFC 7230 §6.1).
    static bool is_hop_by_hop(const std::string& name) {
        static const char* const hop[] = {
            "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
            "te", "trailer", "transfer-encoding", "upgrade"};
        for (const char* h : hop) {
            if (iequals(name, h)) return true;
        }
        return false;
    }

    // --- The client coroutine ---------------------------------------------

    // Main per-connection coroutine. Defined as a free function friend so it
    // can be a coroutine returning Task<void>.
    friend Task<void> serve_client(ProxyServer* self, int client_fd);

    int listen_fd_;
    std::vector<ProxyRoute> routes_;
    unsigned max_retries_;
    std::chrono::seconds upstream_cooldown_;
    std::unique_ptr<Poller> poller_;
    std::thread loop_thread_;
    std::atomic<bool> running_{false};
    // Live client coroutines keyed by client fd. Destroyed when done or on stop.
    std::unordered_map<int, Task<void>> live_tasks_;
    // Keep-alive upstream connection pools keyed by "host:port".
    std::unordered_map<std::string, std::deque<int>> pools_;
    // Health state per upstream.
    std::unordered_map<std::string, UpstreamHealth> health_;
};

// The per-client coroutine: read request → route → forward to upstream → read
// response → write back → (keep-alive) loop. Defined outside the class because
// it's a coroutine and needs to return Task<void>.
inline Task<void> serve_client(ProxyServer* self, int client_fd) {
    for (;;) {  // keep-alive loop
        // Read the request headers (up to \r\n\r\n).
        std::string head = co_await ProxyServer::read_until(self, client_fd, "\r\n\r\n");
        if (head.empty() || head.find("\r\n\r\n") == std::string::npos) {
            co_return;  // client closed or malformed
        }
        // Parse to find method/url and Content-Length for the body.
        muses::HttpInfo info = muses::HttpContext::parse_request(head);
        // Determine the body length (Content-Length; chunked bodies are forwarded
        // as-is with the header — we don't de-chunk on the request side here).
        std::string cl = ProxyServer::header_get(info.headers, "Content-Length");
        std::size_t body_len = 0;
        if (!cl.empty()) {
            try { body_len = static_cast<std::size_t>(std::stoull(cl)); } catch (...) {}
        }
        // Read the request body if any (the head already includes any body bytes
        // that arrived in the same read past \r\n\r\n).
        std::size_t hdr_end = head.find("\r\n\r\n") + 4;
        std::string body_in_head = head.substr(hdr_end);
        head.resize(hdr_end);  // trim to headers only
        std::string body = body_in_head;
        if (body.size() < body_len) {
            std::string rest = co_await ProxyServer::read_n(self, client_fd, body_len - body.size());
            body += rest;
        }

        // Route by prefix (first match wins). Default to the last route (catch-all "/").
        const ProxyRoute* route = nullptr;
        for (const auto& r : self->routes_) {
            if (info.url.rfind(r.prefix, 0) == 0) { route = &r; break; }
        }
        if (route == nullptr && !self->routes_.empty()) route = &self->routes_.back();
        if (route == nullptr) {
            std::string resp = muses::HttpContext::build_response(
                502, "Bad Gateway", "text/plain", "no route", false);
            co_await ProxyServer::write_all_async(self, client_fd, resp);
            co_return;
        }

        // Health gate.
        if (!self->is_healthy(route->host, route->port)) {
            std::string resp = muses::HttpContext::build_response(
                502, "Bad Gateway", "text/plain", "upstream unavailable", false);
            co_await ProxyServer::write_all_async(self, client_fd, resp);
            co_return;
        }

        // Rewrite request headers: strip hop-by-hop, add X-Forwarded-For.
        std::string fwd_request = info.method + " " + info.url + " " + info.version + "\r\n";
        for (const auto& [k, v] : info.headers) {
            if (ProxyServer::is_hop_by_hop(k)) continue;
            if (ProxyServer::iequals(k, "Host")) {
                // Keep the original Host (reverse proxy to the same vhost) or
                // rewrite to upstream. Here we preserve it for transparency.
            }
            fwd_request += k + ": " + v + "\r\n";
        }
        fwd_request += "X-Forwarded-Proto: http\r\n";
        fwd_request += "\r\n";
        fwd_request += body;

        // Forward with retries.
        std::string upstream_response;
        bool got_response = false;
        for (unsigned attempt = 0; attempt <= self->max_retries_ && !got_response; ++attempt) {
            int up_fd = co_await self->acquire_upstream(route->host, route->port);
            if (up_fd < 0) {
                self->mark_unhealthy(route->host, route->port);
                continue;  // retry
            }
            bool wrote = co_await ProxyServer::write_all_async(self, up_fd, fwd_request);
            if (!wrote) {
                ::close(up_fd);
                self->mark_unhealthy(route->host, route->port);
                continue;
            }
            // Read the upstream response headers.
            std::string up_head = co_await ProxyServer::read_until(self, up_fd, "\r\n\r\n");
            if (up_head.empty() || up_head.find("\r\n\r\n") == std::string::npos) {
                ::close(up_fd);
                self->mark_unhealthy(route->host, route->port);
                continue;
            }
            // Parse the response to find body length (Content-Length or chunked).
            muses::HttpInfo up_info = muses::HttpContext::parse_request(up_head);
            std::size_t uhdr_end = up_head.find("\r\n\r\n") + 4;
            upstream_response = up_head;  // keep headers verbatim for now
            std::string up_body_in_head = up_head.substr(uhdr_end);
            upstream_response.resize(uhdr_end);
            std::string up_te = ProxyServer::header_get(up_info.headers, "Transfer-Encoding");
            std::string up_cl = ProxyServer::header_get(up_info.headers, "Content-Length");
            bool chunked = up_te.find("chunked") != std::string::npos;
            if (chunked) {
                // Read chunks until the terminating "0\r\n\r\n".
                std::string chunkbuf = up_body_in_head;
                for (;;) {
                    if (chunkbuf.find("0\r\n\r\n") != std::string::npos) break;
                    // Need more data. Look for a complete chunk size line.
                    if (chunkbuf.find("\r\n") == std::string::npos) {
                        std::string more = co_await ProxyServer::read_until(self, up_fd, "\r\n");
                        chunkbuf += more;
                        continue;
                    }
                    // Parse hex size, read that many bytes + CRLF, repeat.
                    // Simplified: just stream until we see the terminator.
                    std::string more = co_await ProxyServer::read_n(self, up_fd, 4096);
                    if (more.empty()) break;
                    chunkbuf += more;
                    if (chunkbuf.size() > 16 * 1024 * 1024) break;  // safety cap
                }
                upstream_response += chunkbuf;
            } else if (!up_cl.empty()) {
                std::size_t up_body_len = 0;
                try { up_body_len = static_cast<std::size_t>(std::stoull(up_cl)); } catch (...) {}
                std::string up_body = up_body_in_head;
                if (up_body.size() < up_body_len) {
                    std::string rest = co_await ProxyServer::read_n(self, up_fd, up_body_len - up_body.size());
                    up_body += rest;
                }
                upstream_response += up_body;
            } else {
                // No length info: read to EOF (upstream closes).
                std::string rest = up_body_in_head;
                for (;;) {
                    std::string more = co_await ProxyServer::read_n(self, up_fd, 4096);
                    if (more.empty()) break;
                    rest += more;
                    if (rest.size() > 16 * 1024 * 1024) break;
                }
                upstream_response += rest;
            }
            // Success: return the upstream fd to the pool.
            self->release_upstream(route->host, route->port, up_fd, /*reusable=*/true);
            got_response = true;
        }

        if (!got_response) {
            std::string resp = muses::HttpContext::build_response(
                502, "Bad Gateway", "text/plain", "upstream failed", false);
            co_await ProxyServer::write_all_async(self, client_fd, resp);
            co_return;
        }

        // Write the upstream response back to the client.
        bool client_ok = co_await ProxyServer::write_all_async(self, client_fd, upstream_response);
        if (!client_ok) co_return;

        // keep-alive decision: if the client asked to close, stop.
        if (!info.wants_keep_alive()) co_return;
        // Loop to read the next request on the same connection.
    }
}

}  // namespace muses

#endif  // MUSES_NET_PROXY_HPP
