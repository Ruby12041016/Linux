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

#define ctrl_port 2100  //自定义控制连接的端口号
#define MAX_NUM 5  //listen的backlog参数，表示监听队列的最大长度，最多同时处理5个待处理的连接
#define BUF_SIZE 1024  //读写缓冲区的大小，和客户端一致
#define THREAD_MAX 10  //线程池的最大线程数
#define MAX_SIZE 10000  //epoll的最大事件数，最多同时处理10000个事件，也就是最多支持10000个客户端连接

ThreadPool pool(THREAD_MAX);

//清理字符串末尾换行符
static void delete_(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
}

//分割命令参数，和客户端类似
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

//客户端状态结构体
struct Client {
    int cli_fd;  //客户端的控制连接fd
    bool nowuser = false;  //是否已经输入了用户名，等待密码
    bool islogin = false;  //是否已经登录成功
    std::string username;  //客户端的用户名
    std::mutex mutex;  //保护这个客户端状态的互斥锁，避免多线程同时修改这个客户端的状态导致数据竞争
    int pasvfd = -1;  //被动模式的监听fd，服务端收到PASV命令后会创建的监听socket
    int datafd = -1;  //数据连接的fd，和客户端传输数据用的连接
    std::string readbuf;  //读缓冲区，非阻塞IO下，保存没读完的客户端数据
    std::string writebuf;  //写缓冲区，非阻塞IO下，保存没发完的响应数据
    bool quit = false;  //是否已经收到QUIT命令，准备关闭连接
};

//保护clients map的互斥锁，多个线程可能同时访问这个map，所以需要加锁
std::mutex clients_mutex;
//存储所有客户端的map，键是客户端的控制连接fd，值是客户端的状态结构体
std::map<int, Client> clients;
//epoll的事件数组，用于存储 epoll_wait返回的就绪事件
struct epoll_event events[MAX_SIZE];


//这个是scandir的过滤函数，用于过滤掉不需要显示的文件
int filter(const struct dirent* entry) {
    if ((entry->d_name[0] == '.' || strcmp(entry->d_name, "..") == 0)) {
        return 0;
    }
    return 1;
}

//将文件描述符设置为非阻塞模式
void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int epfd = -1;

//封装epoll的添加函数，将fd添加到epoll的监听列表，监听指定的事件
void epoll_add(int fd, uint32_t events) {
    struct epoll_event envent;
    envent.events = events;
    envent.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &envent);
}

//封装epoll的删除函数，将fd从epoll的监听列表中删除
void epoll_delete(int fd, uint32_t events) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

//封装epoll的修改函数，修改fd的监听事件
void epoll_mod(int fd, uint32_t events) {
    struct epoll_event envent;
    envent.events = events;
    envent.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &envent);
}

