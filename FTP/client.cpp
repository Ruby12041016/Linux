#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define BUF_SIZE 1024   //读写缓冲区的大小
#define ctrl_port 2100  //自定义控制连接的端口号

int ctrlfd = -1;  //全局的控制连接的文字描述符

//pasv返回的拼接端口号的函数（高八位*256+第八位）
int addport(int p1, int p2) {
    return p1 * 256 + p2;
}

//连接服务器函数，参数是服务端的 IP 地址和端口号，返回连接成功的套接字fd，失败返回-1
int connect_ser(const std::string& ip, int port) {
    int data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd < 0)
        return -1;
    struct sockaddr_in data_addr{};  // 定义服务端的地址结构体，用于存储要连接的服务端的IP和端口信息
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(port);  // 主机字节序转网络字节序
    inet_pton(AF_INET, ip.c_str(),
              &data_addr.sin_addr);  // 将点分十进制的 IP 地址转换为网络字节序的二进制IP地址，存入地址结构体的sin_addr字段。
    if (connect(data_fd, (sockaddr*)&data_addr, sizeof(data_addr)) <
        0) {  // 主动向服务端发起TCP连接
        close(data_fd);
        return -1;
    }
    return data_fd;
}

//发送ftp命令的函数
void send_cmd(const std::string& commond, int fd) {
    std::string cmd =
        commond + "\r\n";  // FTP 协议规定，所有的命令行都必须以\r\n作为行结尾
    int ret = send(fd, cmd.c_str(), cmd.size(), 0);
    if (ret <= 0) {
        std::cerr << "send_cmd failed, connection lost." << std::endl;
        close(ctrlfd);
        ctrlfd = -1;
        return;
    }
    std::cout << cmd << " send!" << std::endl;
}


// 接受服务端响应的函数
std::string recv_ans(int fd) {
    char buf[BUF_SIZE];
    memset(buf, 0,
           sizeof(buf));  //定义读取缓冲区，并用memset初始化为全 0，避免脏数据
    int n = recv(fd, buf, BUF_SIZE - 1, 0);
    if (n <= 0) {
        std::cerr << "recv_ans failed, connection lost." << std::endl;
        close(ctrlfd);
        ctrlfd = -1;
        return "";
    }
    std::string answer(buf, n);
    //去掉响应末尾的\r和\n换行符
    while (!answer.empty() && (answer.back() == '\r' || answer.back() == '\n'))
        answer.pop_back();
    std::cout << "answer:" << answer << std::endl;
    return answer;
}


//本地函数重命名
std::string file_name(std::string file){
    if(!std::filesystem::exists(file)){
        return file;
    }
    std::filesystem::path path(file);
//获取文件名和后缀
    std::string name = path.stem().string();
    std::string hou = path.extension().string();

    int index = 1;
    while (true) {
        std::string new_name = name + "(" + std::to_string(index) + ")" + hou;

        if (!std::filesystem::exists(new_name)) {
            return new_name;
        }

        index++;
    }
}

// 解析服务端返回的 PASV 命令响应
std::pair<std::string, int> do_pasv(const std::string& answer) {
    int l1 = answer.find('(');
    int l2 = answer.find(')');
    if (l1 == std::string::npos || l2 == std::string::npos)
        throw std::runtime_error("Invalid PASV response");
    // 提取括号里的内容，也就是h1,h2,h3,h4,p1,p2
    std::string ip_n = answer.substr(l1 + 1, l2 - l1 - 1);
    std::vector<int> nums;
    std::stringstream ss(ip_n);
    std::string t;
    // 按逗号分割这部分内容，把每个数字转成整数，存入数组
    while (getline(ss, t, ',')) {
        nums.push_back(stoi(t));
    }
    if (nums.size() < 6)
        throw std::runtime_error("Invalid PASV response");
    std::string ip = std::to_string(nums[0]) + "." + std::to_string(nums[1]) +
                     "." + std::to_string(nums[2]) + "." +
                     std::to_string(nums[3]);
    int port = addport(nums[4], nums[5]);
    //返回 IP 和端口组成的键值对 
    return {ip, port};
}

//建立被动模式数据连接
int pasv_conn() {
    //向服务端发送PASV命令，请求进入被动模式
    send_cmd("PASV", ctrlfd);
    if (ctrlfd < 0)
        return -1;
    std::string answer = recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return -1;
    auto [ip, port] = do_pasv(answer);
    // 解析得到的服务端的数据连接地址
    std::cout << "Connect:" << ip << ":" << port << std::endl;
    return connect_ser(ip, port);
}

// 列出服务端目录
void do_ls() {
    int datafd = pasv_conn();  //建立被动模式的数据连接
    if (datafd < 0) {
        std::cerr << "pasv_conn failed" << std::endl;
        return;
    }
    //向服务端发送 LIST 命令，请求列出目录
    send_cmd("LIST", ctrlfd);
    if (ctrlfd < 0) {
        close(datafd);
        return;
    }

    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    int n = recv(datafd, buf, BUF_SIZE - 1, 0);
    if (n > 0)
        std::cout << "LIST:\n" << std::string(buf, n) << std::endl;
    close(datafd);
    //从数据连接读取服务端发来的目录列表并输出
    recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return;
    recv_ans(ctrlfd);
}

