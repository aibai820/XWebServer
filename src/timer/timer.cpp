/**
 * @file    timer.cpp
 * @brief   定时器模块实现
 *
 * 实现有序定时器链表的所有操作、Utils 工具类方法、
 * 以及定时器到期回调函数。
 *
 * 参考：TinyWebServer 原项目的 lst_timer.cpp
 */

#include "timer.h"
#include "../http/http_connection.h"   // 需要访问 HttpConnection::user_count_
#include "../log/log.h"

// ============================================================
// sort_timer_lst 析构函数
// 遍历链表，释放所有定时器节点的内存
// ============================================================
sort_timer_lst::~sort_timer_lst() {
    util_timer* tmp = head;
    while (tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
    head = nullptr;
    tail = nullptr;
}

// ============================================================
// add_timer(): 按过期时间升序插入定时器节点
//
// 示例：链表当前为 [t=10] → [t=20] → [t=30]
//       插入 t=25 → 遍历后插入到 [t=20] 和 [t=30] 之间
//       插入 t=5  → 直接插到 head 之前（成为新 head）
//       插入 t=50 → 遍历到末尾，成为新 tail
// ============================================================
void sort_timer_lst::add_timer(util_timer* timer)
{
    if (!timer) return;

    // 情况 1：链表为空
    if (!head)
    {
        head = tail = timer;
        return;
    }

    // 情况 2：新节点的过期时间比当前head还早 → 插到最前面
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev  = timer;
        head        = timer;
        return;
    }
    
    // 情况 3：从head开始，找到正确的排序位置
    add_timer(timer, head);
}

// ============================================================
// add_timer() 内部重载：从 lst_head 开始搜索插入位置
//
// 遍历策略：
//   从 lst_head 开始，不断向后移动，直到找到第一个
//   expire > timer->expire 的节点，插在其前面；
//   如果遍历到末尾都找不到，插到链表最后（成为新 tail）。
// ============================================================
void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) 
{
    util_timer* prev = lst_head;
    util_timer* tmp  = prev->next;

    // 遍历链表，找到 timer 应该插入的位置
    while (tmp) {
        if (timer->expire < tmp->expire) {
            // 找到了：插入在 prev 和 tmp 之间
            prev->next    = timer;
            timer->next   = tmp;
            tmp->prev     = timer;
            timer->prev   = prev;
            return;
        }
        prev = tmp;
        tmp  = tmp->next;
    }

    // 遍历到链表末尾：timer 的 expire 比所有节点都大
    // 插到末尾成为新的 tail
    prev->next   = timer;
    timer->prev  = prev;
    timer->next  = nullptr;
    tail         = timer;
}

// ============================================================
// adjust_timer(): 更新定时器位置
//
// 典型场景：客户端有新的 I/O 活动 → 延长该连接的超时时间
// 旧 expire = T_old, 新 expire = now + 3*TIMESLOT > T_old
//
// 优化判断：
//   如果 timer->next 为空（是 tail）或者新 expire 仍小于 next->expire，
//   说明节点不需移动（仍在正确位置），直接返回。
//   否则：从链表摘除 → 重新插入。
// ============================================================
void sort_timer_lst::adjust_timer(util_timer* timer)
{
    if (!timer) return;

    util_timer* tmp = timer->next;

    // 优化：检查是否需要移动
    // 条件：有后继节点，且当前 expire >= 后继的 expire → 需要移动
    if (!tmp || (timer->expire < tmp->expire)) {
        return;  // 位置正确，无需调整
    }

    // 从链表中摘除该节点
    if (timer == head) {
        // 是头节点
        head = head->next;
        if (head) head->prev = nullptr;
        timer->next = nullptr;
    } else {
        // 是中间或尾节点
        timer->prev->next = timer->next;
        if (timer->next) {
            timer->next->prev = timer->prev;
        } else {
            // 是尾节点，需要更新 tail
            tail = timer->prev;
        }
        timer->prev = nullptr;
        timer->next = nullptr;
    }

    // 从新的头部开始重新插入
    // 注意：这里从头开始，因为 timer->expire 已经更新为新值
    add_timer(timer, head);
}

