// Example: a minimal static-file HTTP server built on the muses reactor.
//
// Run from the build directory (where tests/statics has been copied):
//   ./muses_http_server
// Env vars:
//   MUSES_REACTORS  number of reactor shards (default: hardware_concurrency())
//   MUSES_WORKERS   worker threads PER shard (default 4)
//   MUSES_PORT      listen port (default 8864)
// Then: curl http://127.0.0.1:8864/
//
// Multi-reactor: each shard is an independent reactor thread with its own
// poller, worker pool, and connection map, all sharing the one listen fd via
// SO_REUSEPORT (the kernel hands each incoming connection to exactly one
// shard). This is how muses scales across cores.

#include "muses/logging.hpp"
#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

static std::atomic<bool> g_stop{false};

int main() {
    std::signal(SIGINT, [](int) { g_stop.store(true); });

    unsigned reactors = std::thread::hardware_concurrency();
    if (reactors == 0) reactors = 4;
    if (const char* r = std::getenv("MUSES_REACTORS")) reactors = static_cast<unsigned>(std::atoi(r));
    unsigned workers_per_shard = 4;
    if (const char* w = std::getenv("MUSES_WORKERS")) workers_per_shard = static_cast<unsigned>(std::atoi(w));
    unsigned short port = 8864;
    if (const char* p = std::getenv("MUSES_PORT")) port = static_cast<unsigned short>(std::atoi(p));
    // DoS-hardening knobs. Defaults are safe for real traffic; tune for your
    // load. 0 disables the corresponding check.
    unsigned idle_timeout_s = 30;
    if (const char* t = std::getenv("MUSES_IDLE_TIMEOUT")) idle_timeout_s = static_cast<unsigned>(std::atoi(t));
    std::size_t max_connections = 10000;
    if (const char* m = std::getenv("MUSES_MAX_CONNECTIONS")) max_connections = static_cast<std::size_t>(std::strtoull(m, nullptr, 10));
    std::size_t ip_rate_per_sec = 0;  // off by default (localhost benchmarks share one IP)
    if (const char* ir = std::getenv("MUSES_IP_RATE")) ip_rate_per_sec = static_cast<std::size_t>(std::strtoull(ir, nullptr, 10));

    muses::TCPListener listener("127.0.0.1", port);
    auto lfd_result = listener.get_listener();
    if (!lfd_result) {
        std::cerr << "failed to listen: " << lfd_result.error() << "\n";
        return 1;
    }
    int lfd = *lfd_result;

    muses::ReactorPool pool(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, reactors, workers_per_shard, 1024,
       std::chrono::seconds(idle_timeout_s), max_connections, ip_rate_per_sec);
    pool.start();

    std::cout << "serving ./statics on http://127.0.0.1:" << port
              << "/ (reactors=" << pool.shard_count()
              << ", workers/shard=" << workers_per_shard
              << ", idle_timeout=" << idle_timeout_s << "s"
              << ", max_conn=" << max_connections
              << ", ip_rate=" << ip_rate_per_sec << "/s"
              << ", Ctrl-C to quit)\n";
    while (!g_stop.load()) {
        sleep(1);
    }

    pool.stop();
    return 0;
}
