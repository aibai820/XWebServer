/**
 * @file    timer.h
 * @brief   定时器模块 —— 有序双向链表 + 统一事件源
 *
 * 设计思想：
 *
 *   1. 为什么需要定时器？
 *      服务器不能无限制地保持所有连接。当客户端长时间无活动（不发送任何数据），
 *      服务器应该主动断开连接以回收资源（文件描述符、内存、epoll 槽位）。
 *
 *   2. 定时器数据结构 —— 有序双向链表
 *      所有定时器按过期时间升序排列：
 *        - 插入: O(n) —— 遍历找到插入位置
 *        - 删除: O(1) —— 双向链表直接操作前后指针
 *        - 调整: O(n) —— 更新过期时间后重新定位
 *        - tick: O(k) —— 只遍历头部的 k 个过期节点（后面都是未过期的）
 *
 *      为什么不用时间轮（Time Wheel）？
 *        - 时间轮插入/删除 O(1)，但实现复杂度高
 *        - 对于教学项目，有序链表的代码清晰易懂
 *        - 在几千个连接的场景下，O(n) 插入是可接受的
 *
 *   3. 统一事件源（Unified Event Source）
 *      核心问题：信号处理函数（signal handler）中有严格的限制。
 *      - 信号处理函数中不能调用 printf、malloc 等非异步信号安全的函数
 *      - 但我们需要在收到信号时执行复杂操作（如 tick 定时器）
 *
 *      解决方案：socketpair + epoll
 *        a) 用 socketpair() 创建一对 Unix 域 socket：pipefd[0] 和 pipefd[1]
 *        b) pipefd[0] 注册到 epoll 中（只监听读端）
 *        c) 信号处理函数 sig_handler() 中只做一件事：向 pipefd[1] 写一个字节
 *           （send() 是异步信号安全的！）
 *        d) 这个字节会使 epoll_wait 返回 pipefd[0] 的可读事件
 *        e) 主循环中处理这个"信号事件"——此时可以安全执行任何操作
 *
 *      这样就把"异步的信号"变成了"同步的 I/O 事件"！
 *
 *   4. SIGALRM 定时信号
 *      用 alarm(TIMESLOT) 设置周期性闹钟：
 *        - 每 TIMESLOT 秒触发一次 SIGALRM
 *        - 信号通过 socketpair 传递到主循环
 *        - 主循环调用 timer_handler() → tick() 清理过期连接 → alarm() 重新定时
 *
 * 参考：TinyWebServer 原项目的定时器设计、《Linux 高性能服务器编程》第 11 章
 */
#ifndef XWEBSERVER_TIMER_H
#define XWEBSERVER_TIMER_H

#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>

// 前置声明：避免循环依赖
// 实际头文件在 timer.cpp 中包含
class util_timer;
struct client_data;
class HttpConnection;

/**
 * @brief 客户端数据
 *
 * 每个连接关联一个 client_data 对象（存储在 WebServer 的数组中）。
 * 它持有 socket fd、地址信息，以及指向关联定时器节点的指针。
 * 当定时器过期时，cb_func 会收到这个结构体，从中获取 sockfd 进行清理。
 */
struct client_data
{
    sockaddr_in address;        // 客户端网络地址
    int sockfd;                 // 客户端 socket 文件描述符
    util_timer* timer;          // 指向关联的定时器节点（双向链表节点）
};

/**
 * @brief 定时器节点
 *
 * 每个活跃连接都有一个定时器节点，插入在有序双向链表中。
 * 节点按 expire 值升序排列（越早过期的越靠前）。
 *
 * 过期时间策略：每次客户端有 I/O 活动时，
 * 将该连接的定时器过期时间重置为 当前时间 + 3 * TIMESLOT（默认 15 秒）。
 * 如果在 15 秒内没有任何活动，定时器过期 → 断开连接。
 */
class util_timer
{
public:
    util_timer() : expire(0), cb_func(nullptr), user_data(nullptr),
                   prev(nullptr), next(nullptr) {}
public:
    time_t  expire;                    // 绝对过期时间（Unix 时间戳，秒）
    void  (*cb_func)(client_data*);    // 到期时的回调函数指针
    client_data* user_data;            // 指向关联的客户端数据
    util_timer*  prev;                 // 双向链表前驱节点
    util_timer*  next;                 // 双向链表后继节点
};

