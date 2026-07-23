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
 *
 *   9. 统一事件源（阶段 4 新增）
 *      - socketpair 将信号转化为 I/O 事件
 *      - 信号处理函数只做 send()（异步信号安全）
 *      - 主循环通过 epoll 安全地处理信号
 *
 *   10. 定时器管理（阶段 4 新增）
 *       - 有序双向链表，按过期时间升序
 *       - SIGALRM 周期性触发 tick()
 *       - 15 秒无活动自动断开
 */


#include "server.h"

// ============================================================
// 构造与析构
// ============================================================

WebServer::WebServer() {
    // 预分配连接对象数组
    // 以 fd 为索引直接 O(1) 查找，避免 map 的额外开销
    users_       = new HttpConnection[MAX_FD];

    // 阶段 4: 预分配定时器 client_data 数组
    // 每个活跃连接有一个 client_data，存储 socket 地址 + 定时器节点指针
    users_timer_ = new client_data[MAX_FD];

    // 初始化 pipefd_（socketpair 管道）
    pipefd_[0] = -1;
    pipefd_[1] = -1;
}

WebServer::~WebServer() {
    // 关闭监听 socket
    if (listenfd_ != -1) close(listenfd_);

    // 关闭 epoll 实例
    if (epollfd_  != -1) close(epollfd_);

    // 阶段 4: 关闭 socketpair 管道
    if (pipefd_[0] != -1) close(pipefd_[0]);
    if (pipefd_[1] != -1) close(pipefd_[1]);

    // 释放连接数组
    delete[] users_;

    // 阶段 4: 释放定时器 client_data 数组
    delete[] users_timer_;

    // 释放线程池（析构函数会优雅关闭所有工作线程）
    delete pool_;

    printf("[退出] 服务器已关闭\n");
}

// ============================================================
// init(): 保存所有配置参数
// ============================================================
void WebServer::init(int port, const char* doc_root, int trig_mode,
                     int thread_num, int actor_model,
                     int close_log, int log_write,
                     int sql_num, int opt_linger,
                     const char* sql_user,
                     const char* sql_passwd,
                     const char* sql_dbname)
{
    port_        = port;
    trig_mode_   = trig_mode;
    thread_num_  = thread_num;
    actor_model_ = actor_model;
    close_log_   = close_log;
    log_write_   = log_write;
    sql_num_     = sql_num;
    opt_linger_  = opt_linger;

    // 存储 MySQL 凭据
    sql_user_   = (sql_user   && sql_user[0])   ? sql_user   : "";
    sql_passwd_ = (sql_passwd && sql_passwd[0]) ? sql_passwd : "";
    sql_dbname_ = (sql_dbname && sql_dbname[0]) ? sql_dbname : "";

    // 安全拷贝文档根目录
    strncpy(doc_root_, doc_root, sizeof(doc_root_) - 1);
    doc_root_[sizeof(doc_root_) - 1] = '\0';

    printf("========================================\n");
    printf("  My WebServer — Phase 4\n");
    printf("  生产级高并发 HTTP 服务器\n");
    printf("========================================\n\n");
    printf("[配置] 端口=%d, 根目录=%s, 触发模式=%d, 线程数=%d, 并发=%s\n",
           port_, doc_root_, trig_mode_, thread_num_,
           (actor_model_ == ACTOR_REACTOR) ? "Reactor" : "Proactor");
    printf("[配置] 日志=%s, 日志模式=%s, DB连接数=%d, SO_LINGER=%d\n",
           close_log_ ? "关闭" : "启用",
           log_write_ ? "异步" : "同步",
           sql_num_, opt_linger_);
}

// ============================================================
// log_write(): 初始化日志系统
//
// 同步模式 (log_write_ == 0)：每条日志直接 fputs + fflush
// 异步模式 (log_write_ == 1)：日志推入阻塞队列，后台线程写盘
// ============================================================
void WebServer::log_write() {
    if (close_log_ == 0) {
        // 日志启用
        if (log_write_ == 1) {
            // 异步模式：阻塞队列容量 800
            Log::get_instance()->init("./ServerLog", close_log_,
                                       2000, 800000, 800);
        } else {
            // 同步模式：max_queue_size = 0
            Log::get_instance()->init("./ServerLog", close_log_,
                                       2000, 800000, 0);
        }
        LOG_INFO("========== 服务器启动 ==========");
        LOG_INFO("端口: %d, 触发模式: %d, 线程数: %d, 并发模式: %s",
                 port_, trig_mode_, thread_num_,
                 (actor_model_ == ACTOR_REACTOR) ? "Reactor" : "Proactor");
    }
}

