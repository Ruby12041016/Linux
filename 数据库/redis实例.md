# 实例
## 整体

1. RedisConn：单条连接封装（封装 redisContext，ping、释放、重连）
2. RedisPool：连接池核心（队列存空闲连接、互斥锁、条件变量、配置参数）
3. 对外工具类：简易调用入口

```text
RedisConfig 配置类
    └─ host、port、pwd、db、最大连接、最小空闲、超时、连接超时

RedisConn 单连接对象
    属性：redisContext* ctx; bool valid;
    方法：Connect() 建立连接、Ping() 检测存活、Reconnect() 重连、Close() 关闭

RedisPool 连接池管理器
    成员：
        queue<RedisConn*> idle_conns; // 空闲连接队列
        mutex mtx;                    // 互斥锁
        condition_variable cv;        // 等待连接条件变量
        vector<RedisConn*> all_conns; // 所有已创建连接
        RedisConfig cfg;
        int cur_conn_num;             // 当前总连接数
    核心方法：
        RedisConn* GetConn();   // 取出连接
        void ReturnConn(RedisConn* conn); // 归还连接
        void InitMinIdle();     // 初始化最小空闲连接
        void Destroy();         // 销毁全部连接
```

## 相关API
1. `redisConnectWithTimeout()`：带超时创建 redis 连接上下文
2. `redisFree()`：关闭连接、释放 redisContext
3. `redisCommand()`：执行 redis 命令（可变参数）
4. `redisvCommand()`：可变参数版本，内部封装用
5. `freeReplyObject()`：释放 redisReply 结果，``必须手动释放``

## 核心结构体
1. redisContext：Redis 连接句柄，代表一条 TCP 连接，存放 socket、错误信息，等同于 MYSQL。
2. redisReply：命令返回结果载体，统一接收所有指令返回数据，等同于 MYSQL_RES。
### redisReply 重要成员
- type：返回数据类型（字符串 / 数组 / 数字 / 空 / 错误）
- str：字符串结果；
- integer：数字结果
- elements：数组内元素总数
- element[]：数组类型结果的子元素数组（List、Set 等多条数据）

---
```cpp
redis_pool.h
#pragma once
#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>

class RedisPool {
public:
    // 构造：redis地址、端口、密码、最大连接数
    RedisPool(const std::string& host, int port, const std::string& passwd, int maxConn);
    ~RedisPool();

    // 获取连接
    redisContext* getConnection();
    // 归还连接
    void releaseConnection(redisContext* ctx);

private:
    // 创建单个连接
    redisContext* createConnection();

    std::string host_;
    int port_;
    std::string passwd_;
    int maxConn_;            // 最大连接数量
    std::queue<redisContext*> free_conns_; // 空闲连接队列

    std::mutex mtx_;
    std::condition_variable cond_;
};

```
---
```cpp
redis_pool.cpp
#include "redis_pool.h"
#include <iostream>

RedisPool::RedisPool(std::string host, int port, std::string passwd, int maxConn)
    : host_(host), port_(port), passwd_(passwd), maxConn_(maxConn)
{
    // 初始化预先创建一部分连接（简单版：初始0，需要时动态创建）
}

RedisPool::~RedisPool() {
    std::lock_guard<std::mutex> lock(mtx_);
    while (!free_conns_.empty()) {
        redisContext* ctx = free_conns_.front();
        free_conns_.pop();
        redisFree(ctx);
    }
}

redisContext* RedisPool::createConnection() {
    redisContext* ctx = redisConnect(host_.c_str(), port_);
    if (!ctx || ctx->err) {
        std::cerr << "redis connect error:" << (ctx ? ctx->errstr : "null") << std::endl;
        if(ctx) redisFree(ctx);
        return nullptr;
    }
    // 如果有密码，认证
    if (!passwd_.empty()) {
        redisReply* r = (redisReply*)redisCommand(ctx, "AUTH %s", passwd_.c_str());
        if(r == nullptr || r->type == REDIS_REPLY_ERROR) {
            std::cerr << "redis auth fail" << std::endl;
            freeReplyObject(r);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(r);
    }
    return ctx;
}

redisContext* RedisPool::getConnection() {
    std::unique_lock<std::mutex> lock(mtx_);

    // 队列不为空，直接取
    if (!free_conns_.empty()) {
        redisContext* c = free_conns_.front();
        free_conns_.pop();
        return c;
    }

    // 没有空闲连接，判断总连接是否到达上限
    // 简易策略：这里简化，直接新建；生产要统计总连接数做限制
    lock.unlock();
    return createConnection();
}

void RedisPool::releaseConnection(redisContext* ctx) {
    if (!ctx) return;
    std::lock_guard<std::mutex> lock(mtx_);
    // 检测连接是否异常，如果断连直接释放，不回池
    if (ctx->err) {
        redisFree(ctx);
        return;
    }
    free_conns_.push(ctx);
    cond_.notify_one();
}
```
---

```cpp
main.cpp
#include "redis_pool.h"
#include <iostream>

int main() {
    // 创建连接池，最大20条连接
    RedisPool pool("127.0.0.1", 6379, "", 20);

    // 1. 获取连接
    redisContext* conn = pool.getConnection();

    // 执行命令
    redisReply* rep = (redisReply*)redisCommand(conn, "SET username zhangsan");
    freeReplyObject(rep);

    rep = (redisReply*)redisCommand(conn, "GET username");
    if(rep->type == REDIS_REPLY_STRING) {
        std::cout << "result:" << rep->str << std::endl;
    }
    freeReplyObject(rep);

    // 重点！！归还连接，不要调用redisFree
    pool.releaseConnection(conn);
    return 0;
}
```
