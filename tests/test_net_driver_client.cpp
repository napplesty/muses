#include <ostream>
#include <string>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>
#include <future>
#include "muses/profiler.hpp"
#include "muses/thread_pool.hpp"

void tcp_client(std::string ip, int port) {
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8864);
    serverAddr.sin_addr.s_addr = inet_addr(ip.c_str());

    int connection = connect(client_socket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));

    std::string msg = "hello from client";
    int sendResult = send(client_socket, msg.c_str(), msg.size(), 0);
    if (sendResult < 0) {
        std::cout << "failed" << std::endl;
    } else {
        std::cout << "send: " << msg.c_str() << std::endl;
    }
    char buffer[512];
    int read_num = read(client_socket, buffer, sizeof(buffer));
    std::stringstream ss;
    ss << "recv: " << buffer << std::endl;
    std::cout << ss.str() << std::endl;
    close(client_socket);
}

void thread_func(size_t id) {
    tcp_client("127.0.0.1", 8864);
}

int main() {
    muses::ThreadPool pool(32);
    std::vector<std::future<void> > results;
    for (int i = 0; i < 1000; i++) {
        results.emplace_back(pool.enqueue(thread_func, i));
    }
    for (auto&& result: results) {
        result.get();
    }
    return 0;
}