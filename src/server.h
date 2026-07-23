/**
 * @file    server.h
 * @brief   WebServer 核心类：epoll 事件循环 + 连接管理
 *
 * 架构演进路线（对应各阶段）：
 *   阶段 1（已完成）: 单线程 epoll + echo
 *   阶段 2（已完成）: 单线程 epoll + HTTP 协议解析 + 静态文件
 *   阶段 3（已完成）: epoll + 线程池 + Reactor/Proactor
 *   阶段 4（已完成）: 定时器 + 异步日志 + 数据库连接池 + CGI
 *
 * 阶段 4 新增的核心概念：
 *
 *   1. 定时器（Timer）
 *      - 有序双向链表管理所有活跃连接的过期时间
 *      - 每个连接 15 秒无活动则自动断开
 *      - 每次 I/O 活动自动延长（"续期"）
 *
 *   2. 统一事件源（Unified Event Source）
 *      - socketpair 创建一对 Unix 域 socket
 *      - 信号处理函数只做 send()（异步信号安全）
 *      - 主循环通过 epoll 接收信号 → 可以安全执行任意操作
 *      - 彻底解决了"信号处理函数中不能做复杂操作"的矛盾
 *
 *   3. 异步日志（Async Logging）
 *      - 同步模式：write_log 直接 fputs
 *      - 异步模式：write_log 推入阻塞队列 → 后台线程写盘
 *      - 日志滚动：按天 + 按行数
 *
 *   4. 数据库连接池（DB Connection Pool）
 *      - 单例模式 + RAII
 *      - 信号量控制并发获取
 *      - 启动时加载用户凭据到内存缓存
 */

#ifndef XWEBSERVER_SERVER_H
#define XWEBSERVER_SERVER_H

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <string>
#include <unistd.h>

#include "http/http_connection.h"
#include "pool/threadpool.h"
#include "pool/sql_connection_pool.h"
#include "timer/timer.h"
#include "log/log.h"

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

// 定时器
constexpr int TIMESLOT = 5;         // 定时器时间槽（秒）：超时 = 3 * TIMESLOT = 15s

// 默认配置
constexpr int DEFAULT_CLOSE_LOG  = 0;   // 0=启用日志, 1=禁用
constexpr int DEFAULT_LOG_WRITE  = 0;   // 0=同步日志, 1=异步日志
constexpr int DEFAULT_SQL_NUM    = 8;   // 数据库连接池默认大小
constexpr int DEFAULT_OPT_LINGER = 0;   // SO_LINGER: 0=优雅关闭, 1=强制关闭

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
     * @brief 设置服务器参数（阶段 4 新增日志、数据库、优雅关闭选项）
     *
     * @param port        监听端口
     * @param doc_root    静态文件根目录
     * @param trig_mode   触发模式组合：0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET
     * @param thread_num  线程池大小（默认 8）
     * @param actor_model 并发模式：0=Proactor（默认）, 1=Reactor
     * @param close_log   是否关闭日志：0=启用, 1=禁用
     * @param log_write   日志模式：0=同步, 1=异步
     * @param sql_num     数据库连接池大小（默认 8）
     * @param opt_linger  SO_LINGER 选项：0=优雅关闭, 1=强制关闭
     */
    void init(int port, const char* doc_root, int trig_mode = LT_MODE,
              int thread_num = DEFAULT_THREADS,
              int actor_model = ACTOR_PROACTOR,
              int close_log = DEFAULT_CLOSE_LOG,
              int log_write = DEFAULT_LOG_WRITE,
              int sql_num = DEFAULT_SQL_NUM,
              int opt_linger = DEFAULT_OPT_LINGER,
              const char* sql_user = "",
              const char* sql_passwd = "",
              const char* sql_dbname = "");

    // ----------------------------------------------------------
    // 初始化链（按顺序调用）
    // ----------------------------------------------------------

    /** @brief 解析 trig_mode_ 为 listen_trig_mode_ 和 conn_trig_mode_ */
    void trig_mode();

    /** @brief 初始化同步/异步日志系统 */
    void log_write();

    /** @brief 初始化数据库连接池 + 加载用户凭据缓存 */
    void sql_pool();

    /** @brief 创建线程池 */
    void thread_pool();

    /** @brief 创建 socket、bind、listen、epoll、socketpair、信号、alarm */
    void event_listen();

    /** @brief 进入主事件循环（阻塞，直到收到 SIGTERM 信号） */
    void event_loop();

