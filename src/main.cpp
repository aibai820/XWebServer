/**
 * @file    main.cpp
 * @brief   程序入口
 *
 * 阶段 1（已完成）: 最简单的 epoll echo 服务器
 * 阶段 2（已完成）: HTTP 静态文件服务器
 * 阶段 3（已完成）: 并发服务器 —— epoll + 线程池 + Reactor/Proactor
 * 阶段 4（待实现）: 生产级服务器 —— 定时器 + 异步日志 + 数据库连接池
 *
 * 编译: cd build && cmake .. && make
 *
 * 运行:
 *   ./webserver                                    # 全部默认（端口 9006, Proactor, 8 线程）
 *   ./webserver 8080                               # 指定端口
 *   ./webserver 8080 ./static                      # 指定端口和根目录
 *   ./webserver 8080 ./static lt                   # 指定端口、根目录、LT 模式
 *   ./webserver 8080 ./static et 4                 # ET 模式、4 个工作线程
 *   ./webserver 8080 ./static et 8 reactor         # ET 模式、8 线程、Reactor
 *   ./webserver 8080 ./static lt 6 proactor        # LT 模式、6 线程、Proactor（默认）
 *
 * 测试:
 *   curl http://localhost:9006
 *   # 或使用压力测试工具
 *   wrk -t4 -c100 -d10s http://localhost:9006/
 */

#include<csignal>
#include<cstdlib>
#include"server.h"
// 全局指针，用于信号处理函数中访问 server
// （信号处理函数必须是 C 风格函数，无法访问 this）
WebServer* g_server = nullptr;

// ----------------------------------------------------------
// 信号处理：优雅退出
// SIGINT  (Ctrl+C)  → 退出
// SIGTERM (kill)    → 退出
// SIGPIPE           → 忽略（向已关闭的 socket 写数据会触发）
// ----------------------------------------------------------
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[信号] 收到退出信号 %d，正在关闭...\n", sig);
        // 简单的退出方式：这里没有做复杂清理
        // 阶段 4 会实现完整的优雅关闭
        exit(0);
    }
}

void setup_signals() {
    // SIGPIPE: 向已关闭的 socket 写入数据时触发，默认行为是终止进程
    // 我们不希望因此挂掉，忽略它，通过 send() 返回值 -1 + errno==EPIPE 来处理
    signal(SIGPIPE, SIG_IGN);

    // SIGINT / SIGTERM: 优雅退出
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
}

// ============================================================
// 用法
// ============================================================
void print_usage(const char* prog) {
    printf("用法: %s [端口] [根目录] [触发模式] [线程数] [并发模式]\n\n", prog);
    printf("参数说明:\n");
    printf("  %-20s 监听端口 (默认 %d)\n",       "端口",     DEFAULT_PORT);
    printf("  %-20s 文档根目录 (默认 ./static)\n", "根目录");
    printf("  %-20s 触发模式: lt(默认) 或 et\n",   "触发模式");
    printf("  %-20s 线程池大小 (默认 %d)\n",       "线程数",  DEFAULT_THREADS);
    printf("  %-20s 并发模型: proactor(默认) 或 reactor\n", "并发模式");
    printf("\n");
    printf("并发模式说明:\n");
    printf("  proactor: 主线程负责全部 I/O，工作线程只做 HTTP 处理\n");
    printf("  reactor:  主线程只做事件分发，工作线程负责 I/O + HTTP 处理\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s                                          # 全部默认\n", prog);
    printf("  %s  8080                                    # 指定端口\n", prog);
    printf("  %s  8080  ./static  et  4                   # ET 模式, 4 线程\n", prog);
    printf("  %s  8080  ./static  lt  8  reactor          # 8 线程, Reactor\n", prog);
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 解析命令行参数 ----
    //
    // 参数位置约定（简单的位置参数解析，避免依赖 getopt）:
    //   argv[1] = 端口号
    //   argv[2] = 文档根目录
    //   argv[3] = 触发模式 (lt / et)
    //   argv[4] = 线程数
    //   argv[5] = 并发模式 (proactor / reactor)
    //
    int  port      = DEFAULT_PORT;
    int  trig_mode = LT_MODE;
    int  thread_num  = DEFAULT_THREADS;
    int  actor_model = ACTOR_PROACTOR;
    const char* doc_root = "static";

    // 帮助参数（-h 或 --help 可以出现在任何位置）
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // 简单的位置参数解析（避免依赖getopt，跨平台兼容）
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            printf("错误: 无效端口号 '%s'\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3)
    {
        doc_root = argv[2];
    }
    if (argc >= 4)
    {
        if (strcmp(argv[3], "et") == 0 || strcmp(argv[3], "ET") == 0) 
        {
            trig_mode = ET_MODE;
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
        // "proactor" 或 "Proactor" 就用默认的 ACTOR_PROACTOR
    }
    
    const char* mode_str  = (trig_mode == ET_MODE) ? "ET" : "LT";
    const char* actor_str = (actor_model == ACTOR_REACTOR) ? "Reactor" : "Proactor";

    printf("========================================\n");
    printf("  My WebServer — Phase 2\n");
    printf("  HTTP/1.1 静态文件服务器\n");
    printf("========================================\n\n");
    printf("[配置] 端口=%d, 根目录=%s, 触发模式=%s, 线程数=%d, 并发模式=%s\n\n",
           port, doc_root, mode_str, thread_num, actor_str);

    // ---- 安装信号处理器 ----
    setup_signals();

    // ---- 启动服务器 ----
    WebServer server;
    server.init(port, doc_root, trig_mode, thread_num, actor_model);
    server.event_listen();

    // 进入 epoll 循环（阻塞直到信号退出）
    server.event_loop();

    return 0;
}
