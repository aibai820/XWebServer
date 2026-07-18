/**
 * @file    server.cpp
 * @brief   WebServer 核心实现
 *
 * 学习要点：
 *   1. epoll 是 Linux 下最高效的 I/O 多路复用机制
 *      - select:  O(n) 遍历，fd 上限 1024
 *      - poll:    O(n) 遍历，无 fd 上限但仍有性能问题
 *      - epoll:   O(1) 事件通知，基于红黑树 + 就绪链表
 *
 *   2. 为什么用非阻塞 IO？
 *      - 阻塞 IO 下 accept/recv 无数据会卡住整个线程
 *      - 非阻塞 + epoll 让一个线程可以管理数千个连接
 *
 *   3. EPOLLRDHUP 是 Linux 2.6.17 引入的标志
 *      - 对端关闭连接（发送 FIN）时触发
 *      - 比传统的 recv()==0 判断更优雅
 *
 * 阶段 2 新增：
 *   4. writev 分散/聚集 I/O
 *      - 一次系统调用发送多个不连续的内存块
 *      - 避免 HTTP 响应头复制到文件缓冲区（或反之）
 *
 *   5. mmap 零拷贝
 *      - 文件直接映射到用户空间内存，内核态 copy 一次到 page cache
 *      - 比 read() + write() 少一次内核→用户态的拷贝
 */

#include "server.h"

// ============================================================
// 构造与析构
// ============================================================

WebServer::WebServer() {
    // 预分配连接对象数组
    // 以 fd 为索引直接 O(1) 查找，避免 map 的额外开销
    users_ = new HttpConnection[MAX_FD];
}

WebServer::~WebServer() {
    if (listenfd_ != -1) close(listenfd_);
    if (epollfd_  != -1) close(epollfd_);
    delete[] users_;
}

// ============================================================
// 初始化参数
// ============================================================
void WebServer::init(int port, const char* doc_root, int trig_mode)
{
    port_ = port;
    trig_mode_ = trig_mode;

    // 安全拷贝文档根目录
    strncpy(doc_root_, doc_root, sizeof(doc_root_) - 1);
    doc_root_[sizeof(doc_root_) - 1] = '\0';


    const char* mode_str = (trig_mode_ == ET_MODE) ? "ET" : "LT";
    printf("[初始化] 端口: %d, 根目录: %s, 触发模式: %s\n",
           port_, doc_root_, mode_str);
}

// ============================================================
// 创建 socket、绑定、监听、初始化 epoll
//
// 这是 Linux 网络编程的标准 6 步：
//   socket() → setsockopt() → bind() → listen() → epoll_create() → epoll_ctl()
// ============================================================
void WebServer::event_listen()
{
    // ---- 第 1 步: socket() ----
    // PF_INET = IPv4, SOCK_STREAM = TCP, 0 = 自动选择协议
    listenfd_ = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd_ >= 0);

    // ---- 第 2 步: setsockopt() ----
    // SO_REUSEADDR: 允许重用 TIME_WAIT 状态的地址
    //   服务器重启时不必等 2MSL（约 60 秒），可以立即 bind
    int reuse = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // ---- 第 3 步: bind() ----
    // 把 socket 绑定到特定 IP:Port
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));             // IPv4
    address.sin_family = AF_INET;                     // 监听所有网卡（0.0.0.0）
    address.sin_addr.s_addr = htonl(INADDR_ANY);      // 主机字节序 → 网络字节序
    address.sin_port = htons(port_);

    int ret = bind(listenfd_, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    // ---- 第 4 步: listen() ----
    // backlog = 5: 已完成三次握手但尚未 accept 的连接队列长度
    // 内核实际值 = min(backlog, /proc/sys/net/core/somaxconn)
    ret = listen(listenfd_, 5);
    assert(ret >= 0);
  
    // ---- 第 5 步: epoll_create() ----
    // Linux 2.6.8+ 之后，参数只要 >0 即可，大小由内核动态管理
    epollfd_ = epoll_create(5);
    assert(epollfd_ != -1);
    
    // ---- 第 6 步: epoll_ctl() 注册 listenfd ----
    // 监听 socket 用 LT 模式（listenfd 不需要 ET，accept 每次只取一个连接）
    add_fd_to_epoll(epollfd_, listenfd_, false, LT_MODE);

    // 设置全局 epollfd，HttpConnection 的静态方法会用到
    HttpConnection::epoll_fd_ = epollfd_;

    const char* mode_str = (trig_mode_ == ET_MODE) ? "ET" : "LT"; 
    printf("[就绪] 服务器监听在 http://0.0.0.0:%d  (epoll fd=%d, 连接模式: %s)\n",
           port_, epollfd_, mode_str);
    printf("[提示] 用浏览器访问 http://localhost:%d 测试\n", port_);
    printf("[提示] 或者用 curl http://localhost:%d\n", port_);
}

