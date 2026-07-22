/**
 * @file    threadpool.h
 * @brief   线程池模板类 —— 生产者-消费者模型
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
 *   └──────────────────────────────────────────────────────────────┘
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ Reactor 模式 (m_actor_model == 1)                            │
 *   │   主线程: 只负责事件分发（往队列里塞任务）                     │
 *   │   工作线程: 负责 I/O + 业务逻辑                               │
 *   │   优点: 主线程更轻量，I/O 操作可以并行执行                    │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 为什么用信号量而非条件变量？
 *   - 信号量的语义是"资源的计数"，天然适合表示"队列中有多少个任务"
 *   - 条件变量需要配合 while 循环防止虚假唤醒，代码更繁琐
 *   - sem_wait/sem_post 接口更简洁
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
#include <pthread.h>
#include "../sync/locker.h"

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
     * @param thread_number 工作线程数量（建议设置为 CPU 核心数）
     * @param max_requests  请求队列最大长度（防止任务堆积导致 OOM）
     */
    ThreadPool(int actor_model, int thread_number = 0, int max_requests = 10000);
    
    /**
     * @brief 析构函数：释放线程数组
     *
     * 注意：这个简单的实现不包含优雅关闭逻辑（join 所有线程），
     * 因为服务器通常是一直运行，退出时由 OS 回收资源。
     * 阶段 4 会加入优雅关闭。
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
     * 无限循环：
     *   1. sem_wait() 阻塞等待任务
     *   2. 从队列取出任务
     *   3. 根据 actor_model 执行不同逻辑
     */
    void run();

private:
    int m_thread_number;         // 工作线程数量
    int m_max_requests;          // 请求队列最大长度
    pthread_t* m_threads;        // 线程 ID 数组（动态分配）
    std::list<T*> m_workqueue;   // 请求队列（使用 list 方便头删尾插）
    MutexLock m_queuelocker;     // 保护请求队列的互斥锁
    Semaphore m_queuestat;       // 信号量：队列中待处理的任务数量
    int m_actor_model;           // 并发模型：0=Proactor, 1=Reactor

};

// ============================================================
// 构造函数
// ============================================================
template <typename T>
ThreadPool<T>::ThreadPool(int actor_model, int thread_number, int max_requests)
    : m_actor_model(actor_model)
    , m_thread_number(thread_number)
    , m_max_requests(max_requests)
    , m_threads(nullptr)
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
            throw std::exception();
        }
    }
    
    const char* model_str = (m_actor_model == 1) ? "Reactor" : "Proactor";
    printf("[线程池] 创建成功: %d 个线程, 队列上限 %d, 模式: %s\n",
           m_thread_number, m_max_requests, model_str);
}

// ============================================================
// 析构函数
// ============================================================
template<typename T>
ThreadPool<T>::~ThreadPool()
{
    delete[] m_threads;
    printf("[线程池] 已销毁\n");
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

    if (m_workqueue.size() >= static_cast<size_t>(m_max_requests)) {
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
    return pool;
}

// ============================================================
// 工作线程主循环
//
// 这是线程池的核心逻辑。每个工作线程都在这个函数中无限循环：
//   1. 在信号量上阻塞，等待有任务到来
//   2. 被唤醒后，从队列头部取出一个任务
//   3. 根据并发模型执行不同的处理逻辑
//   4. 回到步骤 1
// ============================================================
template <typename T>
void ThreadPool<T>::run()
{
    while (true) {
        // ---- 第 1 步：P 操作，阻塞等待任务 ----
        m_queuestat.wait();

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
        // 如果出错（连接断开），设置 timer_flag = 1（阶段 4 用于定时器）。
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
                    // 执行http业务处理（解析＋构建响应）
                    request->process();
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
                // 工作线程亲自执行io写入
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
        // ==========================================================
        else
        {
            request->process(); // 执行http业务处理
        }
    }

}

#endif // XWEBSERVER_THREADPOOL_H