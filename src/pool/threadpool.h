/**
 * @file    threadpool.h
 * @brief   线程池模板类 —— 生产者-消费者模型
 *
 * 阶段 3（已完成）：基础线程池 + Reactor/Proactor 模式
 * 阶段 4（已完成）：数据库连接池 RAII 集成 + 优雅关闭
 *
 * 设计思想：
 *   线程池预先创建 N 个工作线程，它们阻塞在信号量上等待任务。
 *   主线程（生产者）将任务放入请求队列后 post 信号量，
 *   工作线程（消费者）被唤醒后从队列取任务执行。
 *
 * 两种并发模式：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ Proactor 模式 (m_actor_model == 0，默认)                     │
 *   │   主线程: 负责全部 I/O 操作 (read_once / write)               │
 *   │   工作线程: 只负责业务逻辑 (process: HTTP 解析 + 响应构建)     │
 *   │   优点: I/O 集中在主线程，适合 I/O 操作本身很快的场景         │
 *   │   阶段 4: 工作线程在 process() 前后自动获取/释放 DB 连接       │
 *   └──────────────────────────────────────────────────────────────┘
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ Reactor 模式 (m_actor_model == 1)                            │
 *   │   主线程: 只负责事件分发（往队列里塞任务）                     │
 *   │   工作线程: 负责 I/O + 业务逻辑                               │
 *   │   优点: 主线程更轻量，I/O 操作可以并行执行                    │
 *   │   阶段 4: 工作线程在读完成后自动获取/释放 DB 连接              │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 阶段 4 新增：数据库连接池 RAII 集成
 *   每个处理 HTTP 请求的工作线程在执行 process() 前，
 *   通过 connectionRAII 自动从连接池获取一个 MySQL 连接。
 *   离开作用域时自动归还。这样 CGI 处理可以直接使用该连接。
 *
 * 阶段 4 新增：优雅关闭
 *   m_stop 标志 + stop() 方法 + joinable 线程：
 *     - stop(): 设置 m_stop=true，post 信号量唤醒所有线程
 *     - 线程检测到 m_stop 后退出 while 循环
 *     - 析构函数: stop() → pthread_join 等待所有线程 → 清理资源
 *   相比之前"线程 detach 后不管"的做法，这保证了完整的资源回收。
 *
 * 线程池大小建议：
 *   - CPU 密集型: 线程数 = CPU 核心数
 *   - I/O 密集型: 线程数 = CPU 核心数 × 2
 *   - Jetson Xavier NX (6 核): 建议 6~8 个线程
 */

#ifndef XWEBSERVER_THREADPOOL_H
#define XWEBSERVER_THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <atomic>
#include <pthread.h>
#include "../sync/locker.h"
#include "sql_connection_pool.h"

/**
 * @brief 线程池模板类
 *
 * @tparam T  任务类型（在我们的项目中是 HttpConnection）
 *
 * 使用方式：
 *   ThreadPool<HttpConnection> pool(actor_model, thread_num, max_requests);
 *   pool.append(conn, 0);     // Reactor 模式：提交读任务
 *   pool.append(conn, 1);     // Reactor 模式：提交写任务
 *   pool.append_p(conn);      // Proactor 模式：提交处理任务
 */
template <typename T>
class ThreadPool
{
public:
    /**
     * @brief 构造函数：创建线程池
     *
     * @param actor_model   并发模型：0=Proactor, 1=Reactor
     * @param connPool      数据库连接池指针（阶段 4 新增，可为 nullptr）
     * @param thread_number 工作线程数量（建议设置为 CPU 核心数）
     * @param max_requests  请求队列最大长度（防止任务堆积导致 OOM）
     */
    ThreadPool(int actor_model, connection_pool* connPool,
               int thread_number = 8, int max_requests = 10000);
    