//处理epoll的可写事件，也就是把writebuf里没发完的数据发送出去
void epoll_write(int fd) {
    ssize_t n;
    //加锁，保护这个客户端的状态，避免多线程同时修改
    std::lock_guard<std::mutex> lock(clients[fd].mutex);
    //循环发送writebuf里的数据，直到缓冲区空了
    while (!clients[fd].writebuf.empty()) {
        n = send(fd, clients[fd].writebuf.data(), clients[fd].writebuf.size(), 0);
        //如果发送成功，把已经发出去的数据从缓冲区里删掉
        if (n > 0) {
            clients[fd].writebuf.erase(0, n);
        } else if (n < 0) {  //如果发送返回EAGAIN，说明内核发送缓冲区满了，暂时发不出去，break等下次可写的时候再发
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            //如果是其他错误，说明连接断开了
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
        if (clients[fd].quit) {  //QUIT后所有数据已发完，关闭连接
            close(fd);
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients.erase(fd);
            epoll_delete(fd, 0);
            return;
        }
        epoll_mod(fd, EPOLLET | EPOLLIN);
    } else {  //修改epoll的事件，只监听读事件，因为没有数据要发了
        epoll_mod(fd, EPOLLIN | EPOLLET | EPOLLOUT);
    }
}


std::vector<std::string> do_list_dir(const char* path) {
    struct dirent** namelist;
    std::vector<std::string> results;  //把所有的文件名存入结果数组
    int n = scandir(path, &namelist, filter, NULL);
    if (n == -1) {
        std::cout << " Scan error!\n";
        return results;
    }
    for (int i = 0; i < n; i++) {
        results.push_back(namelist[i]->d_name);
        free(namelist[i]);
    }
    free(namelist);  //释放scandir分配的内存，返回文件列表
    return results;
}

// 处理客户端的PASV命令，这个函数丢到线程池里异步执行，因为里面的accept是阻塞的，不能阻塞主线程的epoll循环
void do_pasv(int connfd) {
    int datalfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in data_addr{};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = 0;  //端口设为0，系统会自动分配一个可用的随机端口
    data_addr.sin_addr.s_addr = INADDR_ANY;  //地址设为INADDR_ANY，监听所有地址
    //绑定地址，然后listen，backlog设为1，因为只需要接受一个客户端的连接
    bind(datalfd, (sockaddr*)&data_addr, sizeof(data_addr));
    listen(datalfd, 1);
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].pasvfd = datalfd;  //把这个监听fd保存到客户端的状态里
    }

    socklen_t len = sizeof(data_addr);
    getsockname(datalfd, (struct sockaddr*)&data_addr, &len);
    unsigned short port = ntohs(data_addr.sin_port);
    int p1 = port / 256;
    int p2 = port % 256;
    //获取控制连接的本地IP，也就是服务端的IP，然后把IP拆成四个8位的整数
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    getsockname(connfd, (struct sockaddr*)&local_addr, &local_len);
    uint32_t ip = ntohl(local_addr.sin_addr.s_addr);
    int ip0 = (ip >> 24) & 0xFF;
    int ip1 = (ip >> 16) & 0xFF;
    int ip2 = (ip >> 8) & 0xFF;
    int ip3 = ip & 0xFF;
    //拼成PASV的响应
    char massg[BUF_SIZE];
    sprintf(massg, "227 entering passive mode (%d,%d,%d,%d,%d,%d)", ip0, ip1,
            ip2, ip3, p1, p2);
    std::string mass(massg);
    mass += "\r\n";
    //把响应加到客户端的写缓冲区，修改epoll的事件
    {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += mass;
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    //在子线程里阻塞等待客户端连接
    int datafd = accept(datalfd, NULL, NULL);
    close(datalfd);  //不再需要监听 fd

    //保存数据连接
    {
        std::lock_guard<std::mutex> map_lock(clients_mutex);
        clients[connfd].datafd = datafd;
    }
}

//验证用户密码
bool check_us(const std::string& name, const std::string& pass) {
    static std::map<std::string, std::string> users;
    static bool logined = false;
    if (!logined) {
        //读取account.txt文件
        std::ifstream file("account.txt");
        std::string massage;
        while (std::getline(file, massage)) {
            auto wei = massage.find(':');
            if (wei != std::string::npos) {
                users[massage.substr(0, wei)] = massage.substr(wei + 1);
            }
        }
        logined = true;
        //如果文件不存在或者为空，就创建一个默认的用户ruby，密码1024
        if (users.empty()) {
            users["ruby"] = "1024";
        }
    }
    auto it = users.find(name);
    return (it != users.end() && it->second == pass);
}

//处理客户端的LIST命令，列出目录
void do_list(int connfd) {
    int datafd = -1;
    {
        //获取客户端的数据连接fd
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
    //扫描目录的文件列表，拼成字符串，每个文件名一行
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
    //关闭数据连接，把客户端的datafd重置为-1，因为数据连接用完就可以关了
}

//处理RETR命令
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
    // char path[PATH_MAX];
    // getcwd(path, sizeof(path));
    // std::string fullpath = std::string(path) + "/" + filename;

    FILE* fp = fopen(filename.c_str(), "rb");  //打开要下载的文件
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
        // 用sendfile零拷贝系统调用
        ssize_t sent = sendfile(datafd, file_fp, &offset, st.st_size - offset);
        if (sent <= 0)
            break;
    }
    fclose(fp);
    close(datafd);

    //关闭文件和数据连接，重置datafd
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

