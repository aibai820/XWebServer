/**
 * @file    main.cpp
 * @brief   程序入口 —— 参数解析 + 初始化链
 *
 * 阶段 1（已完成）: 最简单的 epoll echo 服务器
 * 阶段 2（已完成）: HTTP 静态文件服务器
 * 阶段 3（已完成）: 并发服务器 —— epoll + 线程池 + Reactor/Proactor
 * 阶段 4（已完成）: 生产级服务器 —— 定时器 + 异步日志 + 数据库连接池 + CGI
 *
 * 编译: cd build && cmake .. && make
 *
 * 运行:
 *   ./webserver                                    # 全部默认
 *   ./webserver 8080                               # 指定端口
 *   ./webserver 8080 ./static                      # 指定端口和根目录
 *   ./webserver 8080 ./static et                   # 指定端口、根目录、ET 模式
 *   ./webserver 8080 ./static lt 8 proactor        # 全参数
 *
 * 初始化链（阶段 4）:
 *   1. server.init()       — 保存配置参数
 *   2. server.trig_mode()   — 解析触发模式组合
 *   3. server.log_write()   — 初始化日志系统
 *   4. server.sql_pool()    — 初始化数据库连接池 + 加载用户凭据
 *   5. server.thread_pool() — 创建线程池
 *   6. server.event_listen() — 创建 socket + epoll + 信号 + 定时器
 *   7. server.event_loop()  — 进入主事件循环
 *
 * 阶段 4 信号处理改进：
 *   不再使用简单 signal() + exit()，而是通过 socketpair 统一事件源：
 *   SIGALRM → sig_handler → pipefd[1] → pipefd[0] → epoll → dealwithsignal
 *   SIGTERM → sig_handler → pipefd[1] → pipefd[0] → epoll → stop_server
 */

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include "server.h"

// ============================================================
// MySQL 数据库配置
//
// 修改以下内容以匹配你的 MySQL 环境。
// 需要预先创建数据库和 user 表：
//
//   CREATE DATABASE webserver;
//   USE webserver;
//   CREATE TABLE user(
//       username char(50) NULL,
//       passwd   char(50) NULL
//   ) ENGINE=InnoDB;
//
// 如果不需要数据库功能（纯静态文件服务器），
// 可以保持以下默认值，服务器仍可正常运行。
// ============================================================
#ifndef MYSQL_USER
#define MYSQL_USER     "root"
#endif

#ifndef MYSQL_PASSWD
#define MYSQL_PASSWD   "root"
#endif

#ifndef MYSQL_DBNAME
#define MYSQL_DBNAME   "webserver"
#endif

