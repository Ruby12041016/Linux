#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// ========== 工具函数 ==========

/**
 * @brief 将文件描述符设置为非阻塞模式
 * @param fd 要设置的文件描述符
 *
 * 非阻塞模式下，read/write操作不会阻塞等待数据，
 * 如果没有数据或缓冲区满，会立即返回EAGAIN/EWOULDBLOCK错误
 */
void setNonBlock(int fd) {
    // 获取当前文件状态标志
    int flags = fcntl(fd, F_GETFL, 0);
    // 添加O_NONBLOCK标志，设置为非阻塞模式
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief 创建一个eventfd文件描述符，用于线程间唤醒
 * @return 创建成功的eventfd文件描述符
 *
 * eventfd是Linux提供的轻量级进程间通信机制，
 * 常用于线程间的事件通知（如唤醒事件循环）
 */
int createEventFd() {
    // 创建eventfd，设置非阻塞和exec时关闭标志
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        perror("eventfd create");
        abort();  // 如果创建失败，直接终止程序
    }
    return efd;
}

// ========== 前向声明 ==========
class EventLoop;      // 事件循环类，管理epoll和事件处理
class TcpConnection;  // TCP连接类，代表一个客户端连接

// ========== Worker线程池（厨房） ==========
/**
 * @class WorkerPool
 * @brief 线程池类，用于执行耗时的业务逻辑
 *
 * 类比餐厅中的厨房：服务员（EventLoop）接收到订单（请求）后，
 * 交给厨房（WorkerPool）处理，处理完成后再由服务员上菜（发送响应）
 */
class WorkerPool {
   public:
    /**
     * @brief 构造函数，创建指定数量的工作线程
     * @param numThreads 工作线程数量
     */
    WorkerPool(int numThreads) : stop_(false) {
        // 创建numThreads个工作线程
        for (int i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this]() {
                // 工作线程主循环
                while (true) {
                    std::function<void()> task;  // 存储待执行的任务
                    {
                        // 加锁访问任务队列
                        std::unique_lock<std::mutex> lock(mtx_);
                        // 等待条件：要么停止，要么有任务
                        cv_.wait(lock,
                                 [this] { return stop_ || !tasks_.empty(); });
                        // 如果停止且任务为空，退出线程
                        if (stop_ && tasks_.empty())
                            return;
                        // 取出队列头部的任务（移动语义，避免拷贝）
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }  // 锁在这里释放
                    task();  // 执行耗时业务逻辑
                }
            });
        }
    }

    /**
     * @brief 提交一个任务到线程池
     * @param f 要执行的任务（函数对象）
     */
    void submit(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.emplace(std::move(f));  // 将任务加入队列
        }  // 锁在这里释放
        cv_.notify_one();  // 唤醒一个等待的工作线程
    }

    /**
     * @brief 析构函数，优雅地停止所有工作线程
     */
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;  // 设置停止标志
        }
        cv_.notify_all();  // 唤醒所有工作线程
        // 等待所有线程完成
        for (auto& t : workers_)
            t.join();
    }

    /**
     * @brief 获取单例实例（全局唯一的WorkerPool）
     * @return WorkerPool实例指针
     */
    static WorkerPool* instance() {
        // C++11保证静态局部变量初始化是线程安全的
        static WorkerPool pool( // 静态局部变量，只在第一次调用instance()初始化，之后都直接返回同一个对象的地址
            std::thread::hardware_concurrency());  // hardware_concurrency()用来获取当前cpu的硬件线程数
        return &pool;
    }

   private:
    std::vector<std::thread> workers_;         // 工作线程集合
    std::queue<std::function<void()>> tasks_;  // 任务队列
    std::mutex mtx_;                           // 保护任务队列的互斥锁
    std::condition_variable cv_;               // 条件变量，用于线程同步
    bool stop_;                                // 停止标志
};

// ========== 事件循环（SubReactor / 服务员） ==========
/**
 * @class EventLoop
 * @brief 事件循环类，管理epoll和事件处理
 *
 * 类比餐厅中的服务员：负责监听客人（客户端连接）的需求（事件），
 * 接收订单（读取数据），交给厨房（WorkerPool）处理，
 * 然后把做好的菜（响应）送给客人
 */
