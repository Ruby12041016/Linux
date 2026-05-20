#include <arpa/inet.h>
#include <dirent.h>  //包含struct dirent结构体类型和alphasort函数
#include <errno.h>
#include <fcntl.h>
// #include <fcntl.h>
#include <grp.h>  //包含struct group,getgrgid
#include <limits.h>
#include <netinet/in.h>
#include <pwd.h>  //包含struct paswd,getpwgid
#include <sys/epoll.h>
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
#define MAX_SIZE 10000

ThreadPool pool(THREAD_MAX);

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

struct Client {
    int cli_fd;
    bool nowuser = false;
    bool islogin = false;
    std::string username;
    std::mutex mutex;
    int pasvfd = -1;
    int datafd = -1;
    std::string readbuf;
    std::string writebuf;
    bool quit = false;
};

std::mutex clients_mutex;
std::map<int, Client> clients;
struct epoll_event events[MAX_SIZE];

int filter(const struct dirent* entry) {
    if ((entry->d_name[0] == '.' || strcmp(entry->d_name, "..") == 0)) {
        return 0;
    }
    return 1;
}

void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int epfd = -1;
void epoll_add(int fd, uint32_t events) {
    struct epoll_event envent;
    envent.events = events;
    envent.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &envent);
}

void epoll_delete(int fd, uint32_t events) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

void epoll_mod(int fd, uint32_t events) {
    struct epoll_event envent;
    envent.events = events;
    envent.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &envent);
}

void epoll_write(int fd) {
    ssize_t n;
    std::lock_guard<std::mutex> lock(clients[fd].mutex);
    while (!clients[fd].writebuf.empty()) {
        n = send(fd, clients[fd].writebuf.data(), clients[fd].writebuf.size(),
                 0);
        if (n > 0) {
            clients[fd].writebuf.erase(0, n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close(fd);
            {
                std::lock_guard<std::mutex> map_lock(clients_mutex);
                clients.erase(fd);
            }
            epoll_delete(fd, 0);
            return;
        }
    }
    if (clients[fd].writebuf.empty()) {
        if (clients[fd].quit) {  // QUIT 后所有数据已发完，关闭连接
            close(fd);
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients.erase(fd);
            epoll_delete(fd, 0);
            return;
        }
        epoll_mod(fd, EPOLLET | EPOLLIN);
    } else {
        epoll_mod(fd, EPOLLIN | EPOLLET | EPOLLOUT);
    }
}

std::vector<std::string> do_list_dir(const char* path) {
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

void do_pasv(int connfd) {
    int datalfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in data_addr{};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = 0;
    data_addr.sin_addr.s_addr = INADDR_ANY;

    bind(datalfd, (sockaddr*)&data_addr, sizeof(data_addr));
    listen(datalfd, 1);
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].pasvfd = datalfd;
    }

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
    sprintf(massg, "227 entering passive mode (%d,%d,%d,%d,%d,%d)", ip0, ip1,
            ip2, ip3, p1, p2);
    std::string mass(massg);
    mass += "\r\n";
    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += mass;
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    // 在子线程里阻塞等待客户端连接
    int datafd = accept(datalfd, NULL, NULL);
    close(datalfd);  // 不再需要监听 fd

    // 保存数据连接
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].datafd = datafd;
    }
}

bool check_us(const std::string& name, const std::string& pass) {
    static std::map<std::string, std::string> users;
    static bool logined = false;
    if (!logined) {
        std::ifstream file("account.txt");
        std::string massage;
        while (std::getline(file, massage)) {
            auto wei = massage.find(':');
            if (wei != std::string::npos) {
                users[massage.substr(0, wei)] = massage.substr(wei + 1);
            }
        }
        logined = true;
        if (users.empty()) {
            users["ruby"] = "1024";
        }
    }
    auto it = users.find(name);
    return (it != users.end() && it->second == pass);
}

void do_list(int connfd) {
    int datafd = -1;
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        auto it = clients.find(connfd);
        if (it == clients.end())
            return;
        datafd = it->second.datafd;
    }

    if (datafd < 0) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "425 No data connection\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "150 List directory!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    char path[PATH_MAX];
    getcwd(path, sizeof(path));
    std::vector<std::string> document = do_list_dir(path);
    std::string mag;
    for (int i = 0; i < document.size(); i++) {
        mag += document[i] + "\r\n";
    }

    // 通过数据连接发送目录列表，而不是控制连接
    send(datafd, mag.c_str(), mag.size(), 0);

    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "226 List compare!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    close(datafd);
    std::lock_guard<std::mutex> map_lock(clients_mutex);
    clients[connfd].datafd = -1;
}

void do_retr(int connfd, const std::vector<std::string>& commonds) {
    int datafd = -1;
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        auto it = clients.find(connfd);
        if (it == clients.end())
            return;
        datafd = it->second.datafd;
    }

    if (datafd < 0) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "425 No data connection\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "150 Retr directory!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    std::string filename = commonds[1];
    char path[PATH_MAX];
    getcwd(path, sizeof(path));
    std::string fullpath = std::string(path) + "/" + filename;

    FILE* fp = fopen(fullpath.c_str(), "rb");
    if (!fp) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "550 File not found\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }
    int file_fp = fileno(fp);

    struct stat st;
    fstat(file_fp, &st);
    off_t offset = 0;
    while (offset < st.st_size) {
        ssize_t sent = sendfile(datafd, file_fp, &offset, st.st_size - offset);
        if (sent <= 0)
            break;
    }
    fclose(fp);
    close(datafd);

    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].datafd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "226 Retr compare!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }
}