// ============================================================
// sql_pool(): 初始化数据库连接池 + 加载用户凭据
//
// 步骤：
//   1. 获取连接池单例
//   2. 初始化：创建 sql_num_ 个 MySQL 连接
//   3. 从 MySQL 加载全部用户凭据到 HttpConnection::users_ map
// ============================================================
void WebServer::sql_pool() {
    // 获取连接池单例
    m_connPool = connection_pool::GetInstance();

    // 初始化连接池：创建 sql_num_ 个连接
    m_connPool->init("localhost", sql_user_, sql_passwd_,
                     sql_dbname_, 3306, sql_num_, close_log_);

    // 从数据库加载用户凭据到内存缓存
    HttpConnection::initmysql_result(m_connPool);
}

// ============================================================
// thread_pool(): 创建线程池
//
// 传入 connection_pool 指针，工作线程在处理请求时
// 会通过 connectionRAII 自动获取/归还数据库连接。
// ============================================================
void WebServer::thread_pool() {
    pool_ = new ThreadPool<HttpConnection>(actor_model_, m_connPool,
                                            thread_num_, 10000);
}

// ============================================================
// event_listen(): 创建 socket、绑定、监听、初始化 epoll、
//                 统一事件源、信号处理器、定时器
//
// 这是整个服务器的"布线"阶段，把所有事件源（网络 + 信号 + 定时器）
// 接到 epoll 这个"总开关"上。
//
// 步骤总览：
//   1. socket()                  — 创建监听 socket
//   2. SO_LINGER                 — 优雅关闭选项
//   3. SO_REUSEADDR + bind       — 绑定端口
//   4. listen()                  — 开始监听
//   5. utils_.init()             — 初始化定时器时间槽
//   6. epoll_create()            — 创建 epoll 实例
//   7. addfd listenfd            — 注册监听 socket 到 epoll
//   8. socketpair()              — 创建管道（统一事件源）
//   9. addfd pipefd[0]           — 注册管道读端到 epoll
//   10. addsig()                 — 注册信号处理器
//   11. 设置静态变量             — pipefd/epollfd 供回调使用
//   12. alarm()                  — 启动定时器闹钟
// ============================================================
void WebServer::event_listen() {
    // ---- 第 1 步: socket() ----
    listenfd_ = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd_ >= 0);

    // ---- 第 2 步: SO_LINGER（优雅关闭 vs 强制关闭） ----
    // SO_LINGER 控制 close() 的行为：
    //   opt_linger_ == 0: l_onoff=0, l_linger=1 → 优雅关闭，发送剩余数据后 FIN
    //   opt_linger_ == 1: l_onoff=1, l_linger=1 → 强制关闭，立即 RST
    if (opt_linger_ == 0) {
        struct linger tmp = {0, 1};
        setsockopt(listenfd_, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else if (opt_linger_ == 1) {
        struct linger tmp = {1, 1};
        setsockopt(listenfd_, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // ---- 第 3 步: SO_REUSEADDR ----
    int reuse = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // ---- 第 4 步: bind() ----
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;              // IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY);    // 监听所有网卡（0.0.0.0）
    address.sin_port        = htons(port_);          // 主机字节序 → 网络字节序

    int ret = bind(listenfd_, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    // ---- 第 5 步: listen() ----
    ret = listen(listenfd_, 5);
    assert(ret >= 0);

    // ---- 第 6 步: 初始化定时器工具类 ----
    utils_.init(TIMESLOT);

    // ---- 第 7 步: epoll_create() ----
    epollfd_ = epoll_create(5);
    assert(epollfd_ != -1);

    // ---- 第 8 步: 注册 listenfd 到 epoll ----
    // 监听 socket 不需要 EPOLLONESHOT（它只被主线程处理）
    utils_.addfd(epollfd_, listenfd_, false, listen_trig_mode_);

    // 设置全局 epollfd，HttpConnection::init 和 modify_fd_in_epoll 会用到
    HttpConnection::epoll_fd_ = epollfd_;

    // ---- 第 9 步: socketpair（统一事件源的关键）----
    // PF_UNIX + SOCK_STREAM 创建一对全双工的本地 socket：
    //   pipefd_[0] = 读端（注册到 epoll）
    //   pipefd_[1] = 写端（信号处理函数向此端写入信号编号）
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd_);
    assert(ret != -1);

    // 将写端设为非阻塞（信号处理函数中调用 send，不能阻塞）
    utils_.setnonblocking(pipefd_[1]);

    // 将读端注册到 epoll（当信号到达时，epoll_wait 会返回此 fd 的可读事件）
    utils_.addfd(epollfd_, pipefd_[0], false, 0);

    // ---- 第 10 步: 注册信号处理函数 ----
    // SIGPIPE: 向已关闭的 socket 写数据时触发 → 忽略（通过返回值+errno 处理）
    utils_.addsig(SIGPIPE, SIG_IGN);
    // SIGALRM: 定时器信号 → 通过管道传递到主循环
    utils_.addsig(SIGALRM, Utils::sig_handler, false);
    // SIGTERM: 终止信号 → 通过管道传递到主循环，触发优雅退出
    utils_.addsig(SIGTERM, Utils::sig_handler, false);

    // ---- 第 11 步: 设置静态引用（供信号处理函数和回调函数使用） ----
    // 因为它们没有 this 指针，必须通过静态变量访问
    Utils::u_pipefd  = pipefd_;
    Utils::u_epollfd = epollfd_;

    // ---- 第 12 步: 启动定时器闹钟 ----
    // alarm(TIMESLOT) 设置一个 TIMESLOT 秒后触发的 SIGALRM
    // 触发后需要重新 alarm（在 utils_.timer_handler() 中完成）
    alarm(TIMESLOT);

    // ---- 打印就绪信息 ----
    printf("[就绪] 服务器监听在 http://0.0.0.0:%d  (epoll fd=%d)\n",
           port_, epollfd_);
    printf("[就绪] 定时器: %d 秒周期, 超时: %d 秒\n",
           TIMESLOT, 3 * TIMESLOT);
    printf("[就绪] 日志: %s模式, 数据库: %d 连接\n",
           log_write_ ? "异步" : "同步", sql_num_);
    printf("[就绪] 统一事件源: pipefd[0]=%d (epoll监听), pipefd[1]=%d (信号写入)\n",
           pipefd_[0], pipefd_[1]);
    printf("[提示] 用浏览器访问 http://localhost:%d 测试\n", port_);
    printf("[提示] 或者用 curl http://localhost:%d\n", port_);
}

// ============================================================
// event_loop(): 主事件循环
//
// 这是服务器的"心脏"。无限循环，每次迭代：
//   1. epoll_wait() 阻塞等待事件
//   2. 遍历就绪事件列表，按类型分派：
//      - listenfd         → dealclientdata()  新连接
//      - pipefd[0]        → dealwithsignal()   信号
//      - EPOLLIN          → dealwithread()     客户端数据
//      - EPOLLOUT         → dealwithwrite()    可写通知
//      - EPOLLRDHUP/ERR   → deal_timer()       连接出错
//   3. 检查 timeout 标志 → utils_.timer_handler()  清理过期连接
//
// 阶段 4 改进：
//   - 通过 socketpair 管道处理信号（而非旧版本的信号标志）
//   - 定时器 tick 在每次 SIGALRM 到达后执行
//   - stop_server 标志支持优雅退出
// ============================================================
void WebServer::event_loop() {
    bool timeout     = false;   // 是否收到了 SIGALRM（需要 tick 定时器）
    bool stop_server = false;   // 是否收到了 SIGTERM（需要退出）

    printf("\n========== 进入事件循环 ==========\n\n");

    while (!stop_server) {
        // 阻塞等待事件（-1 = 没有事件就永久等）
        int ready_count = epoll_wait(epollfd_, events_, MAX_EVENT_NUMBER, -1);

        if (ready_count < 0) {
            if (errno == EINTR) {
                // 被信号中断（正常流程——SIGALRM 会中断 epoll_wait）
                // 信号已经被 sig_handler 写入 pipefd，下一轮循环会处理
                continue;
            }
            LOG_ERROR("epoll_wait 失败: %s", strerror(errno));
            break;
        }

        // 遍历所有就绪事件
        for (int i = 0; i < ready_count; ++i) {
            int sockfd = events_[i].data.fd;
            uint32_t revents = events_[i].events;

            // ---- 情况 1: 新连接到达 ----
            if (sockfd == listenfd_) {
                bool flag = dealclientdata();
                if (!flag)
                    continue;
            }
            // ---- 情况 2: 对端关闭连接或出错 ----
            else if (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 获取关联的定时器节点，清理连接
                util_timer* timer = users_timer_[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // ---- 情况 3: 信号到达（通过 socketpair 管道） ----
            else if ((sockfd == pipefd_[0]) && (revents & EPOLLIN)) {
                bool flag = dealwithsignal(timeout, stop_server);
                if (!flag)
                    LOG_ERROR("dealwithsignal 失败");
            }
            // ---- 情况 4: 客户端数据到达（EPOLLIN） ----
            else if (revents & EPOLLIN) {
                dealwithread(sockfd);
            }
            // ---- 情况 5: socket 可写（EPOLLOUT） ----
            else if (revents & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
            // ---- 情况 6: 未知事件 ----
            else {
                LOG_WARN("fd=%d 未知事件: 0x%x", sockfd, revents);
            }
        }

        // ---- 定时器过期处理 ----
        // timeout 为 true 表示收到了 SIGALRM 信号
        if (timeout) {
            utils_.timer_handler();   // tick() 清理过期连接 → alarm() 重新定时
            LOG_INFO("定时器心跳 (tick)");
            timeout = false;
        }
    }

    printf("\n[退出] 事件循环结束\n");
}

// ============================================================
// dealclientdata(): 处理新连接（LT/ET 自适应）
//
// LT 模式（水平触发）：
//   只 accept 一次。如果还有未处理的连接，epoll 会再次通知。
//
// ET 模式（边缘触发）：
//   必须循环 accept 直到返回 EAGAIN/EWOULDBLOCK。
//   因为在 ET 下，epoll 只通知一次"有新连接"，
//   如果不全部取走，剩余连接可能一直不被处理。
// ============================================================
bool WebServer::dealclientdata() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // ---- LT 模式：一次 accept ----
    if (listen_trig_mode_ == LT_MODE) {
        int connfd = accept(listenfd_, (struct sockaddr*)&client_addr,
                            &client_len);
        if (connfd < 0) {
            LOG_ERROR("accept 错误: %s", strerror(errno));
            return false;
        }

        // 连接数上限检查
        if (HttpConnection::user_count_ >= MAX_FD) {
            utils_.show_error(connfd,
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
            LOG_ERROR("服务器繁忙：连接数已达上限 %d", MAX_FD);
            return false;
        }

        // 创建定时器 + 初始化连接
        timer(connfd, client_addr);
    }
    // ---- ET 模式：循环 accept ----
    else {
        while (true) {
            int connfd = accept(listenfd_, (struct sockaddr*)&client_addr,
                                &client_len);
            if (connfd < 0) {
                // EAGAIN/EWOULDBLOCK 表示已 accept 完所有等待的连接
                // 其他错误才是真正的错误
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("accept 错误: %s", strerror(errno));
                }
                break;
            }

            // 连接数上限检查
            if (HttpConnection::user_count_ >= MAX_FD) {
                utils_.show_error(connfd,
                    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
                LOG_ERROR("服务器繁忙：连接数已达上限 %d", MAX_FD);
                break;
            }

            // 创建定时器 + 初始化连接
            timer(connfd, client_addr);
        }
        return false;
    }

    return true;
}

// ============================================================
// timer(): 为新连接创建定时器节点
//
// 操作步骤：
//   1. 初始化 HttpConnection 对象（注册到 epoll）
//   2. 填充 client_data 结构体（地址 + fd）
//   3. 创建 util_timer 节点（设置过期时间 + 回调函数 + 用户数据）
//   4. 将节点插入有序定时器链表
//
// 超时时间：当前时间 + 3 * TIMESLOT（默认 15 秒）
// ============================================================
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    // 初始化 HTTP 连接对象
    users_[connfd].init(connfd, client_address, doc_root_, conn_trig_mode_,
                        close_log_, sql_user_.c_str(), sql_passwd_.c_str(),
                        sql_dbname_.c_str());

    // 填充 client_data
    users_timer_[connfd].address = client_address;
    users_timer_[connfd].sockfd  = connfd;

    // 创建定时器节点
    util_timer* timer = new util_timer;
    timer->user_data = &users_timer_[connfd];
    timer->cb_func   = cb_func;         // 到期回调（关闭连接 + epoll 移除）

    // 设置过期时间：当前时间 + 3 个时间槽
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;

    // 保存定时器指针到 client_data（以便后续 adjust/deal）
    users_timer_[connfd].timer = timer;

    // 插入有序定时器链表
    utils_.m_timer_lst.add_timer(timer);

    LOG_INFO("[连接] 新客户端 fd=%d 来自 %s:%d  (当前在线: %d, 过期: %ld)",
             connfd,
             inet_ntoa(client_address.sin_addr),
             ntohs(client_address.sin_port),
             HttpConnection::user_count_,
             (long)timer->expire - cur);
}

// ============================================================
// adjust_timer(): 延长定时器（"续期"）
//
// 当客户端有新的 I/O 活动时调用。
// 将过期时间重置为 当前时间 + 15 秒，
// 然后重新定位节点在有序链表中的位置。
//
// @param timer  要续期的定时器节点
// ============================================================
void WebServer::adjust_timer(util_timer* timer) {
    if (!timer) return;

    // 更新过期时间
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;

    // 在有序链表中重新定位该节点
    utils_.m_timer_lst.adjust_timer(timer);

    LOG_INFO("定时器续期: fd=%d (新过期: %ld)",
             timer->user_data ? timer->user_data->sockfd : -1,
             (long)timer->expire - cur);
}
// ============================================================
// deal_timer(): 处理定时器到期后的连接清理
//
// 当连接超时或主动关闭时调用。
// 触发回调函数（关闭 socket + epoll 移除），
// 从定时器链表中删除节点，防止悬空指针。
//
// @param timer   要处理的定时器节点
// @param sockfd  关联的 socket fd
// ============================================================
void WebServer::deal_timer(util_timer* timer, int sockfd) {
    if (timer) {
        // 触发回调：epoll_ctl DEL + close(sockfd) + user_count--
        timer->cb_func(&users_timer_[sockfd]);

        // 从定时器链表删除节点 + 释放内存
        utils_.m_timer_lst.del_timer(timer);

        // 防止悬空指针
        users_timer_[sockfd].timer = nullptr;
    }

    LOG_INFO("连接关闭: fd=%d", sockfd);
}

// ============================================================
// dealwithsignal(): 从 socketpair 管道读取信号
//
// 统一事件源的核心实现。管道中可能累积了多个信号字节
// （例如在两次 epoll_wait 之间连续收到多个 SIGALRM），
// 所以用循环读取所有字节。
//
// @param timeout     [out] 是否收到 SIGALRM（需要 tick 定时器）
// @param stop_server [out] 是否收到 SIGTERM（需要退出主循环）
// @return true=成功读取, false=管道错误
// ============================================================
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];

    // 从管道读端读取信号编号
    ret = recv(pipefd_[0], signals, sizeof(signals), 0);

    if (ret == -1) {
        // 读错误
        return false;
    } else if (ret == 0) {
        // 管道对端关闭（不应发生）
        return false;
    } else {
        // 处理管道中累积的所有信号
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
            case SIGALRM:
                timeout = true;        // 需要 tick 定时器
                break;
            case SIGTERM:
                stop_server = true;    // 需要退出主循环
                LOG_INFO("收到 SIGTERM 信号，准备优雅退出");
                break;
            default:
                break;
            }
        }
    }

    return true;
}

// ============================================================
// dealwithread(): 处理读事件（Proactor/Reactor 自适应）
//
// Proactor 模式 (actor_model_ == 0):
//   主线程亲自执行 read_once() I/O 操作，
//   读成功后把任务交给线程池做 process() 处理。
//   读失败则关闭连接（通过 deal_timer 清理）。
//
// Reactor 模式 (actor_model_ == 1):
//   主线程只做事件分发，把读任务交给线程池。
//   工作线程负责 read_once() + process()。
//   主线程 busy-wait 等待工作线程完成（通过 improv 标志）。
//
// 两种模式都会在 I/O 活动时延长定时器（adjust_timer）。
// ============================================================
void WebServer::dealwithread(int sockfd) {
    util_timer* timer = users_timer_[sockfd].timer;

    // ==========================================================
    // Reactor 模式: 主线程分发 → 线程池 I/O + 业务
    // ==========================================================
    if (actor_model_ == ACTOR_REACTOR) {
        // 有 I/O 活动 → 延长定时器
        if (timer) {
            adjust_timer(timer);
        }

        // 将读任务提交到线程池（state=0 表示读事件）
        if (!pool_->append(&users_[sockfd], 0)) {
            // 线程池队列已满
            LOG_WARN("fd=%d 线程池队列已满（读事件），关闭连接", sockfd);
            deal_timer(timer, sockfd);
            return;
        }

        // 主线程 busy-wait，等待工作线程完成 I/O + 处理
        // （忙等的替代方案：条件变量或 std::future，留给后续优化）
        while (true) {
            if (users_[sockfd].improv == 1) {
                // 工作线程处理完毕

                // 检查是否需要关闭连接（读或处理过程中出错）
                if (users_[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users_[sockfd].timer_flag = 0;
                }

                users_[sockfd].improv = 0;
                break;
            }
        }
    }
    // ==========================================================
    // Proactor 模式: 主线程 I/O → 线程池业务
    // ==========================================================
        else {
        // 主线程执行 I/O 读取
        if (users_[sockfd].read_once()) {
            LOG_INFO("read from client(%s)",
                     inet_ntoa(users_[sockfd].address().sin_addr));

            // 将任务加入线程池，工作线程执行 process()
            if (!pool_->append_p(&users_[sockfd])) {
                LOG_WARN("fd=%d 线程池队列已满（Proactor），关闭连接", sockfd);
                deal_timer(timer, sockfd);
                return;
            }

            // 有 I/O 活动 → 延长定时器（在成功读取后）
            if (timer) {
                adjust_timer(timer);
            }
        } else {
            // 读取失败（对端断开或出错）→ 清理连接
            deal_timer(timer, sockfd);
        }
    }
}

// ============================================================
// dealwithwrite(): 处理写事件（Proactor/Reactor 自适应）
//
// Proactor 模式 (actor_model_ == 0):
//   主线程亲自执行 write() I/O 操作。
//   write() 返回 false → 关闭连接。
//   write() 返回 true  → 发送完成（keep-alive 则等待下一个请求）。
//
// Reactor 模式 (actor_model_ == 1):
//   主线程把写任务交给线程池。
//   工作线程负责 write()。
//   主线程 busy-wait 等待完成。
//
// 两种模式都会在 I/O 活动时延长定时器（adjust_timer）。
// ============================================================
void WebServer::dealwithwrite(int sockfd) {
    util_timer* timer = users_timer_[sockfd].timer;

    // ==========================================================
    // Reactor 模式: 主线程分发 → 线程池 I/O
    // ==========================================================
    if (actor_model_ == ACTOR_REACTOR) {
        // 有 I/O 活动 → 延长定时器
        if (timer) {
            adjust_timer(timer);
        }

        // 将写任务提交到线程池（state=1 表示写事件）
        if (!pool_->append(&users_[sockfd], 1)) {
            LOG_WARN("fd=%d 线程池队列已满（写事件），关闭连接", sockfd);
            deal_timer(timer, sockfd);
            return;
        }

        // 主线程 busy-wait，等待工作线程完成写 I/O
        while (true) {
            if (users_[sockfd].improv == 1) {
                // 工作线程处理完毕

                // 检查是否需要关闭连接（写过程中出错）
                if (users_[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users_[sockfd].timer_flag = 0;
                }

                users_[sockfd].improv = 0;
                break;
            }
        }
    }
    // ==========================================================
    // Proactor 模式: 主线程 I/O
    // ==========================================================
    else {
        // 主线程执行 I/O 写入
        if (users_[sockfd].write()) {
            LOG_INFO("write to client(%s)",
                     inet_ntoa(users_[sockfd].address().sin_addr));

            // 有 I/O 活动 → 延长定时器
            if (timer) {
                adjust_timer(timer);
            }
        } else {
            // write() 返回 false：
            //   1. 发送出错 → 关闭连接
            //   2. 发送完毕且 Connection: close → 关闭连接
            deal_timer(timer, sockfd);
        }
    }
}
