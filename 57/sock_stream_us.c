//客户端
#include "us_xfr.h"

int main(int argc, char* argv[]) {
    struct sockaddr_un addr; 
    int sfd;
    ssize_t numRead;
    char buf[BUF_SIZE];

    //创建套接字
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //清零
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    //复制服务端路径
    strncpy(addr.sun_path, SV_SOCK_PATH, sizeof(addr.sun_path) - 1);
    //主动链接服务端，向服务端的套接字文件发起连接，成功后sfd是与服务端通信的唯一通道
    if (connect(sfd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) ==
        -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    printf("Connected to server. Type text and press Enter:\n");

    // STDIN_FILENO表示从终端键盘读取，循环读取
    while ((numRead = read(STDIN_FILENO, buf, BUF_SIZE)) > 0) {
        if (write(sfd, buf, numRead) != numRead) {      //write(sfd,...)将用户输入发送给服务端
            perror("partial/failed write");
            break;
        }
    }

    if (numRead == -1) {
        perror("read");
    }

    exit(EXIT_SUCCESS);  //关闭套接字，服务端受到EOF，内核会自动关闭sfd，结束通信
}