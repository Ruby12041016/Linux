# epoll：Linux 高性能 I/O 的核心机制

## 一、epoll 的设计哲学

epoll 是 Linux 内核为处理**海量并发连接**而设计的高效 I/O 多路复用机制。它的核心设计目标是在**连接数巨大但活跃连接很少**的场景下，依然能保持高效性能。

## 二、epoll 的三大核心 API

### 1. **epoll_create / epoll_create1**

```c
int epoll_create(int size);  // 传统接口
int epoll_create1(int flags);  // 推荐使用的现代接口
```

**作用**：创建一个 epoll 实例，返回文件描述符。

**参数详解**：
- `size`：历史上表示期望监控的描述符数量，现在只是提示值（Linux 2.6.8+ 忽略，但必须 > 0）
- `flags`：
  - `0`：默认行为
  - `EPOLL_CLOEXEC`：exec 时关闭文件描述符

**内核内部**：
- 创建 `struct eventpoll` 结构体
- 初始化红黑树（管理所有被监控的 fd）
- 初始化就绪链表（存放就绪的事件）

### 2. **epoll_ctl**

```c
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
```

**作用**：注册/修改/删除要监控的文件描述符和事件。

**参数详解**：
- `op` 操作类型：
  - `EPOLL_CTL_ADD`：添加新的 fd
  - `EPOLL_CTL_MOD`：修改已有 fd 的监控事件
  - `EPOLL_CTL_DEL`：删除 fd

- `struct epoll_event`：
```c
typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;    // 要监控的事件
    epoll_data_t data;      // 用户数据，事件就绪时返回
};
```

**常用 events 标志**：
| 标志 | 含义 | 说明 |
|------|------|------|
| EPOLLIN | 可读 | 有数据可读 |
| EPOLLOUT | 可写 | 可写入数据 |
| EPOLLRDHUP | 对端关闭 | 对端关闭连接或半关闭 |
| EPOLLPRI | 紧急数据 | 带外数据到达 |
| EPOLLERR | 错误 | 文件描述符发生错误 |
| EPOLLHUP | 挂起 | 文件描述符被挂起 |
| EPOLLET | 边缘触发 | 设置为边缘触发模式 |
| EPOLLONESHOT | 一次性事件 | 事件就绪后自动禁用监控 |

### 3. **epoll_wait**

```c
int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout);
```

**作用**：等待事件发生，返回就绪的事件。

**参数详解**：
- `events`：存放就绪事件的数组
- `maxevents`：最多返回的事件数量，不能超过 `events` 数组大小
- `timeout`：超时时间（毫秒）
  - `-1`：永久阻塞
  - `0`：立即返回
  - `>0`：等待指定的毫秒数

## 三、epoll 的内核实现机制

### 1. **关键数据结构**

```c
// 内核中的关键结构
struct eventpoll {
    spinlock_t lock;            // 自旋锁保护就绪队列
    wait_queue_head_t wq;       // 等待队列，epoll_wait 的进程挂在此处
    wait_queue_head_t poll_wait; // file->poll() 使用的等待队列
    
    struct list_head rdllist;   // 就绪描述符链表
    struct rb_root rbr;         // 红黑树根节点
    struct epitem *ovflist;     // 就绪事件临时链表
};
```

### 2. **工作流程解析**

**监控流程**：
1. 应用程序调用 `epoll_ctl(EPOLL_CTL_ADD)` 添加 socket
2. 内核将 socket 插入红黑树
3. 为 socket 注册回调函数 `ep_poll_callback`
4. 当 socket 有数据到达时，内核回调 `ep_poll_callback`
5. 回调函数将 socket 插入就绪链表 `rdllist`

**等待流程**：
1. 应用程序调用 `epoll_wait()`
2. 内核检查就绪链表
3. 将就绪事件复制到用户空间
4. 清空就绪链表（ET 模式）或保留（LT 模式）

## 四、深入理解两种触发模式

### 1. **水平触发（LT）详解**

**特点**：只要缓冲区有数据，就会一直通知。