    /**
     * @brief 析构函数：优雅关闭所有线程并释放资源
     *
     * 阶段 4 改进：
     *   1. 调用 stop() 通知所有线程退出
     *   2. pthread_join 等待每个线程结束
     *   3. 释放线程 ID 数组
     * 相比阶段 3 的 detach 方式，这保证了完整的资源回收。
     */
    ~ThreadPool();

    // 禁止拷贝（管理者线程和队列）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator = (const ThreadPool&) = delete;
    
    /**
     * @brief Reactor 模式：向队列添加任务（带状态标记）
     *
     * 主线程只负责事件分发，I/O 由工作线程完成。
     * state == 0：工作线程执行 read_once() + process()
     * state == 1：工作线程执行 write()
     *
     * @param request 任务对象指针
     * @param state   任务类型：0=读事件, 1=写事件
     * @return true=添加成功, false=队列已满
     */
    bool append(T* request, int state);
    /**
     * @brief Proactor 模式：向队列添加任务（无状态）
     *
     * 主线程已完成 I/O (read_once)，工作线程只需做 process()。
     *
     * @param request 任务对象指针
     * @return true=添加成功, false=队列已满
     */
    bool append_p(T* request);

private:
    /**
     * @brief 工作线程的入口函数（静态，符合 pthread_create 签名）
     *
     * 因为 C++ 成员函数指针有隐式的 this 参数，不能直接传给
     * pthread_create。所以用静态函数做跳板，通过 arg 传递 this 指针，
     * 再调用 run() 成员函数。
     */
    static void* worker(void* arg);

    /**
     * @brief 工作线程的主循环
     *
     * 无限循环（直到收到 m_stop 信号）：
     *   1. sem_wait() 阻塞等待任务
     *   2. 从队列取出任务
     *   3. 根据 actor_model 执行不同逻辑
     *      - Proactor: connectionRAII 获取 DB 连接 → process()
     *      - Reactor 读: read_once() → connectionRAII → process()
     *      - Reactor 写: write()
     */
    void run();

private:
    int               m_thread_number;    // 工作线程数量
    int               m_max_requests;     // 请求队列最大长度
    pthread_t*        m_threads;          // 线程 ID 数组（动态分配）
    std::list<T*>     m_workqueue;        // 请求队列（使用 list 方便头删尾插）
    MutexLock         m_queuelocker;       // 保护请求队列的互斥锁
    Semaphore         m_queuestat;         // 信号量：队列中待处理的任务数量
    int               m_actor_model;       // 并发模型：0=Proactor, 1=Reactor
    connection_pool*  m_connPool;          // 数据库连接池指针（阶段 4 新增）
    std::atomic<bool> m_stop;             // 关闭标志（阶段 4 新增，线程安全）

};

// ============================================================
// 构造函数
// ============================================================
template <typename T>
ThreadPool<T>::ThreadPool(int actor_model, connection_pool* connPool,
                          int thread_number, int max_requests)
    : m_actor_model(actor_model)
    , m_thread_number(thread_number)
    , m_max_requests(max_requests)
    , m_threads(nullptr)
    , m_connPool(connPool)
    , m_stop(false)
{
    // 参数合法性检查
    if (thread_number <= 0 || max_requests <= 0)
    {
        throw std::exception();
    }

    // 分配线程ID数组
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) 
    {
        throw std::exception();
    }

    // 创建所有工作线程
    for (int i = 0; i < thread_number; i++)
    {
        // pthread_create: 创建线程
        // 参数: [线程ID输出] [线程属性(NULL=默认)] [入口函数] [入口参数]
        if (pthread_create(m_threads + i, nullptr, worker, this) != 0)
        {
            delete[] m_threads;
            m_threads = nullptr;
            throw std::exception();
        }
        // 阶段 3: pthread_detach（不 join，OS 回收）
        // 阶段 4: 不再 detach——析构时 join，保证优雅关闭
    }
    
    const char* model_str = (m_actor_model == 1) ? "Reactor" : "Proactor";
    printf("[线程池] 创建成功: %d 个线程, 队列上限 %d, 模式: %s, DB池: %s\n",
           m_thread_number, m_max_requests, model_str,
           m_connPool ? "已连接" : "未连接");
}

