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

void handle_client(int connfd) {
    char buf[1024];

    std::string hello = "220 Welcome to Simple FTP Server!\r\n";
    send(connfd, hello.c_str(), hello.size(), 0);

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(connfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            std::cout << "Client disconnect\n";
            break;
        }
        std::string cmd(buf);
        std::cout << "Client:" << cmd;

        if (cmd.substr(0, 4) == "USER") {
            std::string answer = "230 Login OK\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);

        } else if (cmd.substr(0, 4) == "QUIT") {
            std::string answer = "221 Goodbye\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            break;
        } else {
            std::string answer = "502 Command not Found\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
        }
    }
    close(connfd);
}

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

        handle_client(connfd);
    }
    close(listenfd);
    return 0;
}