#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <string>

using namespace std;

/*
    fork() 系统调用
    - 创建子进程，子进程是父进程的副本
    - 返回两次：父进程返回子进程PID，子进程返回0
    - 错误时返回-1
 */
void _fork() {
    pid_t pid = fork();
    if (pid < 0) {
        cerr << "fork失败" << endl;
    } else if (pid == 0) {
        // 子进程执行
        cout << "子进程: PID=" << getpid() << ", PPID=" << getppid() << endl;
        _exit(0);  // 子进程退出
    } else {
        // 父进程执行
        cout << "父进程: PID=" << getpid() << ", 创建的子进程PID=" << pid
             << endl;
        wait(nullptr);  // 等待子进程
    }
}

/*
   写时复制 (Copy-On-Write, COW)
    - 父子进程初始共享物理内存页
    - 内存页标记为只读
    - 当任一进程尝试写入时，内核复制该页
    - 优化性能，减少不必要的内存复制
 */
void copy_write() {
    int data = 100;
    cout << "父进程: data地址=" << &data << ", 值=" << data << endl;
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程修改数据
        data = 200;
        cout << "子进程修改后: data地址=" << &data << ", 值=" << data << endl;
        _exit(0);
    } else {
        wait(nullptr);
        cout << "父进程: data地址=" << &data << ", 值=" << data << endl;
    }
}

/*
   文件描述符继承
    - 子进程继承父进程所有打开的文件描述符
    - 共享相同的文件偏移量
    - 需要正确管理，避免意外共享
 */
void file() {
    ofstream outfile("test.txt", ios::out | ios::trunc);
    if (!outfile) {
        cerr << "打开文件失败" << endl;
        return;
    }
    outfile << "父进程写入" << endl;
    pid_t pid = fork();
    if (pid == 0) {
        outfile << "子进程写入" << endl;
        outfile.close();
        _exit(0);
    } else {
        wait(nullptr);
        outfile << "父进程再次写入" << endl;
        outfile.close();
        cout << "文件内容被父进程写入" << endl;
    }
}

/*
   竞争条件与同步
    - 父子进程执行顺序不确定
    - 需要同步机制避免竞态条件
 */
void condition() {
    int num = 0;
    for (int i = 0; i < 5; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            cout << "子进程 " << i << ": 当前num=" << num << endl;
            _exit(0);
        } else {
            num++;
        }
    }
    for (int i = 0; i < 5; i++) {
        wait(nullptr);
    }
    cout << "最终num值: " << num << endl;
}

int main() {
    _fork();
    copy_write();
    file();
    condition();
    return 0;
}