/**
 * @brief 有序双向链表定时器容器
 *
 * 维护一个按过期时间升序排列的 util_timer 双向链表。
 * - head: 指向最早过期的节点（最小 expire）
 * - tail: 指向最晚过期的节点（最大 expire）
 *
 * 关键性质：由于链表有序，tick() 从头开始遍历，遇到第一个未过期节点即可停止。
 */
class sort_timer_lst
{
public:
    /**
     * @brief 构造函数：初始化空链表
     */
    sort_timer_lst() : head(nullptr), tail(nullptr) {}

    /**
     * @brief 析构函数：遍历链表删除所有节点，释放内存
     *
     * 安全措施：如果服务器正常退出时还有未过期的定时器，
     * 析构函数保证不会有内存泄漏。
     */
    ~sort_timer_lst();

    /**
     * @brief 添加定时器节点到链表（按 expire 升序插入）
     *
     * @param timer  要添加的定时器节点指针（由调用方 new 分配）
     *
     * 三种情况：
     *   1. 链表为空 → 直接设为 head 和 tail
     *   2. 新节点 expire < head->expire → 插到最前面（成为新 head）
     *   3. 其他 → 遍历链表找到正确的排序位置插入
     */
    void add_timer(util_timer* timer);

    /**
     * @brief 调整定时器位置
     *
     * 当某个连接的定时器过期时间被更新（延长）后调用。
     * 从当前位置摘下节点，重新按新的 expire 值插入。
     *
     * 优化：如果节点只被延长（expire 变大），且仍在当前位置之后，
     * 则不需要移动。先检查 timer->next->expire：
     *   如果 timer->expire < timer->next->expire → 不变，直接返回
     *   否则 → 摘下并重新插入
     *
     * @param timer  要调整的定时器节点
     */
    void adjust_timer(util_timer* timer);

    /**
     * @brief 从链表中删除并释放一个定时器节点
     *
     * @param timer  要删除的定时器节点
     *
     * 四种情况：
     *   1. 唯一节点（head == tail == timer）
     *   2. 头部节点（timer == head）
     *   3. 尾部节点（timer == tail）
     *   4. 中间节点
     *
     * 每种情况都需要正确更新 head/tail 和前后的 prev/next 指针。
     */
    void del_timer(util_timer* timer);

    /**
     * @brief 心跳函数：遍历链表头部，触发所有已过期的定时器回调
     *
     * 从 head 开始，检查每个节点的 expire 是否 <= 当前时间。
     * 如果过期：调用 cb_func，从链表移除，删除节点。
     * 如果未过期：停止遍历（因为链表有序，后面的都未过期）。
     *
     * 调用时机：每次 SIGALRM 信号到达时（默认每 5 秒一次）。
     */
    void tick();

private:
    /**
     * @brief 内部辅助：从指定位置开始，按 expire 升序插入定时器
     *
     * @param timer     要插入的定时器节点
     * @param lst_head  搜索起始位置
     */
    void add_timer(util_timer* timer, util_timer* lst_head);

    util_timer* head;   // 链表头（expire 最小的节点）
    util_timer* tail;   // 链表尾（expire 最大的节点）
};

/**
 * @brief 工具类：信号处理 + epoll 操作 + 定时器管理
 *
 * 设计为可实例化的类（非纯静态），在 WebServer 中持有一个实例。
 *
 * 静态成员 u_pipefd 和 u_epollfd 的用途：
 *   - sig_handler 是静态方法（信号处理函数不能是非静态成员函数）
 *   - cb_func 是全局函数（定时器回调）
 *   - 它们都需要访问 pipefd 和 epollfd，但无法通过 this 访问
 *   - 所以用静态成员变量来共享这些值
 */
class Utils
{
public:
    Utils()  = default;
    ~Utils() = default;

    /**
     * @brief 初始化定时器时间槽
     * @param timeslot  定时器间隔（秒），同时也是 alarm() 的周期
     */
    void init(int timeslot);

