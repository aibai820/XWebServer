/**
 * @file    server.h
 * @brief   WebServer 核心类：epoll 事件循环 + 连接管理
 *
 * 架构演进路线（对应各阶段）：
 *   阶段 1:         单线程 epoll + echo
 *   阶段 2（当前）: 单线程 epoll + HTTP 协议解析 + 静态文件
 *   阶段 3:         epoll + 线程池 + Reactor/Proactor
 *   阶段 4:         定时器 + 日志 + 数据库连接池
 *
 * 当前阶段的核心流程：
 *   socket() → bind() → listen()
 *   epoll_create() → epoll_ctl(ADD listenfd)
 *   loop:
 *     epoll_wait() → accept 新连接 or HTTP 请求处理 or 写响应
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

// ============================================================
// 服务器配置常量
// ============================================================
constexpr int  MAX_FD           = 65536;   // 最大文件描述符数
constexpr int  MAX_EVENT_NUMBER = 10000;   // epoll_wait 一次最多返回的事件数
constexpr int  DEFAULT_PORT     = 9006;    // 默认监听端口
constexpr int  LT_MODE          = 0;       // 水平触发
constexpr int  ET_MODE          = 1;       // 边缘触发

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

    // 设置服务器参数
    // @param port      监听端口
    // @param doc_root  静态文件根目录
    // @param trig_mode 触发模式：0=LT, 1=ET（默认 LT）
    void init(int port, const char* doc_root, int trig_mode = LT_MODE);

    // 创建 socket、bind、listen、初始化 epoll
    void event_listen();

    // 进入主事件循环（阻塞，直到收到终止信号）
    void event_loop();

private:
    // ---- 事件处理函数 ----
    // 处理新连接：accept → 创建 HttpConnection → 注册到 epoll
    void handle_new_connection();

    // 处理客户端数据到达：读取数据 → HTTP 处理 → 注册 EPOLLOUT
    void handle_read(int sockfd);

    // 处理 socket 可写事件：writev 发送 HTTP 响应
    void handle_write(int sockfd);

    // 处理连接关闭
    void handle_close(int sockfd);

    // ---- 成员变量 ----
    int  port_      = DEFAULT_PORT;   // 监听端口
    int  listenfd_  = -1;            // 监听 socket
    int  epollfd_   = -1;            // epoll 实例
    int  trig_mode_ = LT_MODE;       // 触发模式
    
    // 文档根目录
    char doc_root_[256] = "static";
    
    // 连接数组：以 fd 为索引，存储 HttpConnection 对象
    // 阶段 1 用固定数组（简单），后续可改为 unordered_map 按需分配
    HttpConnection* users_ = nullptr;

    // epoll_wait 返回的事件数组
    epoll_event events_[MAX_EVENT_NUMBER] = {};

};

#endif  // XWEBSERVER_SERVER_H
