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

#define BUF_SIZE 1024
#define ctrl_port 2100

int ctrlfd = -1;

int addport(int p1, int p2) {
    return p1 * 256 + p2;
}

int connect_ser(const std::string& ip, int port) {
    int data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd < 0)
        return -1;
    struct sockaddr_in data_addr{};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &data_addr.sin_addr);
    if (connect(data_fd, (sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        close(data_fd);
        return -1;
    }
    return data_fd;
}

void send_cmd(const std::string& commond, int fd) {
    std::string cmd = commond + "\r\n";
    int ret = send(fd, cmd.c_str(), cmd.size(), 0);
    if (ret <= 0) {
        std::cerr << "send_cmd failed, connection lost." << std::endl;
        close(ctrlfd);
        ctrlfd = -1;
        return;
    }
    std::cout << cmd << " send!" << std::endl;
}

std::string recv_ans(int fd) {
    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    int n = recv(fd, buf, BUF_SIZE - 1, 0);
    if (n <= 0) {
        std::cerr << "recv_ans failed, connection lost." << std::endl;
        close(ctrlfd);
        ctrlfd = -1;
        return "";
    }
    std::string answer(buf, n);
    while (!answer.empty() && (answer.back() == '\r' || answer.back() == '\n'))
        answer.pop_back();
    std::cout << "answer:" << answer << std::endl;
    return answer;
}

std::string file_name(std::string file){
    if(!std::filesystem::exists(file)){
        return file;
    }
    std::filesystem::path path(file);

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

std::pair<std::string, int> do_pasv(const std::string& answer) {
    int l1 = answer.find('(');
    int l2 = answer.find(')');
    if (l1 == std::string::npos || l2 == std::string::npos)
        throw std::runtime_error("Invalid PASV response");
    std::string ip_n = answer.substr(l1 + 1, l2 - l1 - 1);
    std::vector<int> nums;
    std::stringstream ss(ip_n);
    std::string t;
    while (getline(ss, t, ',')) {
        nums.push_back(stoi(t));
    }
    if (nums.size() < 6)
        throw std::runtime_error("Invalid PASV response");
    std::string ip = std::to_string(nums[0]) + "." + std::to_string(nums[1]) +
                     "." + std::to_string(nums[2]) + "." +
                     std::to_string(nums[3]);
    int port = addport(nums[4], nums[5]);
    return {ip, port};
}

int pasv_conn() {
    send_cmd("PASV", ctrlfd);
    if (ctrlfd < 0)
        return -1;
    std::string answer = recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return -1;
    auto [ip, port] = do_pasv(answer);
    std::cout << "Connect:" << ip << ":" << port << std::endl;
    return connect_ser(ip, port);
}

void do_ls() {
    int datafd = pasv_conn();
    if (datafd < 0) {
        std::cerr << "pasv_conn failed" << std::endl;
        return;
    }
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
    recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return;
    recv_ans(ctrlfd);
}

bool do_stor(const std::string& ser, const std::string& cli) {
    int datafd = pasv_conn();

    if (datafd < 0) {
        std::cerr << "pasv_conn failed\n";
        return false;
    }

    send_cmd("STOR " + ser, ctrlfd);

    std::string ans = recv_ans(ctrlfd);

    if (ans.find("550") != std::string::npos) {
        close(datafd);
        return false;
    }

    std::ifstream fp(cli, std::ios::binary);

    if (!fp) {
        std::cerr << "Local file not found\n";
        close(datafd);
        return false;
    }

    char buf[BUF_SIZE];

    int n;

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

    fp.close();

    shutdown(datafd, SHUT_WR);

    close(datafd);

    recv_ans(ctrlfd);

    return true;
}

bool do_retr(const std::string& ser, const std::string& cli) {
    int datafd = pasv_conn();

    if (datafd < 0) {
        std::cerr << "pasv_conn failed\n";
        return false;
    }

    send_cmd("RETR " + ser, ctrlfd);

    std::string ans = recv_ans(ctrlfd);

    if (ans.find("550") != std::string::npos) {
        close(datafd);
        return false;
    }

    std::string real_name = file_name(cli);
    std::ofstream fp(real_name, std::ios::binary);

    if (!fp) {
        std::cerr << "Cannot create file\n";
        close(datafd);
        return false;
    }

    std::cout << "Save as: " << real_name << std::endl;

    char buf[BUF_SIZE];

    int n;

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
        close(ctrlfd);
        ctrlfd = -1;
    }
}

bool login(const std::string& user, const std::string& pasw) {
    recv_ans(ctrlfd);
    if (ctrlfd < 0)
        return false;
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

void parse_cmd(const std::string& cmd) {
    if (ctrlfd < 0)
        return;

    std::vector<std::string> commonds = split_cmd(cmd, ' ');

    if (commonds.empty())
        return;

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
    ctrlfd = connect_ser("127.0.0.1", ctrl_port);
    if (ctrlfd < 0) {
        std::cerr << "Connect failed!\n";
        return -1;
    }
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