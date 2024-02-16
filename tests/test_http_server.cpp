#include "muses/net_driver.hpp"
#include "muses/logging.hpp"
#include "muses/net_components/http_handler.hpp"
#include <functional>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class NullContext {
    int x;
};

// std::function<bool (context_class *, int connected_fd)>
bool NullHandler(NullContext *ctx, int cfd) {
    return muses::HttpContext::handle_request(cfd);
}

int main() {
    muses::TCPListener listen_handler("127.0.0.1", 8864);
    std::function<bool (NullContext *, int connected_fd)> f(NullHandler);
    muses::ConnectionHandler<NullContext>  Manager(50, 4);
    Manager.init(listen_handler.get_listener(), f);
    while(true) {sleep(1);}
    return 0;
}