//处理客户端的STOR命令
void do_stor(int connfd, const std::vector<std::string>& commonds) {
    int datafd = -1;
    //获取数据连接的fd
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

    //创建本地文件，保存客户端上传的文件
    std::ofstream fp(fullpath, std::ios::binary);
    if (!fp) {
        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
        clients[connfd].writebuf += "550 Cannot create file\r\n";
        epoll_mod(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
        return;
    }
    char buf[BUF_SIZE];
    //循环从数据连接读取客户端发来的文件数据，写入本地文件，直到读完所有数据
    while (1) {
        int len = recv(datafd, buf, sizeof(buf), 0);
        if (len <= 0)
            break;
        fp.write(buf, len);
    }
    fp.close();
    close(datafd);

    //关闭文件和数据连接，重置datafd
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
        //循环读取数据，因为是边缘触发，所以要一次性把所有可读的数据都读完，不然就不会再触发事件了
        memset(buf, 0, sizeof(buf));
        int n = recv(connfd, buf, sizeof(buf) - 1, 0);
        // 读取成功的话，把数据加到客户端的读缓冲区，因为非阻塞，可能一次读不完一个完整的命令，所以要存起来
        if (n > 0) {
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients[connfd].readbuf.append(buf, n);
        } else if (n == 0) {
            //n=0说明客户端关闭了连接，输出提示，关闭fd，清理客户端的状态
            std::cout << "Client disconnect\n";
            close(connfd);
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            clients.erase(connfd);
            epoll_delete(connfd, 0);
            return;
        } else { //如果返回 EAGAIN，说明所有数据都读完了
                if (errno == EAGAIN ||
                    errno == EWOULDBLOCK) break;  // 读完所有数据
            else
                return;  // 出错直接返回
        }

        // 提取所有完整行（加锁）
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> map_lock(clients_mutex);
            std::string& rbuf = clients[connfd].readbuf;
            size_t pos;
            // ftp的命令都是以\r\n结尾的，所以按这个分割，把完整的命令行拿出来，剩下的不完整的留在缓冲区里，等下次数据到了再处理
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
            //处理每个完整的命令行，分割成参数
            if (commonds[0] == "USER") {
                // 检查登录状态：如果客户端还没登录，除了USER、PASS、QUIT这三个命令，其他命令都拒绝
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
            } else if (commonds[0] == "PASV") {//处理 PASV 命令：把do_pasv任务丢到线程里
                pool.enqueue([connfd] { do_pasv(connfd); });
            } else if (commonds[0] == "LIST") {//处理LIST命令：丢到线程池异步执行
                pool.enqueue([connfd] { do_list(connfd); });
            } else if (commonds[0] == "RETR" && commonds.size() >= 2) {
                //处理RETR命令：丢到线程池异步执行
                pool.enqueue([connfd, commonds] { do_retr(connfd, commonds); });
            } else if (commonds[0] == "STOR" && commonds.size() >= 2) {
                //处理STOR命令：丢到线程池异步执行
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
    //初始化服务端的地址，绑定2100端口，监听所有地址
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctrl_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    //绑定地址，然后开始监听
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, MAX_NUM);
    //把监听fd设为非阻塞
    set_nonblock(listenfd);
    epfd = epoll_create1(0);
    // listenfd 的值可能 ≥ 150（MAX_SIZE），直接用作数组下标会导致越界写入。
    //修正：用独立的 epoll_event 变量
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    //把监听fd添加到epoll，监听读事件，也就是新连接的事件
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);

    std::cout << "FTP server listening on port " << ctrl_port << std::endl;
    while (1) {  //进入epoll的循环，等待事件，-1表示无限等待，直到有事件发生
        int nfds = epoll_wait(epfd, events, MAX_SIZE, -1);
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;
            if (fd == listenfd) {  //遍历所有就绪的事件，如果是监听fd的事件，说明有新的客户端连接
                while (1) {
                    //循环accept所有待处理的新连接，因为是边缘触发，所以要一次性把所有的新连接都处理完
                    sockaddr_in cliAddr;
                    socklen_t cliLen = sizeof(cliAddr);
                    connfd = accept(listenfd, (sockaddr*)&cliAddr, &cliLen);
                    if (connfd == -1) {
                        break;
                    }
                    //把新连接的fd设为非阻塞，初始化客户端的状态
                    set_nonblock(connfd);
                    {
                        std::lock_guard<std::mutex> map_lock(clients_mutex);
                        clients[connfd].cli_fd = connfd;
                    }
                    {
                        std::lock_guard<std::mutex> lock(clients[connfd].mutex);
                        clients[connfd].writebuf +=
                            "220 Welcome to My FTP Server!\r\n";
                        //把这个新的fd添加到epoll，监听读写事件，边缘触发
                        epoll_add(connfd, EPOLLIN | EPOLLOUT | EPOLLET);
                    }
                    std::cout
                        << "New Client Connect: " << inet_ntoa(cliAddr.sin_addr)
                        << std::endl;
                }
            }
            // 如果是读事件，调用epoll_read 处理，如果是写事件，调用epoll_write处理
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