// 处理stor命令，将本地文件上传到服务端
bool do_stor(const std::string& ser, const std::string& cli) {
    // 建立被动模式的数据连接
    int datafd = pasv_conn();

    if (datafd < 0) {
        std::cerr << "pasv_conn failed\n";
        return false;
    }
    // 向服务端发送STOR命令
    send_cmd("STOR " + ser, ctrlfd);

    std::string ans = recv_ans(ctrlfd);
    // 如果响应里有550，说明服务端无法创建文件
    if (ans.find("550") != std::string::npos) {
        close(datafd);
        return false;
    }
    //以二进制模式打开本地的源文件
    std::ifstream fp(cli, std::ios::binary);
    if (!fp) {
        std::cerr << "Local file not found\n";
        close(datafd);
        return false;
    }
    char buf[BUF_SIZE];
    int n;
    //循环读取本地文件的内容，每次读1024字节，gcount()用来获取实际读取到的字节数
    while (fp.read(buf, BUF_SIZE), (n = fp.gcount()) > 0) {
        int sent = 0;
        while (sent < n) {
            int m = send(datafd, buf + sent, n - sent, 0);
            if (m <= 0) {
                close(datafd);
                return false;
            }
            sent += m;
        }
    }
    //循环把读取到的文件内容发送到数据连接，因为TCP的send可能一次无法发完所有数据，所以要循环发送，直到这一批数据全部发完，再读下一批
    fp.close();
    //调用shutdown关闭数据连接的写端告诉服务端数据已经发完
    shutdown(datafd, SHUT_WR);
    close(datafd);
    recv_ans(ctrlfd);
    return true;
}

// 从服务端下载文件，处理retr命令，从服务端下载文件到本地
bool do_retr(const std::string& ser, const std::string& cli) {
    //建立被动模式的数据连接
    int datafd = pasv_conn();
    if (datafd < 0) {
        std::cerr << "pasv_conn failed\n";
        return false;
    }
    // 向服务端发送 RETR 命令
    send_cmd("RETR " + ser, ctrlfd);
    std::string ans = recv_ans(ctrlfd);

    if (ans.find("550") != std::string::npos) {
        close(datafd);
        return false;
    }
    //调用之前的file_name函数处理本地文件名，避免重名
    std::string real_name = file_name(cli);
    std::ofstream fp(real_name, std::ios::binary);

    if (!fp) {
        std::cerr << "Cannot create file\n";
        close(datafd);
        return false;
    }
    //告诉用户文件保存的名字
    std::cout << "Save as: " << real_name << std::endl;
    char buf[BUF_SIZE];
    int n;
    //循环从数据连接读取服务端发来的文件数据，写入本地文件，直到读取完所有数据
    while ((n = recv(datafd, buf, BUF_SIZE, 0)) > 0) {
        fp.write(buf, n);
    }
    fp.close();
    close(datafd);
    recv_ans(ctrlfd);
    return true;
}

void quit() {
    send_cmd("QUIT", ctrlfd);
    if (ctrlfd >= 0) {
        recv_ans(ctrlfd);
        close(ctrlfd);  //关闭控制连接
        ctrlfd = -1;    //重置ctrlfd为-1，标记回未连接状态
    }
}

//用户登录
bool login(const std::string& user, const std::string& pasw) {
    //先接收服务端的欢迎消息
    recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return false;
    //发送USER命令，把用户名发给服务端
    send_cmd("USER " + user, ctrlfd);
    if (ctrlfd < 0)
        return false;
    recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return false;
    send_cmd("PASS " + pasw, ctrlfd);
    if (ctrlfd < 0)
        return false;
    std::string r = recv_ans(ctrlfd);
    return r.find("230") != std::string::npos;
}

std::vector<std::string> split_cmd(const std::string& s, char flag) {
    std::vector<std::string> results;
    std::stringstream ss(s);
    std::string result;
    while (std::getline(ss, result, flag)) {
        results.push_back(result);
    }
    return results;
}

//分割命令参数
void parse_cmd(const std::string& cmd) {
    if (ctrlfd < 0)
        return;
    //按指定的分隔符分割字符串
    std::vector<std::string> commonds = split_cmd(cmd, ' ');
    if (commonds.empty())
        return;
    //调用相关函数
    if (commonds[0] == "ls") {
        do_ls();
    } else if (commonds[0] == "retr" && commonds.size() >= 3) {
        do_retr(commonds[1], commonds[2]);
    } else if (commonds[0] == "stor" && commonds.size() >= 3) {
        do_stor(commonds[2], commonds[1]);
    } else if (commonds[0] == "quit" || commonds[0] == "exit") {
        quit();
    } else {
        std::cout << "Commands:\n"
                  << "ls\n"
                  << "retr <remote_path> <local>\n"
                  << "stor <local> <remote_path>\n"
                  << "quit\n";
    }
}

int main() {
    //连接本地的127.0.0.1的2100端口，建立控制连接
    ctrlfd = connect_ser("127.0.0.1", ctrl_port);
    if (ctrlfd < 0) {
        std::cerr << "Connect failed!\n";
        return -1;
    }
    //用户输入用户名和密码
    std::string username, password;
    std::cout << "Username: ";
    std::getline(std::cin, username);
    std::cout << "Password: ";
    std::getline(std::cin, password);

    if (!login(username, password)) {
        std::cerr << "Login failed!\n";
        close(ctrlfd);
        return -1;
    }
    std::cout
        << "Login successful. Commands: ls, retr <remote_path> <local>, stor "
           "<local> <remote_path>, quit\n";

    std::string cmd;
    //进入命令循环，不断读取用户输入的命令，解析处理，直到控制连接断开或者用户退出
    while (ctrlfd >= 0) {
        std::cout << "ftp> ";
        if (!std::getline(std::cin, cmd))
            break;
        if (cmd.empty())
            continue;
        parse_cmd(cmd);
    }
    return 0;
}