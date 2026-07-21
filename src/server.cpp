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
 *   4. writev 分散/聚集 I/O
 *      - 一次系统调用发送多个不连续的内存块
 *      - 避免 HTTP 响应头复制到文件缓冲区（或反之）
 *
 *   5. mmap 零拷贝
 *      - 文件直接映射到用户空间内存，内核态 copy 一次到 page cache
 *      - 比 read() + write() 少一次内核→用户态的拷贝
 *
 * 阶段 2（已完成）：
 *   4. writev 分散/聚集 I/O
 *   5. mmap 零拷贝
 *
 * 阶段 3（当前）：
 *   6. 线程池（生产者-消费者模型）
 *      - 主线程: 事件分发（epoll_wait + 分发到队列）
 *      - 工作线程: 从队列取任务执行
 *
 *   7. Reactor vs Proactor
 *      - Proactor: 主线程做 I/O → 线程池做业务
 *      - Reactor:  主线程分发 → 线程池做 I/O + 业务
 *
 *   8. EPOLLONESHOT + 线程池
 *      - 确保一个 socket 同一时刻只被一个线程处理
 *      - 避免多线程对同一个 fd 的竞争
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
    delete pool_; // 阶段3：释放线程池
}

// ============================================================
// 初始化参数
// ============================================================
void WebServer::init(int port, const char* doc_root, int trig_mode,
                     int thread_num, int actor_model)
{
    port_ = port;
    trig_mode_ = trig_mode;
    thread_num_  = thread_num;
    actor_model_ = actor_model;

    // 安全拷贝文档根目录
    strncpy(doc_root_, doc_root, sizeof(doc_root_) - 1);
    doc_root_[sizeof(doc_root_) - 1] = '\0';

    const char* mode_str = (trig_mode_ == ET_MODE) ? "ET" : "LT";
    const char* actor_str = (actor_model_ == ACTOR_REACTOR) ? "Reactor" : "Proactor";
    printf("[初始化] 端口: %d, 根目录: %s, 触发模式: %s, 线程数: %d, 并发模式: %s\n",
           port_, doc_root_, mode_str, thread_num_, actor_str);
}

// ============================================================
// 创建 socket、绑定、监听、初始化 epoll
// 阶段 3 新增：创建线程池
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

    // ---- 第 7 步: 创建线程池（阶段 3 新增）----
    // 参数：并发模式、线程数、最大请求队列长度
    pool_ = new ThreadPool<HttpConnection>(actor_model_, thread_num_, 10000);

    const char* mode_str = (trig_mode_ == ET_MODE) ? "ET" : "LT"; 
    printf("[就绪] 服务器监听在 http://0.0.0.0:%d  (epoll fd=%d, 连接模式: %s)\n",
           port_, epollfd_, mode_str);
    printf("[提示] 用浏览器访问 http://localhost:%d 测试\n", port_);
    printf("[提示] 或者用 curl http://localhost:%d\n", port_);
}