class EventLoop {
   public:
    /**
     * @brief 构造函数，初始化epoll和eventfd
     */
    EventLoop()
        : epfd_(epoll_create1(EPOLL_CLOEXEC)),  // 创建epoll实例，exec时关闭
          wakeupFd_(createEventFd()),           // 创建唤醒用的eventfd
          stop_(false) {                        // 初始化停止标志为false
        // 把唤醒fd注册到自己的epoll，监听可读事件
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;  // 边缘触发模式
        ev.data.fd = wakeupFd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeupFd_, &ev);
    }

    /**
     * @brief 析构函数，关闭文件描述符
     */
    ~EventLoop() {
        close(epfd_);
        close(wakeupFd_);
    }

    /**
     * @brief 启动事件循环（必须在所属线程中调用）
     *
     * 这是EventLoop的主循环，不断等待事件并处理
     */
    void loop() {
        // 记录当前线程ID，用于判断是否在事件循环线程中
        threadId_ = std::this_thread::get_id();
        // 预分配事件数组，最多容纳128个事件
        std::vector<epoll_event> events(128);

        // 事件循环主循环
        while (!stop_) {
            // 等待事件，-1表示无限等待
            int nfds = epoll_wait(epfd_, events.data(), events.size(), -1);
            if (nfds < 0) {
                // 如果是被信号中断，继续循环
                if (errno == EINTR)
                    continue;
                perror("epoll_wait");
                break;
            }
            // 遍历所有就绪的事件
            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                if (fd == wakeupFd_) {
                    // 收到唤醒信号：处理跨线程投递的任务
                    handleWakeup();
                } else {
                    // 某个连接就绪，找到对应的TcpConnection处理
                    TcpConnection* conn = connections_[fd];
                    if (conn) {
                        conn->handleEvent(events[i].events);
                    }
                }
            }
        }
    }

    /**
     * @brief 跨线程添加任务
     * @param task 要执行的任务
     *
     * 其他线程可以通过此方法向EventLoop线程投递任务，
     * 比如MainReactor接收新连接后，通过此方法交给SubReactor处理
     */
    void addTask(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            pendingTasks_.push(std::move(task));
        }
        wakeup();  // 唤醒事件循环线程
    }

    /**
     * @brief 注册或修改连接的监听事件
     * @param fd 文件描述符
     * @param events 要监听的事件（如EPOLLIN | EPOLLET）
     *
     * 如果该fd还未注册，则添加；否则修改已有监听
     */
    void updateEpollEvent(int fd, int events) {
        epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        // 先尝试修改
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            // 如果返回ENOENT，说明还没添加，尝试ADD
            if (errno == ENOENT) {
                epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
            } else {
                perror("updateEpollEvent");
            }
        }
    }

    /**
     * @brief 移除连接的监听
     * @param fd 要移除的文件描述符
     */
    void removeEpollEvent(int fd) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    /**
     * @brief 保存TcpConnection对象
     * @param fd 连接的文件描述符
     * @param conn TcpConnection对象指针
     *
     * 建立fd到连接对象的映射，以便事件就绪时快速找到对应的连接
     */
    void addConnection(int fd, TcpConnection* conn) { connections_[fd] = conn; }

    /**
     * @brief 移除TcpConnection对象
     * @param fd 连接的文件描述符
     */
    void removeConnection(int fd) { connections_.erase(fd); }

    /**
     * @brief 判断当前线程是否就是事件循环线程
     * @return true表示在事件循环线程中，false表示在其他线程
     */
    bool isInLoopThread() const {
        return std::this_thread::get_id() == threadId_;
    }

   private:
    /**
     * @brief 唤醒事件循环线程
     *
     * 往eventfd写入一个64位整数，触发EPOLLIN事件，
     * 使epoll_wait返回，从而处理pendingTasks_
     */
    void wakeup() {
        uint64_t one = 1;
        ssize_t n = write(wakeupFd_, &one, sizeof(one));
        if (n != sizeof(one)) {
            perror("eventfd write");
        }
    }

    /**
     * @brief 处理唤醒事件
     *
     * 1. 读空eventfd（边缘触发必须读到EAGAIN）
     * 2. 执行所有积压的任务
     */
    void handleWakeup() {
        uint64_t one;
        // 边缘触发必须循环读，直到返回EAGAIN
        while (read(wakeupFd_, &one, sizeof(one)) > 0)
            ;

        // 执行所有积压任务
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            // 交换任务队列，减少临界区时间
            pendingTasks_.swap(tasks);  // 实现直接交换，把pendingTasks_里的任务全部移动到了tasks里（原来tasks是空的，所以pendingTasks_变成空的）
        }
        // 在临界区外执行任务
        while (!tasks.empty()) {
            tasks.front()();
            tasks.pop();
        }
    }

    int epfd_;                       // epoll实例的文件描述符
    int wakeupFd_;                   // 用于唤醒的eventfd
    std::atomic<bool> stop_{false};  // 停止标志（原子操作）
    std::thread::id threadId_;       // 事件循环线程ID
    std::mutex mtx_;                 // 保护pendingTasks_的互斥锁
    std::queue<std::function<void()>> pendingTasks_;       // 待执行的任务队列
    std::unordered_map<int, TcpConnection*> connections_;  // fd到连接的映射
};

