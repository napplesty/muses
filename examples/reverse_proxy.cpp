// Example: a reverse proxy built on the muses coroutine infrastructure.
//
// Hardcoded routing table (edit and rebuild to change). Each client connection
// is a coroutine that forwards requests to the matched upstream, reusing pooled
// keep-alive upstream connections. Upstream I/O suspends on the poller instead
// of blocking a worker — this is what muses/task.hpp enables.
//
// Run from the build directory:
//   ./muses_reverse_proxy
// Env vars:
//   MUSES_PROXY_PORT   listen port (default 8865)
// Then point a client at it: curl http://127.0.0.1:8865/

#include "muses/logging.hpp"
#include "muses/net/reactor.hpp"   // for TCPListener (SO_REUSEPORT)
#include "muses/net_components/proxy.hpp"

#include <csignal>
#include <atomic>
#include <cstdlib>
#include <iostream>

static std::atomic<bool> g_stop{false};

int main() {
    std::signal(SIGINT, [](int) { g_stop.store(true); });

    unsigned short port = 8865;
    if (const char* p = std::getenv("MUSES_PROXY_PORT")) port = static_cast<unsigned short>(std::atoi(p));

    // Hardcoded routing table. First matching prefix wins; "/" is the catch-all.
    // Edit these and rebuild to route to your backends.
    std::vector<muses::ProxyRoute> routes = {
        {"/api/", "127.0.0.1", 8081},
        {"/",     "127.0.0.1", 8082},
    };

    // Print the route table BEFORE moving it into the proxy.
    std::cout << "reverse proxy on http://127.0.0.1:" << port << "/\n";
    std::cout << "routes:\n";
    for (const auto& r : routes) {
        std::cout << "  " << r.prefix << " -> " << r.host << ":" << r.port << "\n";
    }
    std::cout << "(Ctrl-C to quit)\n";

    muses::TCPListener listener("127.0.0.1", port);
    auto lfd_result = listener.get_listener();
    if (!lfd_result) {
        std::cerr << "failed to listen: " << lfd_result.error() << "\n";
        return 1;
    }
    int lfd = *lfd_result;

    muses::ProxyServer proxy(lfd, std::move(routes), /*max_retries=*/2);
    proxy.start();
    while (!g_stop.load()) {
        sleep(1);
    }

    proxy.stop();
    return 0;
}
