/**
 * @file    server.h
 * @brief   WebServer 核心类：epoll 事件循环 + 连接管理
 *
 * 架构演进路线（对应各阶段）：
 *   阶段 1（已完成）: 单线程 epoll + echo
 *   阶段 2（已完成）: 单线程 epoll + HTTP 协议解析 + 静态文件
 *   阶段 3（当前）: epoll + 线程池 + Reactor/Proactor
 *   阶段 4: 定时器 + 日志 + 数据库连接池
 *
 * 阶段 3 新增的核心概念：
 *
 *   1. 线程池（ThreadPool）
 *      - 预先创建一组工作线程，避免频繁创建/销毁线程的开销
 *      - 主线程将任务放入队列，工作线程从队列取任务
 *      - 用信号量实现生产者-消费者同步
 *
 *   2. Reactor vs Proactor 并发模式
 *      ┌──────────────┬─────────────────┬─────────────────┐
 *      │              │    Reactor       │    Proactor      │
 *      ├──────────────┼─────────────────┼─────────────────┤
 *      │ I/O 执行者   │ 工作线程         │ 主线程           │
 *      │ 主线程职责   │ 事件分发         │ 事件分发 + I/O   │
 *      │ 工作线程职责 │ I/O + 业务逻辑   │ 纯业务逻辑       │
 *      │ 适用场景     │ 通用             │ I/O 快、逻辑重   │
 *      └──────────────┴─────────────────┴─────────────────┘
 *
 *   3. EPOLLONESHOT
 *      - 一个 socket 同一时刻只被一个线程处理
 *      - 处理完后需要重新注册（modify_fd），否则不再触发事件
 *      - 阶段 2 已在 add_fd_to_epoll 中启用，本阶段正式发挥作用
 */

#ifndef XWEBSERVER_SERVER_H
#define XWEBSERVER_SERVER_H

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <unistd.h>

#include "http/http_connection.h"
#include "pool/threadpool.h"

// ============================================================
// 服务器配置常量
// ============================================================
constexpr int  MAX_FD           = 65536;   // 最大文件描述符数
constexpr int  MAX_EVENT_NUMBER = 10000;   // epoll_wait 一次最多返回的事件数
constexpr int  DEFAULT_PORT     = 9006;    // 默认监听端口
constexpr int  DEFAULT_THREADS  = 8;       // 默认线程数
constexpr int  LT_MODE          = 0;       // 水平触发
constexpr int  ET_MODE          = 1;       // 边缘触发


// 并发模式常量
constexpr int ACTOR_PROACTOR = 0;  // Proactor: 主线程做 I/O
constexpr int ACTOR_REACTOR  = 1;  // Reactor:  工作线程做 I/O

class WebServer
{
public:
    WebServer();
    ~WebServer();

    // 禁止拷贝赋值（管理裸指针和fd）
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // ----------------------------------------------------------
    // 初始化与运行
    // ----------------------------------------------------------

    /**
     * @brief 设置服务器参数（阶段 3 新增线程数 + 并发模式）
     *
     * @param port        监听端口
     * @param doc_root    静态文件根目录
     * @param trig_mode   触发模式：0=LT, 1=ET（默认 LT）
     * @param thread_num  线程池大小（默认 8）
     * @param actor_model 并发模式：0=Proactor（默认）, 1=Reactor
     */
    void init(int port, const char* doc_root, int trig_mode = LT_MODE,
              int thread_num = DEFAULT_THREADS, int actor_model = ACTOR_PROACTOR);

    // 创建 socket、bind、listen、初始化 epoll
    void event_listen();

    // 进入主事件循环（阻塞，直到收到终止信号）
    void event_loop();

private:
    // ---- 事件处理函数 ----
    // 处理新连接：accept → 创建 HttpConnection → 注册到 epoll
    void handle_new_connection();

    /**
     * @brief 处理读事件（阶段 3：根据并发模式分发到线程池）
     *
     * Proactor 模式：主线程亲自 read_once()，然后 append_p() 交线程池处理
     * Reactor 模式： 主线程 append(conn, 0)，工作线程负责 read_once()
     */
    void handle_read(int sockfd);

    /**
     * @brief 处理写事件（阶段 3：根据并发模式分发到线程池）
     *
     * Proactor 模式：主线程亲自 write()，后续由 write() 内部处理
     * Reactor 模式： 主线程 append(conn, 1)，工作线程负责 write()
     */
    void handle_write(int sockfd);

    // 处理连接关闭
    void handle_close(int sockfd);

    // ---- 成员变量 ----
    int  port_      = DEFAULT_PORT;   // 监听端口
    int  listenfd_  = -1;            // 监听 socket
    int  epollfd_   = -1;            // epoll 实例
    int  trig_mode_ = LT_MODE;       // 触发模式
    int  thread_num_  = DEFAULT_THREADS; // 线程池大小
    int  actor_model_ = ACTOR_PROACTOR;  // 并发模式

    // 文档根目录
    char doc_root_[256] = "static";
    
    // 连接数组：以 fd 为索引，存储 HttpConnection 对象
    HttpConnection* users_ = nullptr;

    // 线程池（阶段 3 新增）
    ThreadPool<HttpConnection>* pool_ = nullptr;
    
    // epoll_wait 返回的事件数组
    epoll_event events_[MAX_EVENT_NUMBER] = {};

};

#endif  // XWEBSERVER_SERVER_H