// ============================================================
// 析构函数（阶段 4 改进：优雅关闭）
//
// 为什么需要 join 而不是 detach？
//   detach 后线程独立运行，无法控制其生命周期。
//   如果主线程退出而 detach 线程还在运行，可能访问已析构的
//   全局对象（如 connection_pool 单例），导致 crash。
//   join 保证所有工作线程完全退出后再释放资源。
// ============================================================
template<typename T>
ThreadPool<T>::~ThreadPool()
{
    // 设置停止标志，通知所有线程退出
    m_stop = true;

    // 向信号量 post（线程数 × 2）确保所有阻塞在 sem_wait 的线程被唤醒
    // 多发一些 post 是安全的——多余的 post 只会让线程在检查 m_stop 后
    // 立即再次循环并再次检查 m_stop
    for (int i = 0; i < m_thread_number; ++i) {
        m_queuestat.post();
    }

    // 等待所有工作线程结束
    for (int i = 0; i < m_thread_number; ++i) {
        pthread_join(m_threads[i], nullptr);
    }

    delete[] m_threads;
    m_threads = nullptr;
    printf("[线程池] 已优雅关闭\n");
}

// ============================================================
// Reactor 模式: append() —— 带状态标记的任务提交
//
// 主线程只做事件分发：
//   - 读事件到来 → append(conn, 0) → 工作线程执行读 I/O + 业务逻辑
//   - 写事件到来 → append(conn, 1) → 工作线程执行写 I/O
//
// m_state 约定:
//   0 = 读 (工作线程调用 read_once() + process())
//   1 = 写 (工作线程调用 write())
// ============================================================
template <typename T>
bool ThreadPool<T>::append(T* request, int state)
{
    // 加锁保护队列
    m_queuelocker.lock();
    
    // 队列满了就不让加（简单拒绝策略）
    if (m_workqueue.size() >= static_cast<size_t>(m_max_requests))
    {
        m_queuelocker.unlock();
        return false;
    }
    
    // 设置任务类型（0=读, 1=写）
    request -> m_state = state;

    // 加入队列尾部
    m_workqueue.push_back(request);

    m_queuelocker.unlock();

    // V 操作：信号量+1，唤醒一个等待的工作线程
    m_queuestat.post();

    return true;
}

// ============================================================
// Proactor 模式: append_p() —— 纯处理任务提交
//
// 主线程已完成 I/O (read_once / write)，
// 工作线程只需要执行业务逻辑 (process)。
//
// 注意：Proactor 模式下主线程已经读完了数据或写完了数据，
// 所以这里不需要 state 标记，工作线程只调用 process()。
// ============================================================
template <typename T>
bool ThreadPool<T>::append_p(T* request) {
    m_queuelocker.lock();

    if (m_workqueue.size() >= static_cast<size_t>(m_max_requests)) 
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);

    m_queuelocker.unlock();

    // V 操作：唤醒一个工作线程
    m_queuestat.post();

    return true;
}

// ============================================================
// 工作线程入口（静态函数）
// ============================================================
template <typename T>
void* ThreadPool<T>::worker(void* arg) {
    // arg 是 ThreadPool 的 this 指针，转回来调用 run()
    ThreadPool* pool = static_cast<ThreadPool*>(arg);
    pool->run();
    return nullptr;
}