// ========== TCP连接（代表一个客人） ==========
/**
 * @class TcpConnection
 * @brief TCP连接类，代表一个客户端连接
 *
 * 类比餐厅中的客人：连接建立后就像客人入座，
 * 可以点菜（发送数据）、吃饭（接收数据）、结账离开（断开连接）
 *
 * 继承enable_shared_from_this是为了在回调中安全地获取自身的shared_ptr，
 * 防止对象在回调执行过程中被销毁:
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
   public:
    /**
     * @brief 构造函数
     * @param fd 连接的文件描述符
     * @param loop 所属的EventLoop
     */
    TcpConnection(int fd, EventLoop* loop)
        : fd_(fd), loop_(loop), state_(kConnected) {
        // 设置为非阻塞模式
        setNonBlock(fd);
        // 初始只监听可读事件（边缘触发）
        loop_->updateEpollEvent(fd_, EPOLLIN | EPOLLET);
        // 注册到EventLoop的连接映射表
        loop_->addConnection(fd_, this);
    }

    /**
     * @brief 析构函数，关闭文件描述符
     */
    ~TcpConnection() {
        ::close(fd_);
        std::cout << "连接 fd=" << fd_ << " 已销毁" << std::endl;
    }

    /**
     * @brief 事件就绪回调（由所属EventLoop线程调用）
     * @param revents 就绪的事件类型
     *
     * EventLoop检测到事件后，调用此方法处理
     */
    void handleEvent(uint32_t revents) {
        // 如果发生错误或连接断开
        if (revents & (EPOLLERR | EPOLLHUP)) {
            closeConnection();
            return;
        }
        // 如果可读
        if (revents & EPOLLIN) {
            handleRead();
        }
        // 如果可写
        if (revents & EPOLLOUT) {
            handleWrite();
        }
    }

    /**
     * @brief 发送数据（线程安全）
     * @param msg 要发送的数据
     *
     * 可以从任何线程调用，内部会判断是否在EventLoop线程，
     * 如果不是，则把发送任务投递到EventLoop线程执行
     */
    void send(const std::string& msg) {
        if (loop_->isInLoopThread()) {
            // 如果就在本EventLoop线程，直接发送
            sendInLoop(msg);
        } else {
            // 跨线程：把发送操作包装成任务，丢给所属EventLoop
            // 使用shared_from_this()确保对象在回调期间不被销毁
            auto self = shared_from_this();
            loop_->addTask([self, msg]() { self->sendInLoop(msg); });
        }
    }

    /**
     * @brief 主动关闭连接
     */
    void closeConnection() {
        // 避免重复关闭
        if (state_ == kDisconnected)
            return;

        state_ = kDisconnected;
        // 从epoll中移除
        loop_->removeEpollEvent(fd_);
        // 从连接映射表中移除
        loop_->removeConnection(fd_);
        // 关闭文件描述符
        ::close(fd_);
        // 智能指针引用计数减1，最后一个引用释放时对象自动销毁
    }

   private:
    /**
     * @brief 处理可读事件（在EventLoop线程执行）
     *
     * 边缘触发模式下必须循环读取，直到返回EAGAIN
     */
    void handleRead() {
        char buf[4096];  // 临时缓冲区，每次最多读4KB
        while (true) {
            // 读取数据
            ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                // 成功读到数据
                std::string msg(buf, n);
                std::cout << "收到数据 fd=" << fd_ << ": " << msg;

                // 交给Worker线程池处理耗时业务
                // 使用shared_from_this()确保对象生命周期安全
                auto self = shared_from_this();
                WorkerPool* pool = WorkerPool::instance();
                pool->submit([self, msg]() {
                    // 模拟耗时操作（如数据库查询、计算等）
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    // 构造响应
                    std::string response = "Echo: " + msg;
                    // 发送响应（send是线程安全的）
                    self->send(response);
                });
            } else if (n == 0) {
                // 对端关闭连接
                std::cout << "对端关闭 fd=" << fd_ << std::endl;
                closeConnection();
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 边缘触发下必须读到EAGAIN才算读完
                    break;
                }
                // 其他错误
                perror("read");
                closeConnection();
                break;
            }
        }
    }

    /**
     * @brief 处理可写事件（在EventLoop线程执行）
     *
     * 当socket发送缓冲区有空间时触发
     */
    void handleWrite() {
        // 如果写缓冲区为空，撤销EPOLLOUT监听
        if (writeBuffer_.empty()) {
            loop_->updateEpollEvent(fd_, EPOLLIN | EPOLLET);
            return;
        }

        // 尝试写入数据
        ssize_t n = ::write(fd_, writeBuffer_.data(), writeBuffer_.size());
        if (n > 0) {
            // 成功写入n字节，从缓冲区删除已写部分
            writeBuffer_.erase(0, n);
            // 如果写完了，撤销EPOLLOUT
            if (writeBuffer_.empty()) {
                loop_->updateEpollEvent(fd_, EPOLLIN | EPOLLET);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // 发生错误
            perror("write");
            closeConnection();
        }
        // 如果返回EAGAIN，保持EPOLLOUT，等待下次触发
    }

    /**
     * @brief 实际发送逻辑（必须在EventLoop线程执行）
     * @param msg 要发送的数据
     */
    void sendInLoop(const std::string& msg) {
        // 如果连接已断开，直接返回
        if (state_ != kConnected)
            return;

        // 如果当前没有待写数据，尝试直接写
        if (writeBuffer_.empty()) {
            ssize_t n = ::write(fd_, msg.data(), msg.size());
            if (n >= 0) {
                if (n == msg.size())
                    return;  // 全部写完，直接返回
                // 没写完，剩余部分放入写缓冲区
                writeBuffer_.append(msg.data() + n, msg.size() - n);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // 发生错误
                perror("sendInLoop write");
                closeConnection();
                return;
            } else {
                // 发送缓冲区满，全部放入写缓冲区
                writeBuffer_.append(msg);
            }
        } else {
            // 原来就有没写完的数据，直接追加到缓冲区
            writeBuffer_.append(msg);
        }
        // 开启EPOLLOUT监听，等待可写
        loop_->updateEpollEvent(fd_, EPOLLIN | EPOLLOUT | EPOLLET);
    }

    int fd_;                                   // 连接的文件描述符
    EventLoop* loop_;                          // 所属的EventLoop
    enum State { kConnected, kDisconnected };  // 连接状态
    State state_;                              // 当前状态
    std::string writeBuffer_;  // 写缓冲区（处理粘包和缓冲区满的情况）
};