// ============================================================
// 用法
// ============================================================
void print_usage(const char* prog) {
    printf("用法: %s [端口] [根目录] [触发模式] [线程数] [并发模式]\n\n", prog);
    printf("参数说明:\n");
    printf("  %-20s 监听端口 (默认 %d)\n",       "端口",     DEFAULT_PORT);
    printf("  %-20s 文档根目录 (默认 ./static)\n", "根目录");
    printf("  %-20s 触发模式: lt(默认) 或 et 或 0-3\n", "触发模式");
    printf("  %-20s 线程池大小 (默认 %d)\n",       "线程数",  DEFAULT_THREADS);
    printf("  %-20s 并发模型: proactor(默认) 或 reactor\n", "并发模式");
    printf("\n");
    printf("触发模式组合 (trig_mode):\n");
    printf("  0 = LT + LT  (默认，全部水平触发)\n");
    printf("  1 = LT + ET  (推荐，listen用LT，连接用ET)\n");
    printf("  2 = ET + LT\n");
    printf("  3 = ET + ET\n");
    printf("\n");
    printf("并发模式说明:\n");
    printf("  proactor: 主线程负责全部 I/O，工作线程只做 HTTP 处理\n");
    printf("  reactor:  主线程只做事件分发，工作线程负责 I/O + HTTP 处理\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s                                          # 全部默认\n", prog);
    printf("  %s  8080                                    # 指定端口\n", prog);
    printf("  %s  8080  ./static  1  8  reactor           # LT+ET, 8线程, Reactor\n", prog);
    printf("\n");
    printf("阶段 4 新增功能:\n");
    printf("  - 定时器：15 秒无活动自动断开连接\n");
    printf("  - 异步日志：日志文件 ./ServerLog_<日期>\n");
    printf("  - CGI 登录/注册 (需要 MySQL)\n");
    printf("  - 优雅退出 (kill 或 Ctrl+C 触发 SIGTERM)\n");
    printf("\n");
    printf("MySQL 配置: 编辑 src/main.cpp 中的 MYSQL_USER/MYSQL_PASSWD/MYSQL_DBNAME\n");
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 默认配置 ----
    int  port        = DEFAULT_PORT;
    int  trig_mode   = LT_MODE;          // 触发模式组合：0=LT+LT
    int  thread_num  = DEFAULT_THREADS;
    int  actor_model = ACTOR_PROACTOR;
    const char* doc_root = "static";

    // 阶段 4 新增配置
    int  close_log   = DEFAULT_CLOSE_LOG;    // 0=启用日志, 1=禁用
    int  log_write   = DEFAULT_LOG_WRITE;    // 0=同步日志, 1=异步
    int  sql_num     = DEFAULT_SQL_NUM;      // 数据库连接池大小
    int  opt_linger  = DEFAULT_OPT_LINGER;   // SO_LINGER 选项

    // MySQL 凭据
    std::string sql_user   = MYSQL_USER;
    std::string sql_passwd = MYSQL_PASSWD;
    std::string sql_dbname = MYSQL_DBNAME;

    // ---- 帮助参数 ----
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // ---- 解析位置参数 ----
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            printf("错误: 无效端口号 '%s'\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        doc_root = argv[2];
    }
    if (argc >= 4) {
        // 触发模式：可以是 "et"/"lt" 或是数字 0-3
        if (strcmp(argv[3], "et") == 0 || strcmp(argv[3], "ET") == 0) {
            trig_mode = 1;  // LT + ET
        } else if (strcmp(argv[3], "lt") == 0 || strcmp(argv[3], "LT") == 0) {
            trig_mode = 0;  // LT + LT
        } else {
            trig_mode = atoi(argv[3]);
            if (trig_mode < 0 || trig_mode > 3) {
                printf("错误: 无效触发模式 '%s'（应为 0-3 或 lt/et）\n", argv[3]);
                return 1;
            }
        }
    }
    if (argc >= 5) {
        thread_num = atoi(argv[4]);
        if (thread_num <= 0 || thread_num > 1024) {
            printf("错误: 无效线程数 '%s'（范围: 1-1024）\n", argv[4]);
            return 1;
        }
    }
    if (argc >= 6) {
        if (strcmp(argv[5], "reactor") == 0 || strcmp(argv[5], "Reactor") == 0) {
            actor_model = ACTOR_REACTOR;
        }
        // "proactor" 就用默认的 ACTOR_PROACTOR
    }

    // ---- 打印配置摘要 ----
    printf("========================================\n");
    printf("  My WebServer — Phase 4\n");
    printf("  生产级高并发 HTTP 服务器\n");
    printf("========================================\n\n");
    printf("[配置] 端口=%d, 根目录=%s\n", port, doc_root);
    printf("[配置] 触发模式=%d, 线程数=%d, 并发=%s, SO_LINGER=%d\n",
           trig_mode, thread_num,
           (actor_model == ACTOR_REACTOR) ? "Reactor" : "Proactor",
           opt_linger);
    printf("[配置] 日志=%s, 日志模式=%s, DB连接=%d\n",
           close_log ? "关闭" : "启用",
           log_write ? "异步" : "同步",
           sql_num);
    printf("[配置] MySQL: user=%s, db=%s\n\n",
           sql_user.c_str(), sql_dbname.c_str());

    // ============================================================
    // 阶段 4 初始化链
    //
    // 顺序很重要！每个步骤依赖于前一步的资源：
    //   log_write → sql_pool → thread_pool → event_listen → event_loop
    //
    // 为什么是这个顺序？
    //   1. log_write 先初始化，后续的 sql_pool/event_listen 需要 LOG_INFO
    //   2. sql_pool 在线程池之前，因为线程池构造函数需要 connection_pool*
    //   3. thread_pool 在 event_listen 之前，因为 event_loop 中就开始用线程池
    //   4. event_listen 最后做，因为它启动 alarm() 定时器
    // ============================================================

    // ---- 第 1 步：保存配置 ----
    WebServer server;
    server.init(port, doc_root, trig_mode, thread_num, actor_model,
                close_log, log_write, sql_num, opt_linger,
                sql_user.c_str(), sql_passwd.c_str(), sql_dbname.c_str());

    // ---- 第 2 步：解析触发模式组合 ----
    server.trig_mode();

    // ---- 第 3 步：初始化日志系统 ----
    server.log_write();

    // ---- 第 4 步：初始化数据库连接池 + 加载用户凭据 ----
    server.sql_pool();

    // ---- 第 5 步：创建线程池 ----
    server.thread_pool();

    // ---- 第 6 步：创建 socket + epoll + 信号 + 定时器 ----
    server.event_listen();

    // ---- 第 7 步：进入主事件循环（阻塞直到 SIGTERM）----
    server.event_loop();

    // 优雅退出：WebServer 析构函数自动清理所有资源
    //   - 关闭所有 socket 和 pipefd
    //   - 释放 HttpConnection 和 client_data 数组
    //   - 线程池析构（stop + join + cleanup）
    //   - 定时器链表析构（释放所有节点）
    //   - 日志文件关闭
    //   - 数据库连接池销毁
    return 0;
}