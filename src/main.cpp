/**
 * @file    main.cpp
 * @brief   程序入口
 *
 * 阶段 1: 最简单的 epoll echo 服务器
 * 阶段 2（当前）: HTTP 静态文件服务器
 *   编译: cd build && cmake .. && make
 *   运行: ./webserver
 *   或:   ./webserver 8080            (指定端口)
 *   或:   ./webserver 8080 ./static   (指定端口和根目录)
 *   或:   ./webserver 8080 ./static e (e 表示 ET 模式)
 *   测试: curl http://localhost:9006
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
    printf("用法: %s [端口] [根目录] [模式]\n", prog);
    printf("  %-20s 监听端口 (默认 %d)\n",   "端口",   DEFAULT_PORT);
    printf("  %-20s 文档根目录 (默认 ./static)\n", "根目录");
    printf("  %-20s 触发模式: lt(默认) 或 et\n",   "模式");
    printf("\n示例:\n");
    printf("  %s                            # 全部默认\n", prog);
    printf("  %s  8080                      # 指定端口\n", prog);
    printf("  %s  8080  ./static            # 端口 + 目录\n", prog);
    printf("  %s  8080  ./static  et        # 端口 + 目录 + ET 模式\n", prog);
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 解析命令行参数 ----
    int  port      = DEFAULT_PORT;
    int  trig_mode = LT_MODE;
    const char* doc_root = "static";

    // 简单的位置参数解析（避免依赖getopt，跨平台兼容）
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535)
        {
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
    
    printf("========================================\n");
    printf("  My WebServer — Phase 2\n");
    printf("  HTTP/1.1 静态文件服务器\n");
    printf("========================================\n\n");

    // ---- 安装信号处理器 ----
    setup_signals();

    // ---- 启动服务器 ----
    WebServer server;
    server.init(port, doc_root, trig_mode);
    server.event_listen();

    // 进入 epoll 循环（阻塞直到信号退出）
    server.event_loop();

    return 0;
}