// ============================================================
// del_timer(): 从链表中删除并释放一个定时器节点
//
// 四种情况（按出现频率排序）：
//   1. 中间节点：最常见，连接正常关闭时
//   2. 头节点且链表只有一个节点
//   3. 头节点（但链表还有其他节点）
//   4. 尾节点（但链表还有其他节点）
// ============================================================
void sort_timer_lst::del_timer(util_timer* timer)
{
    if (!timer) return;

    // 情况 1: timer 是唯一节点
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = nullptr;
        tail = nullptr;
        return;
    }

    // 情况 2: timer 是头节点
    if (timer == head) {
        head = head->next;
        head->prev = nullptr;
        delete timer;
        return;
    }

    // 情况 3: timer 是尾节点
    if (timer == tail) {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    // 情况 4: timer 是中间节点（最常见）
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// ============================================================
// tick(): 心跳——触发所有已过期的定时器
//
// 因为链表按 expire 升序排列，从 head 开始遍历：
//   - 如果节点已过期 (cur >= timer->expire)：触发回调，删除节点，继续
//   - 如果节点未过期：停止遍历（后面的都未过期）
//
// 时间复杂度：O(k)，其中 k = 已过期的节点数
// ============================================================
void sort_timer_lst::tick()
{
    if (!head) return;

    time_t cur = time(nullptr);
    util_timer* tmp = head;

    while (tmp) {
        // 链表有序：遇到第一个未过期节点就可以停了
        if (cur < tmp->expire) {
            break;
        }

        // 触发定时器回调：关闭连接
        tmp->cb_func(tmp->user_data);

        // 从链表移除
        head = tmp->next;
        if (head) {
            head->prev = nullptr;
        }

        delete tmp;
        tmp = head;
    }

    // 如果所有节点都过期了，tail 也需要更新
    if (!head) {
        tail = nullptr;
    }
}

// ============================================================
// Utils 方法实现
// ============================================================

// ---- 静态成员定义 ----
int* Utils::u_pipefd  = nullptr;
int  Utils::u_epollfd = 0;

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

// setnonblocking(): 设置文件描述符为非阻塞模式
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// addfd(): 向 epoll 内核事件表注册文件描述符
// 与 http_connection.h 中的 add_fd_to_epoll 功能等价，
// 提供 Utils 类的内部版本以便统一管理
void Utils::addfd(int epollfd, int fd, bool one_shot, int trig_mode) {
    epoll_event event;
    event.data.fd = fd;

    // 基础事件：可读 + 对端关闭检测
    event.events = EPOLLIN | EPOLLRDHUP;

    // 边缘触发模式（默认使用 LT，即水平触发）
    if (trig_mode == 1) {
        event.events |= EPOLLET;
    }

    // EPOLLONESHOT: 确保一个 socket 同一时刻只被一个线程处理
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// sig_handler(): 统一事件源的"桥梁"
// 在信号上下文中只调用 async-signal-safe 的 send()
void Utils::sig_handler(int sig) {
    // 保存并恢复 errno：保证信号处理函数的可重入性
    // 如果信号处理函数修改了 errno，回到主程序后值就错了
    int save_errno = errno;
    int msg = sig;
    // 向 pipefd[1]（写端）发送信号编号
    // send() 是 POSIX 保证的异步信号安全函数
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// addsig(): 使用 sigaction 注册信号处理器
void Utils::addsig(int sig, void (*handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    sa.sa_handler = handler;           // 信号处理函数

    // SA_RESTART: 让被此信号中断的慢系统调用（如 read/write/accept）
    // 自动重启而不是返回 EINTR。对于 SIGALRM，不设置此标志，
    // 因为我们需要 epoll_wait 被中断以处理定时器。
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }

    // sigfillset: 在处理此信号时阻塞所有其他信号
    // （在信号处理函数执行期间，不会被其他信号打断）
    sigfillset(&sa.sa_mask);

    // sigaction: 注册信号处理函数
    assert(sigaction(sig, &sa, nullptr) != -1);
}

// timer_handler(): 定时器心跳 + 重新设置闹钟
void Utils::timer_handler() {
    m_timer_lst.tick();         // 清理所有过期连接
    alarm(m_TIMESLOT);           // 重新设置闹钟，保证周期性触发
}

// show_error(): 向客户端发送错误响应并立即关闭连接
void Utils::show_error(int connfd, const char* info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// ============================================================
// cb_func(): 定时器过期回调
//
// 当连接在 15 秒内没有任何活动时被调用。
// 清理步骤：
//   1. epoll_ctl DEL → 从 epoll 监听中移除
//   2. close()      → 关闭 TCP 连接
//   3. user_count-- → 更新全局计数
//
// 注意：这个函数不释放 util_timer 节点本身——
// 节点由 tick() 或 del_timer() 负责释放。
// ============================================================
void cb_func(client_data* user_data) {
    // 从 epoll 内核事件表移除该 socket
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, nullptr);

    assert(user_data);  // 防御：user_data 不应为空

    // 关闭 TCP 连接
    close(user_data->sockfd);

    // 递减全局活跃连接计数
    HttpConnection::user_count_--;

    LOG_INFO("[定时器] fd=%d 超时关闭 (当前在线: %d)",
             user_data->sockfd, HttpConnection::user_count_);
}

