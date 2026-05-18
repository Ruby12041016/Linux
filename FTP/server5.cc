#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>  //包含struct dirent结构体类型和alphasort函数
#include <errno.h>
//#include <fcntl.h>
#include <grp.h>  //包含struct group,getgrgid
#include <limits.h>
#include <netinet/in.h>
#include <pwd.h>  //包含struct paswd,getpwgid
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>   //struct stat
#include <sys/types.h>  //struct stat,getpwgid
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "thread_pool.h"

#define ctrl_port 2100
#define MAX_NUM 5
#define BUF_SIZE 1024
#define THREAD_MAX 10

static void delete_(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
}

std::vector<std::string> split_cmd(const std::string& s, char flag) {
    std::vector<std::string> results;
    std::stringstream ss(s);
    std::string result;
    while (std::getline(ss, result, flag)) {
        delete_(result);
        if (!result.empty())
            results.push_back(result);
    }
    return results;
}

int filter(const struct dirent* entry) {
    if ((entry->d_name[0] == '.' || strcmp(entry->d_name, "..") == 0)) {
        return 0;
    }
    return 1;
}

std::vector<std::string> do_list(const char* path) {
    struct dirent** namelist;
    std::vector<std::string> results;
    int n = scandir(path, &namelist, filter, NULL);
    if (n == -1) {
        std::cout << " Scan error!\n";
        return results;
    }
    for (int i = 0; i < n; i++) {
        results.push_back(namelist[i]->d_name);
        free(namelist[i]);
    }
    free(namelist);
    return results;
}

bool check_us(const std::string& name, const std::string& pass){
    static std::map<std::string, std::string> users;
    static bool logined = false;
    if(!logined){
        std::ifstream file("account.txt");
        std::string massage;
        while(std::getline(file,massage)){
            auto wei = massage.find(':');
            if(wei!=std::string::npos){
                users[massage.substr(0, wei)] = massage.substr(wei + 1);
            }
        }
        logined = true;
        if(massage.empty()){
            users["ruby"] = "1024";
        }
    }
    auto it = users.find(name);
    return (it != users.end() && it->second == pass);
}