```c
// LT 模式下的典型处理逻辑
while (1) {
    int nready = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (int i = 0; i < nready; i++) {
        if (events[i].events & EPOLLIN) {
            // 可以只读部分数据
            // 即使没读完，下次 epoll_wait 还会通知
            int n = read(events[i].data.fd, buf, sizeof(buf));
            if (n > 0) {
                // 处理数据...
            }
        }
    }
}
```

**LT 模式的唤醒时机**：
- 读事件：接收缓冲区有数据
- 写事件：发送缓冲区有空间

### 2. **边缘触发（ET）详解**

**特点**：仅在状态变化时通知一次。

```c
// ET 模式必须使用非阻塞 I/O
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ET 模式下的读取（必须一次读完）
if (events[i].events & EPOLLIN) {
    // 必须循环读取直到 EAGAIN
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            // 处理数据
        } else if (n == 0) {
            // 对端关闭连接
            close(fd);
            break;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据已读完
                break;
            } else {
                // 真实错误
                close(fd);
                break;
            }
        }
    }
}
```

**ET 模式的关键点**：
1. **必须使用非阻塞文件描述符**
2. **必须一次性处理完所有数据**（循环读/写直到 EAGAIN）
3. **避免丢失事件**：在 read 返回 EAGAIN 后，如果又有新数据到达，会再次触发 EPOLLIN

## 五、完整服务器示例

```c
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>保证写的互
#include <stdio.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096

// 设置非阻塞
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    // 设置 SO_REUSEADDR
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 绑定
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8888);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    
    // 监听
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    
    // 创建 epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return 1;
    }
    
    // 添加监听 socket
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // 边缘触发
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl listen_fd");
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }
    
    struct epoll_event events[MAX_EVENTS];
    
    printf("Server started on port 8888\n");
    
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;  // 被信号中断
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t event_mask = events[i].events;
            
            // 错误处理
            if (event_mask & (EPOLLERR | EPOLLHUP)) {
                printf("Error on fd %d\n", fd);
                close(fd);
                continue;
            }
            
            // 新连接
            if (fd == listen_fd) {
                // 边缘触发，必须循环 accept
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t addr_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, 
                                         (struct sockaddr*)&client_addr, 
                                         &addr_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // 已接受完所有连接
                        }
                        perror("accept");
                        break;
                    }
                    
                    // 设置非阻塞
                    set_nonblocking(client_fd);
                    
                    // 添加 EPOLLRDHUP 监听对端关闭
                    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl client_fd");
                        close(client_fd);
                    } else {
                        printf("New connection: fd=%d\n", client_fd);
                    }
                }
            }
            // 对端关闭
            else if (event_mask & EPOLLRDHUP) {
                printf("Client disconnected: fd=%d\n", fd);
                close(fd);
            }
            // 可读事件
            else if (event_mask & EPOLLIN) {
                char buffer[BUFFER_SIZE];
                ssize_t total_read = 0;
                
                // 边缘触发，必须循环读取
                while (1) {
                    ssize_t n = read(fd, buffer + total_read, 
                                    sizeof(buffer) - total_read);
                    if (n > 0) {
                        total_read += n;
                        if (total_read >= sizeof(buffer)) {
                            break;  // 缓冲区已满
                        }
                    } else if (n == 0) {
                        // 对端关闭连接
                        printf("Connection closed by client: fd=%d\n", fd);
                        close(fd);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 数据已读完
                            if (total_read > 0) {
                                // 处理数据...
                                buffer[total_read] = '\0';
                                printf("Received %zd bytes: %s\n", 
                                       total_read, buffer);
                                
                                // 回显
                                write(fd, buffer, total_read);
                            }
                        } else {
                            perror("read");
                            close(fd);
                        }
                        break;
                    }
                }
            }
        }
    }
    
    close(listen_fd);
    close(epoll_fd);
    return 0;
}
```

## 六、epoll 的高级特性

### 1. **EPOLLONESHOT**

```c
// 确保一个 socket 在某个时刻只被一个线程处理
ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

// 处理完后重新启用
if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
    perror("epoll_ctl mod");
}
```

**作用**：防止同一个 socket 被多个线程同时处理。

### 2. **EPOLLEXCLUSIVE**（Linux 4.5+）

```c
ev.events = EPOLLIN | EPOLLEXCLUSIVE;
```