void do_stor(int connfd, const std::vector<std::string>& commonds) {
    int datafd = -1;
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        auto it = clients.find(connfd);
        if (it == clients.end())
            return;
        datafd = it->second.datafd;
    }

    if (datafd < 0) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "425 No data connection\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "150 STOR directory!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    std::string filename = commonds[1];
    char path[PATH_MAX];
    getcwd(path, sizeof(path));
    std::string fullpath = std::string(path) + "/" + filename;

    std::ofstream fp(fullpath, std::ios::binary);
    if (!fp) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "550 Cannot create file\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }
    char buf[BUF_SIZE];
    while (1) {
        int len = recv(datafd, buf, sizeof(buf), 0);
        if (len <= 0)
            break;
        fp.write(buf, len);
    }
    fp.close();
    close(datafd);

    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].datafd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "226 Stor compare!\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }
}

void epoll_read(int connfd) {
    char buf[BUF_SIZE];
    while (1) {
        memset(buf, 0, sizeof(buf));
        int n = recv(connfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients[connfd].readbuf.append(buf, n);
        } else if (n == 0) {
            std::cout << "Client disconnect\n";
            close(connfd);
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients.erase(connfd);
            epoll_delete(connfd, 0);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  // 读完所有数据
            else
                return;  // 出错直接返回
        }

        // 提取所有完整行（加锁）
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            std::string& rbuf = clients[connfd].readbuf;
            size_t pos;
            while ((pos = rbuf.find("\r\n")) != std::string::npos) {
                lines.push_back(rbuf.substr(0, pos));
                rbuf.erase(0, pos + 2);
            }
        }  // 释放锁

        // 处理每一行命令（不加锁，避免死锁）
        for (const auto& line : lines) {
            std::cout << "Client:" << line << std::endl;
            std::vector<std::string> commonds = split_cmd(line, ' ');
            if (commonds.empty())
                continue;

            // 登录状态检查（需要访问 clients，所以加锁）
            {
                std::lock_guard<std::mutex> map_lock(clients_mutex);
                if (commonds[0] != "USER" && commonds[0] != "PASS" &&
                    commonds[0] != "QUIT" && !clients[connfd].islogin) {
                    std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                    clients[connfd].writebuf += "530 Not logged in\r\n";
                    epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
                    continue;
                }
            }

            if (commonds[0] == "USER") {
                std::lock_guard<std::mutex> map_lock(clients_mutex);
                std::string user = (commonds.size() > 1) ? commonds[1] : "";
                clients[connfd].username = user;
                clients[connfd].nowuser = !user.empty();
                std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                clients[connfd].writebuf +=
                    "331 User name ok, need password\r\n";
                epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
            } else if (commonds[0] == "PASS") {
                std::lock_guard<std::mutex> map_lock(clients_mutex);
                std::string pass = (commonds.size() > 1) ? commonds[1] : "";
                std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                if (check_us(clients[connfd].username, pass)) {
                    clients[connfd].islogin = true;
                    clients[connfd].writebuf += "230 User logged in\r\n";
                } else {
                    clients[connfd].writebuf += "530 Password incorrect\r\n";
                }
                epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
            } else if (commonds[0] == "QUIT") {
                std::lock_guard<std::mutex> map_lock(clients_mutex);
                std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                clients[connfd].writebuf += "221 Goodbye\r\n";
                epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
                clients[connfd].quit = true;
            } else if (commonds[0] == "PASV") {
                pool.enqueue([connfd] { do_pasv(connfd); });
            } else if (commonds[0] == "LIST") {
                pool.enqueue([connfd] { do_list(connfd); });
            } else if (commonds[0] == "RETR" && commonds.size() >= 2) {
                pool.enqueue([connfd, commonds] { do_retr(connfd, commonds); });
            } else if (commonds[0] == "STOR" && commonds.size() >= 2) {
                pool.enqueue([connfd, commonds] { do_stor(connfd, commonds); });
            } else {
                std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                clients[connfd].writebuf += "502 Command not Found\r\n";
                epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
            }
        }
    }
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
    set_nonblock(listenfd);
    epfd = epoll_create1(0);
    // listenfd 的值可能 ≥ 150（MAX_SIZE），直接用作数组下标会导致越界写入。
    //修正：用独立的 epoll_event 变量
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    std::cout << "FTP server listening on port " << ctrl_port << std::endl;
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_SIZE, -1);
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;
            if (fd == listenfd) {
                while (1) {
                    sockaddr_in cliAddr;
                    socklen_t cliLen = sizeof(cliAddr);
                    connfd = accept(listenfd, (sockaddr*)&cliAddr, &cliLen);
                    if (connfd == -1) {
                        break;
                    }
                    set_nonblock(connfd);
                    {
                        std::lock_guard<std::mutex> map_lock(clients_mutex);
                        clients[connfd].cli_fd = connfd;
                    }
                    {
                        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                        clients[connfd].writebuf +=
                            "220 Welcome to My FTP Server!\r\n";
                        epoll_add(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
                    }
                    std::cout
                        << "New Client Connect: " << inet_ntoa(cliAddr.sin_addr)
                        << std::endl;
                }
            } 
            if (events[i].events & EPOLLIN) {
                epoll_read(fd);
            } 
            if (events[i].events & EPOLLOUT) {
                epoll_write(fd);
            }
        }
    }
    close(epfd);
    close(listenfd);
    return 0;
}

/*1. 客户端 → 控制连接 → PASV
2. 服务器 → 创建数据socket → bind → listen → 获取端口 → 发送227响应
3. 客户端 → 使用227响应中的IP和端口 → 连接数据端口
4. 服务器 → accept() → 建立datafd
5. 客户端 → 控制连接 → RETR/LIST/STOR
6. 服务器 → 使用已建立的datafd传输数据*/