// ============================================================
// 工作线程主循环（阶段 4 改进：DB 连接 RAII + 优雅关闭支持）
//
// 这是线程池的核心逻辑。每个工作线程都在这个函数中循环：
//   1. 在信号量上阻塞，等待有任务到来
//   2. 收到 m_stop 信号后退出（阶段 4 新增）
//   3. 被唤醒后，从队列头部取出一个任务
//   4. 根据并发模型执行不同的处理逻辑
//   5. Proactor/Reactor读: 通过 connectionRAII 自动管理 DB 连接
//   6. 回到步骤 1
// ============================================================
template <typename T>
void ThreadPool<T>::run()
{
    while (true) {
        // ---- 第 1 步：P 操作，阻塞等待任务 ----
        m_queuestat.wait();

        // 阶段 4: 收到停止信号则退出循环
        // 注意：要在取任务之前检查，因为 stop() 只是 post 了信号量，
        // 队列中可能没有实际任务
        if (m_stop) {
            break;
        }

        // ---- 第 2 步：从队列取出一个任务 ----
        m_queuelocker.lock();

        // 双重检查：被唤醒后队列可能是空的（虚假唤醒 / 竞争）
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;  // 回到 wait，继续等待
        }

        T* request = m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelocker.unlock();

        // 空指针防御
        if (!request) {
            continue;
        }

        // ---- 第 3 步：根据并发模型执行 ----

        // ==========================================================
        // Reactor 模式 (m_actor_model == 1)
        //
        // 工作线程负责 I/O + 业务逻辑。
        // 主线程已经通过 append(request, state) 告诉我们该做什么：
        //   m_state == 0 → 这是读事件，调用 read_once() + process()
        //   m_state == 1 → 这是写事件，调用 write()
        //
        // 处理完毕后设置 improv = 1 通知主线程。
        // 如果出错（连接断开），设置 timer_flag = 1。
        //
        // 阶段 4 改进：读事件处理时，通过 connectionRAII 自动获取
        // 数据库连接，确保 CGI 处理（登录/注册）可用。
        // ==========================================================
        if (1 == m_actor_model)
        {
            // ---- Reactor 读事件 ----
            if (0 == request->m_state)
            {
                // 工作线程亲自执行io读取
                if (request->read_once())
                {
                    request->improv = 1;  // 标记：io读取完成

                    // 阶段 4: RAII 获取数据库连接
                    // 仅在连接池存在时获取（无 MySQL 部署时 m_connPool 为 nullptr）
                    if (m_connPool) 
                    {
                        connectionRAII mysqlcon(&request->mysql_, m_connPool);
                        // 执行 HTTP 业务处理（解析 + CGI 路由 + 构建响应）
                        request->process();
                    } else {
                        // 无数据库，直接处理（纯静态文件服务）
                        request->process();
                    }
                }
                else
                {
                    request->improv = 1; // 标记：已完成（虽然失败了）
                    request->timer_flag = 1; // 标记：需要关闭连接
                }
                
            }
            // ---- Reactor 写事件 ----
            else
            {
                // 工作线程亲自执行 I/O 写入
                // 写操作不需要数据库连接
                if (request->write())
                {
                    request->improv = 1; // 标记：io写入完成
                }
                else
                {
                    request->improv = 1; // 标记：已完成（虽然失败了）
                    request->timer_flag = 1; // 标记：需要关闭连接
                }
                
            }
            
        }
        // ==========================================================
        // Proactor 模式 (m_actor_model == 0，默认)
        //
        // 主线程已经完成了 I/O（read_once / write），
        // 工作线程只需要做纯业务逻辑处理（HTTP 解析 + 响应构建）。
        //
        // 阶段 4 改进：通过 connectionRAII 自动获取数据库连接。
        // ==========================================================
        else
        {
            // 阶段 4: RAII 获取数据库连接
            // CGI 处理（登录/注册）需要 MySQL 连接，
            // 静态文件服务不需要。connectionRAII 统一处理。
            if (m_connPool) {
                connectionRAII mysqlcon(&request->mysql_, m_connPool);
                // 执行 HTTP 业务处理（解析 + CGI 路由 + 构建响应）
                request->process();
            } else {
                // 无数据库，直接处理
                request->process();
            }
        }
    }

}

#endif // XWEBSERVER_THREADPOOL_H