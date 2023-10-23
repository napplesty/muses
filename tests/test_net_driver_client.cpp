#include <ostream>
#include <string>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void tcp_client(std::string ip, int port) {
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    std::cout << "hh" << std::endl;
    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8864);
    serverAddr.sin_addr.s_addr = inet_addr(ip.c_str());

    int connection = connect(client_socket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    std::cout << connection << ' ' << client_socket << std::endl;

    std::string msg = "hello from client";
    int sendResult = send(client_socket, msg.c_str(), msg.size(), 0);
    if (sendResult < 0) {
        std::cout << "failed" << std::endl;
    } else {
        std::cout << "send: " << sendResult << std::endl;
    }
    close(client_socket);
}

int main() {
    for(int i = 0; i < 10; i++) {
        tcp_client("127.0.0.1", 8864);
    }
    return 0;
}