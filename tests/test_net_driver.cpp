#include "muses/net_driver.hpp"
#include "muses/logging.hpp"
#include <functional>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class NullContext {
int x = 1;
};

// std::function<bool (context_class *, int connected_fd)>
bool NullHandler(NullContext *ctx, int cfd) {
    char buffer[512];
    int read_num = read(cfd, buffer, sizeof(buffer));
    if(read_num == 0) {
        return true;
    }
    std::stringstream ss;
    ss << "client " << cfd << ':' << buffer << std::endl;
    std::cout << ss.str() << std::endl;
    MUSES_INFO(ss.str());
    std::string s = "greet from server";
    int sendResult = send(cfd, s.c_str(), s.size(), 0);
    return false;
}

int main() {
    muses::TCPListener listen_handler("127.0.0.1", 8864);
    std::function<bool (NullContext *, int connected_fd)> f(NullHandler);
    muses::KqueueConnectionHandler<NullContext>  kqManager(50, 4);
    kqManager.init(listen_handler.get_listener(), f);
    while(true) {}
    return 0;
}