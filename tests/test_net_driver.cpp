#include "muses/net_driver.hpp"
#include <functional>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class NullContext {

};

// std::function<bool (context_class *, int connected_fd)>
bool NullHandler(NullContext *ctx, int cfd) {
    char buffer[512];
    int read_num = read(cfd, buffer, sizeof(buffer));
    std::cout << "from " << cfd << ':' << buffer << std::endl;
    exit(0);
    return false;
}

int main() {
    muses::TCPListener listen_handler("127.0.0.1", 8864);
    std::function<bool (NullContext *, int connected_fd)> f(NullHandler);
    muses::KqueueConnectionHandler<NullContext>  kqManager(50, 4, f);
    kqManager.init_muxer(listen_handler.get_listener());
    while(true) {}
    return 0;
}