    /**
     * @brief 设置文件描述符为非阻塞模式
     *
     * @param fd  文件描述符
     * @return 旧的标志位（可用于恢复）
     *
     * 非阻塞是 epoll ET 模式的前提，也是避免工作线程
     * 在 read/write 上阻塞的关键。
     */
    int setnonblocking(int fd);

    /**
     * @brief 向 epoll 注册文件描述符
     *
     * @param epollfd    epoll 实例
     * @param fd         要注册的文件描述符
     * @param one_shot   是否启用 EPOLLONESHOT
     * @param trig_mode  触发模式：0=LT, 1=ET
     *
     * 与 http_connection.h 中的 add_fd_to_epoll 功能相同，
     * 但由 Utils 类统一管理。两者可以互换使用。
     */
    void addfd(int epollfd, int fd, bool one_shot, int trig_mode);

    /**
     * @brief 信号处理函数（静态）
     *
     * 这是整个"统一事件源"机制的核心：
     *   1. 保存 errno（保证可重入性）
     *   2. 将收到的信号编号写入 pipefd[1]
     *   3. 恢复 errno
     *
     * 为什么只做 send()？
     *   POSIX 规定 send() 是异步信号安全的（async-signal-safe），
     *   而 printf、malloc 等不是。信号处理函数中只能调用
     *   异步信号安全的函数。
     *
     * @param sig  收到的信号编号（SIGALRM 或 SIGTERM）
     */
    static void sig_handler(int sig);

    /**
     * @brief 注册信号处理函数
     *
     * 使用 sigaction 而非 signal：
     *   - sigaction 是 POSIX 标准，行为一致
     *   - signal 在不同 UNIX 变体上行为不统一
     *   - sigaction 提供更多控制选项
     *
     * @param sig       要处理的信号编号
     * @param handler   信号处理函数指针
     * @param restart   是否设置 SA_RESTART（让被信号中断的系统调用自动重启）
     *                  对于 SIGALRM: false（epoll_wait 需要被中断）
     *                  对于其他: true（read/write 应该透明地处理信号）
     */
    void addsig(int sig, void (*handler)(int), bool restart = true);

    /**
     * @brief 定时器处理：tick + 重新设置闹钟
     *
     * 每次 SIGALRM 到达时调用。执行两个操作：
     *   1. m_timer_lst.tick()  —— 清理所有过期连接
     *   2. alarm(m_TIMESLOT)    —— 重新定时，保证周期性触发
     *
     * alarm() 是非重复的——每次触发后自动取消，必须重新设置。
     */
    void timer_handler();

    /**
     * @brief 向客户端发送错误消息并关闭连接
     *
     * 用于处理连接数超限等场景：直接发送错误响应，
     * 不创建完整的 HttpConnection 对象。
     *
     * @param connfd  客户端 socket fd
     * @param info    HTTP 错误响应字符串
     */
    void show_error(int connfd, const char* info);

public:
    // ---- 静态成员：供信号处理函数和回调函数访问 ----
    static int* u_pipefd;       // socketpair 管道文件描述符数组 [0]=读端 [1]=写端
    static int  u_epollfd;      // epoll 实例文件描述符（供 cb_func 使用）

    // ---- 实例成员 ----
    sort_timer_lst m_timer_lst;  // 定时器有序链表
    int m_TIMESLOT = 5;          // 定时器时间槽（秒），默认 5 秒

};

/**
 * @brief 定时器过期回调函数
 *
 * 当某个连接的定时器到期时被调用。
 * 执行三个操作：
 *   1. 从 epoll 中移除该 socket fd
 *   2. 关闭 socket（发送 FIN 给对方）
 *   3. 递减全局活跃连接计数
 *
 * 为什么需要 Utils::u_epollfd 是静态的？
 *   这个函数是 C 风格的回调（void(*)(client_data*)），
 *   没有 this 指针，必须通过全局/静态变量访问 epollfd。
 *
 * @param user_data  指向过期连接的 client_data
 */
void cb_func(client_data* user_data);

#endif // XWEBSERVER_TIMER_H