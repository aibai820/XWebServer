/**
 * @file    main.cpp
 * @brief   程序入口
 *
 * 阶段 1: 最简单的 epoll echo 服务器
 *   编译: cd build && cmake .. && make
 *   运行: ./webserver
 *   或:   ./webserver 8080    (指定端口)
 *   测试: curl http://localhost:9006
 *
 * 命令行参数（后续阶段扩展）：
 *   -p <port>    监听端口 (默认 9006)
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
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 解析命令行参数 ----
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            printf("用法: %s [端口号]\n", argv[0]);
            printf("示例: %s 8080\n", argv[0]);
            return 1;
        }
    }

    // ---- 安装信号处理器 ----
    setup_signals();

    // ---- 启动服务器 ----
    WebServer server;
    server.init(port);
    server.event_listen();

    // 进入 epoll 循环（阻塞直到信号退出）
    server.event_loop();

    return 0;
}