// ========== 主Reactor（迎宾员） ==========
/**
 * @class MainReactor
 * @brief 主Reactor，负责监听新连接
 *
 * 类比餐厅中的迎宾员：专门负责迎接客人（接收新连接），
 * 然后把客人分配给服务员（SubReactor）服务
 */
class MainReactor {
   public:
    /**
     * @brief 构造函数
     * @param port 监听端口
     * @param loops SubReactor数组
     */
    MainReactor(int port, std::vector<EventLoop*>& loops)
        : subLoops_(loops), round_(0) {
        // 创建监听socket（非阻塞模式）
        listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listenFd_ < 0) {
            perror("socket");
            abort();
        }

        // 设置SO_REUSEADDR选项，允许端口快速重用
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // 设置地址结构
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;          // IPv4
        addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
        addr.sin_port = htons(port);        // 转换为网络字节序

        // 绑定地址
        bind(listenFd_, (sockaddr*)&addr, sizeof(addr));
        // 开始监听，SOMAXCONN是系统允许的最大连接数
        listen(listenFd_, SOMAXCONN);

        // 创建主Reactor专用的epoll实例
        epfd_ = epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;  // 边缘触发监听可读
        ev.data.fd = listenFd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, listenFd_, &ev);
    }

    /**
     * @brief 析构函数，关闭文件描述符
     */
    ~MainReactor() {
        close(listenFd_);
        close(epfd_);
    }

    /**
     * @brief 启动主Reactor事件循环
     */
    void run() {
        std::vector<epoll_event> events(16);  // 预分配事件数组
        while (!stop_) {
            // 等待事件
            int n = epoll_wait(epfd_, events.data(), events.size(), -1);
            for (int i = 0; i < n; ++i) {
                // 只处理监听socket的可读事件
                if (events[i].data.fd == listenFd_) {
                    acceptNewConnection();
                }
            }
        }
    }

   private:
    /**
     * @brief 接受新连接
     *
     * 边缘触发模式下必须循环accept，直到返回EAGAIN
     */
    void acceptNewConnection() {
        while (true) {
            sockaddr_in clientAddr;
            socklen_t len = sizeof(clientAddr);
            // accept4直接创建非阻塞socket
            int connFd =
                accept4(listenFd_, (sockaddr*)&clientAddr, &len, SOCK_NONBLOCK);

            if (connFd < 0) {
                // 没有更多连接可接受
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                perror("accept");
                break;
            }

            std::cout << "新连接 fd=" << connFd << " 来自 "
                      << inet_ntoa(clientAddr.sin_addr) << std::endl;

            // 轮询选择一个SubReactor（负载均衡）
            EventLoop* loop = subLoops_[round_];
            round_ = (round_ + 1) % subLoops_.size();

            // 创建TcpConnection并注册到该EventLoop
            // 注意：必须通过addTask跨线程投递，因为EventLoop运行在不同线程
            loop->addTask([loop, connFd]() {
                // 在SubReactor线程内执行构造和注册
                // 使用shared_ptr管理生命周期
                std::shared_ptr<TcpConnection> conn =
                    std::make_shared<TcpConnection>(connFd, loop);
                // 注意：这里conn是局部变量，函数结束后会销毁
                // 但TcpConnection构造时会通过shared_from_this()获得引用
                // 只要有事件处理，引用计数就不会为0
            });
        }
    }

    int epfd_;                          // 主Reactor的epoll实例
    int listenFd_;                      // 监听socket
    std::vector<EventLoop*> subLoops_;  // SubReactor数组
    int round_;                         // 轮询计数器（负载均衡）
    std::atomic<bool> stop_{false};     // 停止标志
};

