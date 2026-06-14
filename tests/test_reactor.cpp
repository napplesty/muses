#include <doctest.h>

#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

// Create a non-blocking listening socket bound to an ephemeral port on
// 127.0.0.1. Returns the fd (or -1) and fills `port`.
int make_listen_socket(unsigned short& port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = 0;  // ephemeral
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return -1;
    }
    if (::listen(fd, 16) < 0) { ::close(fd); return -1; }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// Connect to 127.0.0.1:port and send `req`; read the full response with a
// timeout. Returns "" if connect/read fails.
std::string http_roundtrip(unsigned short port, const std::string& req,
                           std::chrono::milliseconds timeout) {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return "";
    }
    if (::write(fd, req.data(), req.size()) != static_cast<ssize_t>(req.size())) {
        ::close(fd); return "";
    }
    std::string out;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 100 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) {
            out.append(buf, static_cast<std::size_t>(r));
            // Heuristic: stop once we have the full body length.
            auto he = out.find("Content-Length:");
            if (he != std::string::npos) {
                auto le = out.find("\r\n", he);
                std::size_t len = std::stoul(out.substr(he + 15, le - (he + 15)));
                auto body_start = out.find("\r\n\r\n");
                if (body_start != std::string::npos &&
                    out.size() - (body_start + 4) >= len) {
                    break;
                }
            }
        } else if (r == 0) {
            break;  // server closed
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }
    ::close(fd);
    return out;
}

}  // namespace

TEST_CASE("Reactor: serves a static file over a real socket") {
    std::ofstream f("./statics/rr_index.html", std::ios::binary | std::ios::trunc);
    f << "reactor-live";
    f.close();

    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 4);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::string resp = http_roundtrip(port,
        "GET /rr_index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        std::chrono::milliseconds(2000));
    CHECK(resp.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(resp.find("reactor-live") != std::string::npos);

    reactor.stop();
    ::close(lfd);
    std::remove("./statics/rr_index.html");
}

TEST_CASE("Reactor: concurrent clients all get responses") {
    std::ofstream f("./statics/rr_cc.html", std::ios::binary | std::ios::trunc);
    f << "concurrent-ok";
    f.close();

    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 4);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    constexpr int N = 10;
    int got = 0;
    std::vector<std::thread> clients;
    for (int i = 0; i < N; ++i) {
        clients.emplace_back([&port, &got] {
            std::string resp = http_roundtrip(port,
                "GET /rr_cc.html HTTP/1.1\r\nConnection: close\r\n\r\n",
                std::chrono::milliseconds(3000));
            if (resp.find("concurrent-ok") != std::string::npos) ++got;
        });
    }
    for (auto& t : clients) t.join();
    CHECK(got == N);

    reactor.stop();
    ::close(lfd);
    std::remove("./statics/rr_cc.html");
}

TEST_CASE("Reactor: 404 for unknown path") {
    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 2);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::string resp = http_roundtrip(port,
        "GET /does_not_exist_zzz HTTP/1.1\r\nConnection: close\r\n\r\n",
        std::chrono::milliseconds(2000));
    CHECK(resp.rfind("HTTP/1.1 404", 0) == 0);

    reactor.stop();
    ::close(lfd);
}

TEST_CASE("Reactor: keep-alive reuses one socket for two requests") {
    std::ofstream f("./statics/rr_keep.html", std::ios::binary | std::ios::trunc);
    f << "keep-alive-body";
    f.close();

    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 2);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Open a single socket, send two pipelined-ish requests sequentially with
    // keep-alive. Both must be answered on the same connection.
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    auto send_and_recv = [&](const std::string& req) {
        // Give the reactor a moment to (re-)arm the fd between requests.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::write(fd, req.data(), req.size());
        std::string out;
        char buf[4096];
        for (int i = 0; i < 100 && out.find("\r\n\r\n") == std::string::npos; ++i) {
            timeval tv{0, 100 * 1000};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0) out.append(buf, static_cast<std::size_t>(r));
            else if (r == 0) break;
        }
        return out;
    };

    std::string r1 = send_and_recv("GET /rr_keep.html HTTP/1.1\r\n\r\n");
    std::string r2 = send_and_recv("GET /rr_keep.html HTTP/1.1\r\n\r\n");
    CHECK(r1.find("200") != std::string::npos);
    CHECK(r2.find("200") != std::string::npos);
    // Two response bodies on the same socket.
    std::size_t first = r1.find("keep-alive-body");
    CHECK(first != std::string::npos);
    CHECK(r2.find("keep-alive-body") != std::string::npos);

    ::close(fd);
    reactor.stop();
    ::close(lfd);
    std::remove("./statics/rr_keep.html");
}
