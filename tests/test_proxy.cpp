#include <doctest.h>

#include "muses/net/reactor.hpp"        // TCPListener for the proxy listen fd
#include "muses/net_components/proxy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

// A tiny blocking upstream: accept one connection, read the request, write a
// fixed response, optionally keep the connection open. Runs in its own thread.
// Returns the listening fd (caller closes it) and fills the port.
struct MockUpstream {
    int listen_fd = -1;
    unsigned short port = 0;
    std::thread thr;
    std::atomic<bool> stop{false};
    std::string body = "upstream-ok";
    bool keep_alive = true;

    void start() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        a.sin_port = 0;
        int opt = 1; ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(listen_fd, 8);
        socklen_t l = sizeof(a);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        thr = std::thread([this] {
            while (!stop.load()) {
                int c = ::accept(listen_fd, nullptr, nullptr);
                if (c < 0) continue;
                // Read request (until \r\n\r\n), then respond.
                char buf[4096];
                std::string acc;
                while (acc.find("\r\n\r\n") == std::string::npos) {
                    ssize_t r = ::read(c, buf, sizeof(buf));
                    if (r <= 0) break;
                    acc.append(buf, static_cast<std::size_t>(r));
                }
                std::string conn = keep_alive ? "keep-alive" : "close";
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: " + conn +
                    "\r\nX-Upstream: mock\r\n\r\n" + body;
                ::write(c, resp.data(), resp.size());
                if (!keep_alive) { ::close(c); }
                else {
                    // For keep-alive, keep c open a moment then it's reused by pool.
                    // Simplest: close after a short delay so the next request reopens.
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    ::close(c);
                }
            }
        });
    }

    ~MockUpstream() {
        stop.store(true);
        if (listen_fd >= 0) { ::shutdown(listen_fd, SHUT_RDWR); ::close(listen_fd); }
        if (thr.joinable()) thr.join();
    }
};

// Open a client socket, send a request, read the full response (up to a
// Content-Length body or EOF). Returns the response bytes.
std::string proxy_roundtrip(unsigned short port, const std::string& req) {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd); return "";
    }
    ::write(fd, req.data(), req.size());
    std::string out; char buf[4096];
    // Read until EOF (the requests use Connection: close) with a per-read
    // timeout generous enough to cover the proxy's coroutine startup + upstream
    // round-trip. Up to 60 reads × 200ms = 12s ceiling.
    for (int i = 0; i < 60; ++i) {
        timeval tv{0, 200 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) {
            out.append(buf, static_cast<std::size_t>(r));
        } else if (r == 0) {
            break;  // EOF — server closed (Connection: close path)
        } else {
            // Timeout (EAGAIN) — if we already have headers, that's enough;
            // otherwise keep trying.
            if (!out.empty() && out.find("\r\n\r\n") != std::string::npos) break;
        }
    }
    ::close(fd);
    return out;
}

}  // namespace

TEST_CASE("Proxy: forwards a GET request to the upstream") {
    MockUpstream up;
    up.body = "hello-from-upstream";
    up.start();

    // Create the proxy listen fd.
    int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = 0;
    int opt = 1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(lfd, 8);
    socklen_t l = sizeof(a);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &l);
    unsigned short pport = ntohs(a.sin_port);

    muses::ProxyServer proxy(lfd, {{"", "127.0.0.1", up.port}}, /*retries=*/0);
    proxy.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = proxy_roundtrip(pport,
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    CHECK(resp.find("HTTP/1.1 200") != std::string::npos);
    CHECK(resp.find("hello-from-upstream") != std::string::npos);

    proxy.stop();
    ::close(lfd);
}

TEST_CASE("Proxy: returns 502 when no route matches and none is catch-all") {
    int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = 0;
    int opt = 1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(lfd, 8);
    socklen_t l = sizeof(a);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &l);
    unsigned short pport = ntohs(a.sin_port);

    // Empty route table → 502.
    muses::ProxyServer proxy(lfd, {}, /*retries=*/0);
    proxy.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = proxy_roundtrip(pport,
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(resp.find("502") != std::string::npos);

    proxy.stop();
    ::close(lfd);
}

TEST_CASE("Proxy: returns 502 when upstream is down") {
    int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = 0;
    int opt = 1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(lfd, 8);
    socklen_t l = sizeof(a);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &l);
    unsigned short pport = ntohs(a.sin_port);

    // Point at a closed port (1, no listener) → connect fails → 502.
    muses::ProxyServer proxy(lfd, {{"", "127.0.0.1", 1}}, /*retries=*/1);
    proxy.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = proxy_roundtrip(pport,
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(resp.find("502") != std::string::npos);

    proxy.stop();
    ::close(lfd);
}

TEST_CASE("Proxy: prefix routing selects the right upstream") {
    MockUpstream up_api;  up_api.body = "api-backend";  up_api.start();
    MockUpstream up_root; up_root.body = "root-backend"; up_root.start();

    int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = 0;
    int opt = 1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(lfd, 8);
    socklen_t l = sizeof(a);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &l);
    unsigned short pport = ntohs(a.sin_port);

    muses::ProxyServer proxy(lfd, {
        {"/api/", "127.0.0.1", up_api.port},
        {"/",     "127.0.0.1", up_root.port},
    }, /*retries=*/0);
    proxy.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string r1 = proxy_roundtrip(pport,
        "GET /api/users HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    std::string r2 = proxy_roundtrip(pport,
        "GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    CHECK(r1.find("api-backend") != std::string::npos);
    CHECK(r2.find("root-backend") != std::string::npos);

    proxy.stop();
    ::close(lfd);
}
