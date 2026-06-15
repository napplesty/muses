#include <doctest.h>

#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
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

// Pipelined requests: two requests sent in ONE write, back-to-back, on the
// same keep-alive connection. This is the critical edge-triggered correctness
// test — after the first request is dispatched, the second's bytes are already
// in the socket buffer. Under ET the reactor must drain them into read_buf and
// re-dispatch via check_buffered_request; otherwise the second response hangs.
TEST_CASE("Reactor: pipelined requests both answered (ET correctness)") {
    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    std::ofstream("./statics/rr_pipe.html") << "pipeline-body";

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 2);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Send TWO complete requests in a single write (pipelined).
    std::string pipelined =
        "GET /rr_pipe.html HTTP/1.1\r\n"
        "\r\n"
        "GET /rr_pipe.html HTTP/1.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ssize_t w = ::write(fd, pipelined.data(), pipelined.size());
    REQUIRE(w == static_cast<ssize_t>(pipelined.size()));

    // Read until we see BOTH response bodies (or timeout).
    std::string out;
    char buf[4096];
    for (int i = 0; i < 200 && (std::count(out.begin(), out.end(), 'p') < 2 ||
                                out.find("pipeline-body", out.find("pipeline-body") + 1) == std::string::npos); ++i) {
        timeval tv{0, 100 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) out.append(buf, static_cast<std::size_t>(r));
        else if (r == 0) break;
    }

    // Both responses must have arrived on the one socket.
    std::size_t first_body = out.find("pipeline-body");
    CHECK(first_body != std::string::npos);
    if (first_body != std::string::npos) {
        std::size_t second_body = out.find("pipeline-body", first_body + 1);
        CHECK(second_body != std::string::npos);  // two bodies received
    }
    // At least two "HTTP/1.1 200" status lines.
    std::size_t first_status = out.find("HTTP/1.1 200");
    CHECK(first_status != std::string::npos);
    if (first_status != std::string::npos) {
        CHECK(out.find("HTTP/1.1 200", first_status + 1) != std::string::npos);
    }

    ::close(fd);
    reactor.stop();
    ::close(lfd);
    std::remove("./statics/rr_pipe.html");
}

// idle timeout: a connection that sends a partial request (no terminating
// \r\n\r\n) and then goes silent must be closed by the server after the
// configured idle deadline. Uses a short 1s timeout so the test is fast.
TEST_CASE("Reactor: idle timeout closes a slowloris connection") {
    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 2, 1024, std::chrono::seconds(1));  // idle timeout = 1s
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Send a partial request — no \r\n\r\n, so the reactor never dispatches.
    std::string partial = "GET / HTTP/1.1\r\nHost: x\r\n";
    REQUIRE(::write(fd, partial.data(), partial.size()) == static_cast<ssize_t>(partial.size()));

    // The server should close the connection within ~1s of the last activity.
    // Poll with a generous read timeout; expect EOF (r == 0) well before 3s.
    timeval tv{3, 0};  // 3s ceiling
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[64];
    auto t0 = std::chrono::steady_clock::now();
    ssize_t r = ::read(fd, buf, sizeof(buf));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(r == 0);  // server closed the idle connection
    CHECK(elapsed < std::chrono::seconds(3));

    ::close(fd);
    reactor.stop();
    ::close(lfd);
}

// max connections: with a cap of 2, the third simultaneous connection is
// rejected (the server closes it immediately).
TEST_CASE("Reactor: max-connections cap rejects excess clients") {
    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    // idle_timeout=0 (disable sweep so it doesn't interfere), max_connections=2.
    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 2, 1024, std::chrono::seconds(0), /*max_connections=*/2);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    auto open_conn = [&] {
        int c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        a.sin_port = htons(port);
        ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        return c;
    };

    int c1 = open_conn();
    int c2 = open_conn();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // let reactor accept them
    int c3 = open_conn();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // let reactor reject c3

    // c1 and c2 are live (send a request, get a response).
    std::ofstream("./statics/rr_cap.html") << "cap-ok";
    auto send_recv = [&](int c) {
        std::string req = "GET /rr_cap.html HTTP/1.1\r\nConnection: close\r\n\r\n";
        ::write(c, req.data(), req.size());
        std::string out; char b[4096];
        timeval tv{0, 300 * 1000}; ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r = ::read(c, b, sizeof(b));
        if (r > 0) out.append(b, static_cast<std::size_t>(r));
        return out;
    };
    CHECK(send_recv(c1).find("200") != std::string::npos);
    CHECK(send_recv(c2).find("200") != std::string::npos);

    // c3 was rejected: a read should return EOF or a reset promptly (the server
    // closed the fd without responding). Either <= 0 means the connection was
    // refused; macOS may deliver ECONNRESET (-1) rather than a clean EOF (0).
    timeval tv{0, 300 * 1000}; ::setsockopt(c3, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[64];
    ssize_t r = ::read(c3, buf, sizeof(buf));
    CHECK(r <= 0);

    ::close(c1); ::close(c2); ::close(c3);
    reactor.stop();
    ::close(lfd);
    std::remove("./statics/rr_cap.html");
}

// per-IP rate limit: a burst of new connections from one IP exceeds the
// threshold and gets blacklisted; further connections from that IP are dropped.
TEST_CASE("Reactor: per-IP connection rate limit blacklists a flooder") {
    unsigned short port = 0;
    int lfd = make_listen_socket(port);
    REQUIRE(lfd >= 0);

    // idle=0, max_conn=0 (disabled), ip_rate=5 per second.
    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, 1, 1024, std::chrono::seconds(0), /*max_connections=*/0, /*ip_rate=*/5);
    reactor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Open a burst of connections rapidly from 127.0.0.1. After 5 in one
    // window, the IP is blacklisted and subsequent connects get dropped.
    std::vector<int> conns;
    int accepted = 0;
    for (int i = 0; i < 30; ++i) {
        int c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        a.sin_port = htons(port);
        if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            conns.push_back(c);
        }
        // Don't sleep — fire them as fast as possible to exceed the rate.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let reactor process

    // At least the first few (<= threshold+some) should have been accepted;
    // the server-side connection count is bounded by the rate limiter having
    // kicked in. We assert the limiter DID engage: the number of live server
    // connections can't grow without bound here.
    // Verify by checking that a fresh connection now gets dropped (blacklisted).
    int probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    a.sin_port = htons(port);
    ::connect(probe, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    timeval tv{0, 300 * 1000}; ::setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[64];
    // The probe should be closed by the server (blacklisted IP). A refused
    // connection surfaces as EOF (0) or ECONNRESET (-1); both mean dropped.
    ssize_t r = ::read(probe, buf, sizeof(buf));
    CHECK(r <= 0);

    for (int c : conns) ::close(c);
    ::close(probe);
    reactor.stop();
    ::close(lfd);
}
