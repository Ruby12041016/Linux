#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


/*
    fork() 系统调用
    - 创建子进程，子进程是父进程的副本
    - 返回两次：父进程返回子进程PID，子进程返回0
    - 错误时返回-1
 */
void fork_basic() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork失败");
    } else if (pid == 0) {
        // 子进程执行
        printf("子进程: PID=%d, PPID=%d\n", getpid(), getppid());
        _exit(0);  // 子进程退出
    } else {
        // 父进程执行
        printf("父进程: PID=%d, 创建的子进程PID=%d\n", getpid(), pid);
        wait(NULL);  // 等待子进程
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
    printf("父进程: data地址=%p, 值=%d\n", (void*)&data,
           data);
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程修改数据
        data = 200;
        printf("子进程修改后: data地址=%p, 值=%d\n", (void*)&data,
               data);
        _exit(0);
    } else {
        wait(NULL);
        printf("父进程: data地址=%p, 值=%d \n",
               (void*)&data, data);
    }
}

/*
   文件描述符继承
    - 子进程继承父进程所有打开的文件描述符
    - 共享相同的文件偏移量
    - 需要正确管理，避免意外共享
 */
void file() {
    int fd = open("test.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("打开文件失败");
        return;
    }
    write(fd, "父进程写入\n", strlen("父进程写入\n"));
    pid_t pid = fork();
    if (pid == 0) {
        write(fd, "子进程写入\n", strlen("子进程写入\n"));
        close(fd);
        _exit(0);
    } else {
        wait(NULL);
        write(fd, "父进程再次写入\n", strlen("父进程再次写入\n"));
        close(fd);
        printf("文件内容被父进程写入\n");
    }
}

/*
   vfork() 系统调用
    - 创建子进程但不复制内存
    - 子进程共享父进程地址空间
    - 保证子进程先执行，直到调用exec或exit
    - 现代Linux中通常使用fork+COW替代
   竞争条件与同步
    - 父子进程执行顺序不确定
    - 需要同步机制避免竞态条件
 */
void condition() {
    int num = 0;
    for (int i = 0; i < 5; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            printf("子进程 %d: 当前num=%d\n", i, num);
            _exit(0);
        } else {
            num++;
        }
    }
    for (int i = 0; i < 5; i++) {
        wait(NULL);
    }
    printf("最终num值: %d\n", num);
}

int main() {
    fork_basic();
    copy_write();
    file();
    condition();
    return 0;
}
