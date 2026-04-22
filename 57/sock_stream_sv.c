//服务端
#include "us_xfr.h"

#define BACKLOG 5   //定义监听队列的最大长度，最多挂起5个待链接的客户端   //套接字：两个进程间互相发数据的收发窗口

int main(int argc, char* argv[]) {
    struct sockaddr_un addr;    //定义NUIX域套接字地址结构体，用来存套接字的文件路径和协议族 
    int sfd, cfd;              //sfd是监听套接字，服务端自己的“门”，cfd是客户端连接套接字，和客户端连接的通道
    ssize_t numRead;        //储存read读取到的字节数
    char buf[BUF_SIZE];      //数据缓冲区，用来存客户端发过来的数据

    //创建socket
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);    //前两个参数表示UNIX本地域，流式
    if (sfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //清理旧的文件，防止上一次异常退出残留的文件导致本次bind失败
    if (remove(SV_SOCK_PATH) == -1 && errno != ENOENT) {
        perror("remove");
        exit(EXIT_FAILURE);
    }
    //把地址结构体全部清零，避免垃圾数据导致异常
    memset(&addr, 0, sizeof(struct sockaddr_un));
    //指定协议族为UNIX本地域
    addr.sun_family = AF_UNIX;
    //把套接字文件路径复制到地址结构体里
    strncpy(addr.sun_path, SV_SOCK_PATH, sizeof(addr.sun_path) - 1);
    //把套接字和本地文件路径绑定，客户端以后通过这个文件找服务端
    if (bind(sfd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    //把套接字变成被动监听状态，开始等客户端来连
    if (listen(sfd, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s\n", SV_SOCK_PATH);

    for (;;) {  //循环接受客户端连接
        cfd = accept(sfd, NULL, NULL);   //阻塞等待，没客户端来就一直卡在这里，成功后返回新的cfd，专门和这个客户端通信
        if (cfd == -1) {                    //sfd保留，下次循环继续等新的客户端
            perror("accept");
            continue;
        }

        printf("Client connected\n");

        
        while ((numRead = read(cfd, buf, BUF_SIZE)) > 0) {   //从客户端套接字cfd中读取数据存入缓冲区
            if (write(STDOUT_FILENO, buf, numRead) != numRead) {   //把读到的数据原样打印回去
                perror("partial/failed write");
                break;
            }
        }

        if (numRead == -1) {
            perror("read");
        }

        if (close(cfd) == -1) {    //关闭
            perror("close");
        }

        printf("Client disconnected\n");
    }
}