void handle_client(int connfd) {
    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    int datafd = -1;
    std::string now_user;

    std::string hello = "220 Welcome to Simple FTP Server!\r\n";
    send(connfd, hello.c_str(), hello.size(), 0);

    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(connfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            std::cout << "Client disconnect\n";
            break;
        }
        std::string cmd(buf, n);
        std::cout << "Client:" << cmd;
        std::vector<std::string> commonds = split_cmd(cmd, ' ');

        if (commonds.empty())
            continue;

        if (commonds[0] == "USER") {
            now_user = commonds.size() > 1 ? commonds[1] : "";
            std::string answer = "331 User name okay, need password\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
        } else if (commonds[0] == "PASS") {
            std::string pass = commonds.size() > 1 ? commonds[1] : "";
            if (check_us(now_user, pass)) {
                std::string answer = "230 User logged in\r\n";
                send(connfd, answer.c_str(), answer.size(), 0);
            } else {
                std::string answer = "530 Login incorrect\r\n";
                send(connfd, answer.c_str(), answer.size(), 0);
                break;  
            }
        } else if (commonds[0] == "QUIT") {
            std::string answer = "221 Goodbye\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            close(datafd);
            datafd = -1;
            break;
        } else if (commonds[0] == "PASV") {
            int datalfd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in data_addr{};
            data_addr.sin_family = AF_INET;
            data_addr.sin_port = 0;
            data_addr.sin_addr.s_addr = INADDR_ANY;

            bind(datalfd, (sockaddr*)&data_addr, sizeof(data_addr));
            listen(datalfd, 1);

            socklen_t len = sizeof(data_addr);
            getsockname(datalfd, (struct sockaddr*)&data_addr, &len);
            unsigned short port = ntohs(data_addr.sin_port);
            int p1 = port / 256;
            int p2 = port % 256;

            struct sockaddr_in local_addr;
            socklen_t local_len = sizeof(local_addr);
            getsockname(connfd, (struct sockaddr*)&local_addr, &local_len);
            uint32_t ip = ntohl(local_addr.sin_addr.s_addr);
            int ip0 = (ip >> 24) & 0xFF;
            int ip1 = (ip >> 16) & 0xFF;
            int ip2 = (ip >> 8) & 0xFF;
            int ip3 = ip & 0xFF;

            char massg[BUF_SIZE];
            sprintf(massg, "227 entering passive mode (%d,%d,%d,%d,%d,%d)", ip0,
                    ip1, ip2, ip3, p1, p2);
            std::string mass(massg);
            mass += "\r\n";
            send(connfd, mass.c_str(), mass.size(), 0);

            datafd = accept(datalfd, NULL, NULL);
            close(datalfd);
        } else if (commonds[0] == "LIST") {
            if (datafd < 0) {
                std::string answer = "425 No data connection\r\n";
                send(connfd, answer.c_str(), answer.size(), 0);
                continue;
            }
            std::string answer = "150 List directory!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            char path[PATH_MAX];
            getcwd(path, sizeof(path));
            std::vector<std::string> document = do_list(path);
            std::string mag;
            for (int i = 0; i < document.size(); i++) {
                mag += document[i] + "\r\n";
            }
            send(datafd, mag.c_str(), mag.size(), 0);

            answer = "226 List compare!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            close(datafd);
            datafd = -1;

        } else if (commonds[0] == "RETR" && commonds.size() >= 2) {
            if (datafd < 0) {
                std::string answer = "425 No data connection\r\n";
                send(connfd, answer.c_str(), answer.size(), 0);
                continue;
            }

            std::string answer = "150 Retr directory!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            std::string filename = commonds[1];

            char path[PATH_MAX];
            getcwd(path, sizeof(path));
            std::string fullpath = std::string(path) + "/" + filename;

            char file[BUF_SIZE];
            FILE* fp = fopen(fullpath.c_str(), "rb");
            if (!fp) {
                std::string error = "550 File not found\r\n";
                send(connfd, error.c_str(), error.size(), 0);
                continue;
            }
            int file_fp = fileno(fp);

            struct stat st;
            fstat(file_fp, &st);
            off_t offset = 0;
            while (offset < st.st_size) {
                ssize_t sent =
                    sendfile(datafd, file_fp, &offset, st.st_size - offset);
                if (sent <= 0)
                    break;
            }
            fclose(fp);
            close(datafd);
            answer = "226 Retr compare!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            datafd = -1;
        } else if (commonds[0] == "STOR" && commonds.size() >= 2) {
            if (datafd < 0) {
                std::string answer = "425 No data connection\r\n";
                send(connfd, answer.c_str(), answer.size(), 0);
                continue;
            }
            std::string answer = "150 STOR directory!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            std::string filename = commonds[1];

            char path[PATH_MAX];
            getcwd(path, sizeof(path));
            std::string fullpath = std::string(path) + "/" + filename;

            char file[BUF_SIZE];
            std::ofstream fp(fullpath, std::ios::binary);
            if (!fp) {
                std::string error = "550 Cannot create file\r\n";
                send(connfd, error.c_str(), error.size(), 0);
                continue;
            }
            while (1) {
                int len = recv(datafd, file, sizeof(file), 0);
                if (len <= 0) {
                    break;
                }
                fp.write(file, len);
            }
            fp.close();
            close(datafd);
            answer = "226 Stor compare!\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
            datafd = -1;
        } else {
            std::string answer = "502 Command not Found\r\n";
            send(connfd, answer.c_str(), answer.size(), 0);
        }
    }
    close(connfd);
    std::cout << "[Client Disconnected]\n";
}

int main() {
    int listenfd, connfd;
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctrl_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, MAX_NUM);

    ThreadPool pool(THREAD_MAX);

    std::cout << "FTP server listening on port " << ctrl_port << std::endl;

    while (1) {
        sockaddr_in cliAddr;
        socklen_t cliLen = sizeof(cliAddr);
        connfd = accept(listenfd, (sockaddr*)&cliAddr, &cliLen);
        if (connfd < 0)
            continue;
        std::cout << "New Client Connect: " << inet_ntoa(cliAddr.sin_addr)
                  << std::endl;

        pool.enqueue([connfd] { handle_client(connfd); });
    }
    close(listenfd);
    return 0;
}

/*1. 客户端 → 控制连接 → PASV
2. 服务器 → 创建数据socket → bind → listen → 获取端口 → 发送227响应
3. 客户端 → 使用227响应中的IP和端口 → 连接数据端口
4. 服务器 → accept() → 建立datafd
5. 客户端 → 控制连接 → RETR/LIST/STOR
6. 服务器 → 使用已建立的datafd传输数据*/