private:
    // ==========================================================
    // 事件处理函数
    // ==========================================================

    /**
     * @brief 处理新连接（LT/ET 自适应）
     *
     * LT 模式：每次 accept 一个连接
     * ET 模式：循环 accept 直到 EAGAIN
     *
     * @return true=成功, false=失败或连接数已满
     */
    bool dealclientdata();

    /**
     * @brief 处理读事件（Proactor/Reactor 自适应 + 定时器续期）
     *
     * Proactor：主线程 read_once() → 线程池 process()
     * Reactor： 主线程 append(conn, 0) → 线程池 read_once() + process()
     *
     * @param sockfd  客户端 socket fd
     */
    void dealwithread(int sockfd);

    /**
     * @brief 处理写事件（Proactor/Reactor 自适应 + 定时器续期）
     *
     * Proactor：主线程 write()
     * Reactor： 主线程 append(conn, 1) → 线程池 write()
     *
     * @param sockfd  客户端 socket fd
     */
    void dealwithwrite(int sockfd);

    /**
     * @brief 从 socketpair 管道读取信号
     *
     * 统一事件源的核心：将"异步信号"转化为"同步 I/O 事件"。
     * 信号到达时 sig_handler 向 pipefd[1] 写入信号编号，
     * epoll_wait 检测到 pipefd[0] 可读 → 在主循环中同步处理。
     *
     * @param timeout     [out] 是否收到 SIGALRM
     * @param stop_server [out] 是否收到 SIGTERM
     * @return true=成功读取, false=管道错误
     */
    bool dealwithsignal(bool& timeout, bool& stop_server);

    // ==========================================================
    // 定时器管理
    // ==========================================================

    /**
     * @brief 为新连接创建定时器节点并插入有序链表
     *
     * @param connfd          客户端 socket fd
     * @param client_address  客户端地址
     */
    void timer(int connfd, struct sockaddr_in client_address);

    /**
     * @brief 调整定时器过期时间（"续期"）
     *
     * 当客户端有 I/O 活动时调用。将过期时间重置为：
     *   expire = now + 3 * TIMESLOT  (默认 15 秒后)
     * 然后重新定位节点在有序链表中的位置。
     *
     * @param timer  要调整的定时器节点
     */
    void adjust_timer(util_timer* timer);

    /**
     * @brief 处理定时器到期
     *
     * 触发回调函数（关闭连接 + epoll 移除），
     * 从链表中删除节点，释放内存。
     *
     * @param timer   要处理的定时器节点
     * @param sockfd  关联的 socket fd
     */
    void deal_timer(util_timer* timer, int sockfd);

    // ---- 成员变量 ----
    int  port_        = DEFAULT_PORT;    // 监听端口
    int  listenfd_    = -1;              // 监听 socket fd
    int  epollfd_     = -1;              // epoll 实例 fd
    int  trig_mode_   = LT_MODE;         // 触发模式组合（0-3）
    int  thread_num_  = DEFAULT_THREADS; // 线程池大小
    int  actor_model_ = ACTOR_PROACTOR;  // 并发模式

    // 文档根目录
    char doc_root_[256] = "static";

    // 连接数组：以 fd 为索引，存储 HttpConnection 对象
    HttpConnection* users_ = nullptr;

    // 线程池（阶段 3）
    ThreadPool<HttpConnection>* pool_ = nullptr;

    // epoll_wait 返回的事件数组
    epoll_event events_[MAX_EVENT_NUMBER] = {};

    // ==========================================================
    // 阶段 4 新增成员
    // ==========================================================

    // ---- 日志 ----
    int close_log_ = DEFAULT_CLOSE_LOG;   // 日志开关
    int log_write_ = DEFAULT_LOG_WRITE;   // 日志模式：0=同步, 1=异步

    // ---- 数据库 ----
    int sql_num_ = DEFAULT_SQL_NUM;       // 连接池大小
    std::string sql_user_;                // MySQL 用户名
    std::string sql_passwd_;              // MySQL 密码
    std::string sql_dbname_;              // MySQL 数据库名
    connection_pool* m_connPool = nullptr; // 连接池单例指针

    // ---- 触发模式拆分 ----
    int listen_trig_mode_ = LT_MODE;      // 监听 socket 的触发模式
    int conn_trig_mode_   = LT_MODE;      // 连接 socket 的触发模式

    // ---- SO_LINGER ----
    int opt_linger_ = DEFAULT_OPT_LINGER; // 优雅关闭开关

    // ---- 定时器 ----
    client_data* users_timer_ = nullptr;  // 每个连接的 client_data 数组
    int pipefd_[2] = {-1, -1};            // socketpair 管道 fd
    Utils utils_;                          // 定时器工具实例（管理定时器链表 + 信号）

};

#endif  // XWEBSERVER_SERVER_H