// ============================================================
// 主事件循环
//
// 阶段 3 的变化：
//   handle_read() 和 handle_write() 内部会根据并发模式，
//   将任务提交到线程池，而不是直接在事件循环中同步处理。
//
//   Proactor 流程：
//     EPOLLIN  → handle_read()    → 主线程 read_once() → pool_->append_p() → 工作线程 process()
//     EPOLLOUT → handle_write()   → 主线程 write()
//
//   Reactor 流程：
//     EPOLLIN  → handle_read()    → pool_->append(conn, 0) → 工作线程 read_once() + process()
//     EPOLLOUT → handle_write()   → pool_->append(conn, 1) → 工作线程 write()
//
//   无论哪种模式，只有主线程调用 epoll_wait，工作线程不碰 epoll。
//   配合 EPOLLONESHOT，每个 socket 同一时间只被一个线程处理。
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
// （阶段 3 与阶段 2 相同，accept 操作轻量且必须在主线程执行）
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
// 处理读事件（阶段 3：根据并发模式分发到线程池）
//
// Proactor 模式 (actor_model_ == 0):
//   主线程亲自执行 read_once() I/O 操作
//   读成功后把任务交给线程池做 process() 处理
//   读失败则关闭连接
//
// Reactor 模式 (actor_model_ == 1):
//   主线程只做事件分发，把读任务交给线程池
//   工作线程负责 read_once() + process()
//   主线程 busy-wait 等待工作线程完成（通过 improv 标志）
//   完成后检查 timer_flag，决定是否关闭连接
// ============================================================
void WebServer::handle_read(int sockfd)
{
    // ==========================================================
    // Proactor 模式: 主线程 I/O → 线程池业务
    // ==========================================================
    if (actor_model_ == ACTOR_PROACTOR)
    {
        // 主线程执行io读取
        if (users_[sockfd].read_once())
        {
            printf("[Proactor] fd=%d 读完成，提交到线程池处理\n", sockfd);
            
            // 将任务加入线程池，工作线程执行 process()
            // 注意：如果队列满了，append_p 返回 false
            // 此时我们简单关闭连接（生产环境可以重试或限流）
            if (!pool_->append_p(&users_[sockfd]))
            {
                printf("[警告] fd=%d 线程池队列已满，关闭连接\n", sockfd);
                handle_close(sockfd);
            }
        }
        else
        {
            // 读取失败（对端断开或出错）
            handle_close(sockfd);
        }
    }
    // ==========================================================
    // Reactor 模式: 主线程分发 → 线程池 I/O + 业务
    // ==========================================================
    else
    {
        // 将读任务提交到线程池（state=0 表示读事件）
        if (!pool_->append(&users_[sockfd], 0))
        {
            printf("[警告] fd=%d 线程池队列已满，关闭连接\n", sockfd);
            handle_close(sockfd);
            return;
        }
        
        // 主线程 busy-wait，等待工作线程完成 I/O + 处理
        //
        // 为什么用 busy-wait 而不是条件变量？
        //   这是 TinyWebServer 原项目的设计选择：
        //   - 简单直接，不需要每个连接维护一个条件变量
        //   - 实际场景中 I/O + HTTP 处理很快（ms 级），busy-wait 短暂
        //   - 缺点：浪费 CPU。生产环境可用条件变量或 std::future 优化
        while (true)
        {
            if (users_[sockfd].improv == 1)
            {
                // 工作线程处理完毕

                // 检查是否需要关闭连接（读或处理过程中出错）
                if (users_[sockfd].timer_flag == 1)
                {
                    handle_close(sockfd);
                    users_[sockfd].timer_flag = 0; // 重置标志
                }

                users_[sockfd].improv = 0; // 重置同步标志
                break;
            }
        }
    }
}

// ============================================================
// 处理写事件（阶段 3：根据并发模式分发到线程池）
//
// Proactor 模式 (actor_model_ == 0):
//   主线程亲自执行 write() I/O 操作
//   write() 返回 false → 关闭连接
//   write() 返回 true  → 已完成发送（keep-alive 已重置，close 等待关闭）
//
// Reactor 模式 (actor_model_ == 1):
//   主线程把写任务交给线程池
//   工作线程负责 write()
//   主线程 busy-wait 等待完成
// ============================================================
void WebServer::handle_write(int sockfd)
{
    // ==========================================================
    // Proactor 模式: 主线程 I/O
    // ==========================================================
    if (actor_model_ == ACTOR_PROACTOR) {
        // 主线程执行 I/O 写入
        if (!users_[sockfd].write()) {
            // write() 返回 false：
            //   1. 发送出错 → 关闭连接
            //   2. 发送完毕且 Connection: close → 关闭连接
            handle_close(sockfd);
        }
        // write() 返回 true 的情况：
        //   1. EAGAIN（缓冲区满）→ write() 内部已重新注册 EPOLLOUT
        //   2. keep-alive 发送完毕 → write() 内部已重置状态 + 注册 EPOLLIN
    }

    // ==========================================================
    // Reactor 模式: 主线程分发 → 线程池 I/O
    // ==========================================================
    else {
        // 将写任务提交到线程池（state=1 表示写事件）
        if (!pool_->append(&users_[sockfd], 1)) {
            printf("[警告] fd=%d 线程池队列已满，关闭连接\n", sockfd);
            handle_close(sockfd);
            return;
        }

        // 主线程 busy-wait，等待工作线程完成写 I/O
        while (true) {
            if (users_[sockfd].improv == 1) {
                // 工作线程处理完毕

                // 检查是否需要关闭连接（写过程中出错）
                if (users_[sockfd].timer_flag == 1) {
                    handle_close(sockfd);
                    users_[sockfd].timer_flag = 0;  // 重置标志
                }

                users_[sockfd].improv = 0;  // 重置同步标志
                break;
            }
        }
    }
}

// ============================================================
// 处理连接关闭
// ============================================================
void WebServer::handle_close(int sockfd)
{
    users_[sockfd].close_conn();
}
