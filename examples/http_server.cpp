// Example: a minimal static-file HTTP server built on the muses reactor.
//
// Run from the build directory (where tests/statics has been copied):
//   ./muses_http_server
// Env vars:
//   MUSES_WORKERS  number of worker threads (default 4)
//   MUSES_PORT     listen port (default 8864)
// Then: curl http://127.0.0.1:8864/

#include "muses/logging.hpp"
#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <atomic>
#include <cstdlib>
#include <iostream>

static std::atomic<bool> g_stop{false};

int main() {
    std::signal(SIGINT, [](int) { g_stop.store(true); });

    unsigned workers = 4;
    if (const char* w = std::getenv("MUSES_WORKERS")) workers = static_cast<unsigned>(std::atoi(w));
    unsigned short port = 8864;
    if (const char* p = std::getenv("MUSES_PORT")) port = static_cast<unsigned short>(std::atoi(p));

    muses::TCPListener listener("127.0.0.1", port);
    int lfd = listener.get_listener();
    if (lfd < 0) {
        std::cerr << "failed to listen\n";
        return 1;
    }

    muses::Reactor reactor(lfd, [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return muses::HandlerResult{std::move(hr.response), hr.keep_alive};
    }, workers);
    reactor.start();

    std::cout << "serving ./statics on http://127.0.0.1:" << port
              << "/ (workers=" << workers << ", Ctrl-C to quit)\n";
    while (!g_stop.load()) {
        sleep(1);
    }

    reactor.stop();
    return 0;
}