// ============================================================
// 主事件循环
//
// epoll_wait 的参数：
//   epollfd:  epoll 实例
//   events:   输出参数，存放就绪的事件
//   maxevents: 最多返回多少个事件
//   timeout:  阻塞时间（毫秒），-1 = 永久阻塞直到有事件
//
// 返回值：
//   >0: 就绪的事件数量
//   0:  超时（timeout 到期）
//   -1: 出错（通常是被信号中断，errno==EINTR 可以重试）
// ============================================================
void WebServer::event_loop()
{
    bool running = true;

    printf("\n========== 进入事件循环 ==========\n\n");

    while (running)
    {
        // 阻塞等待事件（-1 = 没有事件就永久等待）
        int ready_count = epoll_wait(epollfd_, events_, MAX_EVENT_NUMBER, -1);

        if (ready_count < 0)
        {
            if (errno == EINTR)
            {
                // 被信号中断，继续等待
                continue;
            }
            printf("[错误] epoll_wait 失败：%s\n", strerror(errno));
            break;
        }

        // 遍历所有就绪事件
        for (int i = 0; i < ready_count; i++)
        {
            int sockfd = events_[i].data.fd;
            uint32_t revents = events_[i].events;

            // ---- 情况 1: 新连接到达 ----
            if (sockfd == listenfd_)
            {
                handle_new_connection();
            }
            // ---- 情况 2: 对端关闭连接 ----
            else if (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                handle_close(sockfd);
            }
            // ---- 情况 3: 客户端数据到达 ----
            else if (revents & EPOLLIN)
            {
                handle_read(sockfd);
            }
            // ---- 情况 4: socket可写 ----
            else if (revents & EPOLLOUT)
            {
                handle_write(sockfd);
            }
            // ---- 情况 5: 未知事件 ----
            else
            {
                printf("[警告] fd=%d 未知事件：0x%x\n", sockfd, revents);
            } 
        } 
    }

    printf("\n[退出] 事件循环结束\n");
    
}

// ============================================================
// 处理新连接：accept → 创建 HttpConnection
// ============================================================
void WebServer::handle_new_connection()
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // accept() 从已完成三次握手的队列中取出一个连接
    // 返回的是新的 socket fd，用于和这个客户端通信
    int connfd = accept(listenfd_, (struct sockaddr*)&client_addr, &client_len);

    if (connfd < 0)
    {
        printf("[错误] accep 失败：%s\n", strerror(errno));
        return;
    }

    // 连接数上限检查
    if (HttpConnection::user_count_ >= MAX_FD)
    {
        printf("[拒绝] 连接数已达上限 %d\n", MAX_FD);
        const char* msg = "HTTP/1.1 503 Service Unavailable\r\n"
                          "Content-Length: 0\r\n\r\n";
        send(connfd, msg, strlen(msg), 0);
        close(connfd);
        return;
    }

    // 初始化连接对象（传入文档根目录和触发模式）
    users_[connfd].init(connfd, client_addr, doc_root_, trig_mode_);

}

// ============================================================
// 处理读事件：读取数据 → 处理 → 写回
// ============================================================
void WebServer::handle_read(int sockfd)
{
    HttpConnection& conn = users_[sockfd];

    // 读数据
    if (!conn.read_once()) {
        // 读取失败 → 客户端断开或出错
        handle_close(sockfd);
        return;
    }

    // 阶段 2: HTTP 请求处理
    // process() 内部会 process_read() → process_write() → modify_fd(EPOLLOUT)
    // 然后由 handle_write() → write() 实际发送数据
    conn.process();
}

// ============================================================
// 处理写事件（阶段 1 暂未使用，阶段 2 处理大文件分块发送）
// ============================================================
void WebServer::handle_write(int sockfd)
{
    HttpConnection& conn = users_[sockfd];
    // write() 返回 false 表示：
    //   1. 发送出错 → 关闭连接
    //   2. 发送完毕且 Connection: close → 关闭连接
    if (!conn.write()) {
        handle_close(sockfd);
    }
    // write() 返回 true 表示：
    //   1. EAGAIN（缓冲区满）→ 已重新注册 EPOLLOUT，等待下次触发
    //   2. 发送完毕且 keep-alive → 已重新注册 EPOLLIN + 重置状态，等待下一个请求
}

// ============================================================
// 处理连接关闭
// ============================================================
void WebServer::handle_close(int sockfd)
{
    users_[sockfd].close_conn();
}