**作用**：避免惊群效应（多个进程/线程同时被唤醒）。

## 七、epoll 的性能优化

### 1. **数据结构优化**

```c
// 1. 使用 epoll_data_t.ptr 传递更多信息
struct connection {
    int fd;
    char buffer[BUFFER_SIZE];
    size_t buffer_used;
};

struct connection *conn = malloc(sizeof(struct connection));
conn->fd = client_fd;
conn->buffer_used = 0;

ev.events = EPOLLIN | EPOLLET;
ev.data.ptr = conn;  // 传递结构体指针
epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

// 使用时
struct connection *conn = events[i].data.ptr;
int fd = conn->fd;
// 直接使用 conn 中的 buffer
```

### 2. **批量操作优化**

```c
// 使用 EPOLL_CTL_MOD 批量修改事件
struct epoll_event ev;
ev.events = EPOLLOUT | EPOLLET;  // 改为监听写事件
ev.data.fd = fd;
epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
```

## 八、常见问题与解决方案

### 1. **ET 模式下的饥饿问题**

**问题**：当有大量数据持续到达时，epoll_wait 可能一直返回同一个 fd。

**解决方案**：
```c
// 处理一定次数后主动让出
int process_count = 0;
while (1) {
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, 0);  // 非阻塞
    if (nfds <= 0) break;
    
    for (int i = 0; i < nfds; i++) {
        // 处理事件
        process_count++;
        if (process_count > MAX_PROCESS_PER_LOOP) {
            // 处理一定数量后，短暂阻塞等待
            usleep(1000);  // 1ms
            process_count = 0;
        }
    }
}
```

### 2. **内存泄漏问题**

```c
// 正确关闭连接
void close_connection(int epfd, int fd) {
    // 1. 从 epoll 中删除
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    
    // 2. 关闭 socket
    close(fd);
    
    // 3. 如果使用了 ptr，释放内存
    // 注意：需要额外机制跟踪 ptr
}
```

## 九、epoll 与多线程/多进程

### 1. **多线程 epoll**

```c
// 多个线程共享同一个 epoll 实例
// 需要配合 EPOLLONESHOT 使用
void* worker_thread(void* arg) {
    int epfd = *(int*)arg;
    struct epoll_event events[MAX_EVENTS];
    
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            // 处理事件
            // 处理完成后重新启用 EPOLLONESHOT
        }
    }
    return NULL;
}
```

### 2. **多进程 epoll（REUSEPORT）**

```c
// Linux 3.9+ 支持 SO_REUSEPORT
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

// 多个进程可以绑定相同端口
// 每个进程有自己的 epoll 实例
// 内核负责负载均衡连接
```

## 十、性能对比数据

| 操作 | 10,000 空闲连接 | 10,000 活跃连接 | 100,000 空闲连接 |
|------|----------------|-----------------|------------------|
| **添加连接** | epoll_ctl: 0.2ms | epoll_ctl: 0.2ms | epoll_ctl: 0.3ms |
| **事件等待** | epoll_wait: < 0.1ms | epoll_wait: 0.5ms | epoll_wait: 0.2ms |
| **vs select** | 100x 更快 | 10x 更快 | 1000x 更快 |

## 总结

**epoll 的核心优势**：
1. **时间复杂度 O(1)**：与监控的连接总数无关，只与活跃连接数相关
2. **内存效率高**：不需要每次传递所有文件描述符
3. **边缘触发模式**：减少不必要的系统调用
4. **内核态数据结构**：红黑树管理，查找效率高

**最佳实践**：
1. 长连接、低活跃度的场景首选 epoll
2. 使用 ET 模式以获得最佳性能
3. 配合非阻塞 I/O
4. 合理使用 EPOLLONESHOT 避免竞态条件
5. 监控 EPOLLRDHUP 以优雅处理连接关闭

**注意事项**：
1. ET 模式必须处理 EAGAIN
2. 注意文件描述符泄漏
3. 考虑线程安全性
4. 监控系统资源限制（`/proc/sys/fs/epoll/max_user_watches`）

epoll 是构建现代高性能网络服务器的基石，Nginx、Redis 等知名软件都基于 epoll 实现其高并发能力。