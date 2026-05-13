#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#define ctrl_port 2100
#define MAX_NUM 5

int main() {
    int listenfd, connfd;
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        std::cerr << "socket error\n";
        return 1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctrl_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind erro\n";
        return 1;
    }

    if (listen(listenfd, MAX_NUM) < 0) {
        std::cerr << "listen erro\n";
        return 1;
    }

    std::cout << "FTP server listening on port 2100...\n";

    while (1) {
        connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            std::cerr << "accept error\n";
            continue;
        }
        std::cout << "Client Connect!\n";

        std::string masg = "220 Welcome to Simple FTP Server\r\n";
        send(connfd, masg.c_str(), masg.size(), 0);
        close(connfd);
    }
    close(listenfd);
    return 0;
}