// ========== 服务器入口 ==========
/**
 * @brief 主函数，启动主从Reactor模式的Echo服务器
 *
 * 整体架构说明：
 * ┌─────────────────────────────────────────────────────────────┐
 * │                      主从Reactor架构                        │
 * ├─────────────────────────────────────────────────────────────┤
 * │                                                             │
 * │   MainReactor（迎宾员）                                     │
 * │        │                                                    │
 * │        ▼                                                    │
 * │   listen(fd=8888)  ←── 监听新连接                           │
 * │        │                                                    │
 * │        ├── accept()                                         │
 * │        │                                                    │
 * │        ▼                                                    │
 * │   轮询分配 ─────┬─────┬─────┬─────┐                        │
 * │                 │     │     │     │                         │
 * │                 ▼     ▼     ▼     ▼                         │
 * │   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐          │
 * │   │SubReactor│ │SubReactor│ │SubReactor│ │SubReactor│       │
 * │   │  (服务员) │ │  (服务员) │ │  (服务员) │ │  (服务员) │       │
 * │   └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘          │
 * │        │           │           │           │                │
 * │        ▼           ▼           ▼           ▼                │
 * │   conn fd      conn fd      conn fd      conn fd            │
 * │        │           │           │           │                │
 * │        └───────────┴────┬──────┴───────────┘                │
 * │                         ▼                                  │
 * │              WorkerPool（厨房）                             │
 * │              ┌────┬────┬────┐                              │
 * │              │Worker│Worker│Worker│...                      │
 * │              └────┴────┴────┘                              │
 * └─────────────────────────────────────────────────────────────┘
 */
int main() {
    // 1. 创建Worker线程池（单例模式，所有连接共用）
    // 使用CPU核心数作为线程数
    WorkerPool::instance();
    std::cout << "Worker线程池已创建，线程数: "
              << std::thread::hardware_concurrency() << std::endl;

    // 2. 创建4个SubReactor（服务员）
    const int numSubReactors = 4;
    std::vector<EventLoop> loops(numSubReactors);  // 4个EventLoop实例
    std::vector<EventLoop*> loopPtrs;              // 指针数组，传给MainReactor
    std::vector<std::thread> loopThreads;          // SubReactor线程

    for (int i = 0; i < numSubReactors; ++i) {
        loopPtrs.push_back(&loops[i]);
        // 每个EventLoop运行在独立的线程中
        loopThreads.emplace_back([&loops, i]() { loops[i].loop(); });
    }
    std::cout << "SubReactor线程已创建，数量: " << numSubReactors << std::endl;

    // 3. 创建MainReactor（迎宾员），监听端口8888
    MainReactor mainReactor(8888, loopPtrs);
    std::thread mainThread([&mainReactor]() { mainReactor.run(); });

    std::cout << "\n主从Reactor Echo服务器已启动，端口 8888" << std::endl;
    std::cout << "按 Ctrl+C 退出" << std::endl;

    // 4. 等待所有线程结束
    // 实际项目中应该添加信号处理（如SIGINT）来优雅关闭
    mainThread.join();
    for (auto& t : loopThreads) {
        t.join();
    }

    return 0;
}