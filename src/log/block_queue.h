/**
 * @file    block_queue.h
 * @brief   阻塞队列模板类 —— 循环数组实现
 *
 * 用途：
 *   作为异步日志系统的核心组件，生产者（主线程/工作线程）将日志字符串
 *   推入队列，消费者（后台日志线程）从队列中取出并写入磁盘。
 *   也适用于其他需要线程安全的生产者-消费者场景。
 *
 * 为什么用循环数组而非 std::queue？
 *   1. 固定容量，避免动态内存分配导致的延迟抖动
 *   2. 连续内存布局，缓存友好
 *   3. 完全的线程安全控制，不依赖外部容器的内部实现
 *
 * 线程安全：
 *   所有公有方法都先加锁再操作，然后解锁。
 *   push() 和 pop() 使用条件变量 notify/broadcast 来唤醒等待线程。
 *
 * 基于 TinyWebServer 原项目设计，适配本项目 locker.h 中的同步原语。
 */

#ifndef XWEBSERVER_BLOCK_QUEUE_H
#define XWEBSERVER_BLOCK_QUEUE_H

#include <iostream>
#include <cstdlib>
#include <pthread.h>
#include <sys/time.h>
#include "../sync/locker.h"

/**
 * @brief 阻塞队列模板类
 *
 * 循环数组实现，支持阻塞和非阻塞（超时）的出队操作。
 * 使用 MutexLock + ConditionVar 实现线程同步。
 *
 * @tparam T  队列中存储的元素类型
 *
 * 使用示例：
 *   block_queue<std::string> log_queue(1024);
 *   log_queue.push("log message");           // 生产者推入
 *   std::string msg;
 *   log_queue.pop(msg);                      // 消费者阻塞等待
 *   log_queue.pop(msg, 100);                 // 超时等待（100ms）
 */
template <typename T>
class block_queue
{
public:
    /**
     * @brief 构造函数：初始化循环数组
     *
     * @param max_size  队列最大容量（必须 > 0）
     *
     * 分配 max_size 个 T 的数组，初始化 front/back 指针和 size 计数器。
     * 队列初始为空。
     */
    explicit block_queue(int max_size = 1000) {
        if (max_size <= 0) {
            std::cerr << "[block_queue] 错误：max_size 必须大于 0" << std::endl;
            exit(-1);
        }

        m_max_size = max_size;
        m_array    = new T[max_size];
        m_size     = 0;
        m_front    = -1;   // 队列为空时 front 为 -1
        m_back     = -1;   // 队列为空时 back 为 -1
    }

    /**
     * @brief 析构函数：释放数组内存
     *
     * 注意：析构时不清空队列中的元素。
     * 如果 T 是指针类型，调用方应先清空队列以避免内存泄漏。
     */
    ~block_queue() {
        m_mutex.lock();
        if (m_array != nullptr) {
            delete[] m_array;
            m_array = nullptr;
        }
        m_mutex.unlock();
    }

    // 禁止拷贝（管理着动态分配的内存）
    block_queue(const block_queue&) = delete;
    block_queue& operator=(const block_queue&) = delete;

    // ============================================================
    // 状态查询（线程安全）
    // ============================================================

    /**
     * @brief 清空队列
     *
     * 重置所有指针和计数器。调用后队列回到初始空状态。
     * 注意：不清除旧元素的内容（T 若为指针需调用方自行管理）。
     */
    void clear()
    {
        m_mutex.lock();
        m_size  = 0;
        m_front = -1;
        m_back  = -1;
        m_mutex.unlock();
    }

    /**
     * @brief 判断队列是否已满
     * @return true=已满, false=未满
     */
    bool full() {
        m_mutex.lock();
        bool is_full = (m_size >= m_max_size);
        m_mutex.unlock();
        return is_full;
    }

    /**
     * @brief 判断队列是否为空
     * @return true=为空, false=非空
     */
    bool empty() {
        m_mutex.lock();
        bool is_empty = (m_size == 0);
        m_mutex.unlock();
        return is_empty;
    }

    /**
     * @brief 获取队首元素的引用（不出队）
     * @param value  [out] 接收队首元素
     * @return true=成功, false=队列为空
     */
    bool front(T& value) {
        m_mutex.lock();
        if (m_size == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    /**
     * @brief 获取队尾元素的引用（不出队）
     * @param value  [out] 接收队尾元素
     * @return true=成功, false=队列为空
     */
    bool back(T& value) {
        m_mutex.lock();
        if (m_size == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    /**
     * @brief 获取当前队列中的元素数量
     * @return 元素个数
     */
    int size() {
        m_mutex.lock();
        int sz = m_size;
        m_mutex.unlock();
        return sz;
    }

    /**
     * @brief 获取队列的最大容量
     * @return 最大容量
     */
    int max_size() {
        m_mutex.lock();
        int max = m_max_size;
        m_mutex.unlock();
        return max;
    }

    // ============================================================
    // 入队操作（生产者）
    // ============================================================

    /**
     * @brief 向队列尾部推入一个元素（生产者操作）
     *
     * 如果队列已满，广播通知所有等待线程（让他们检查状态），
     * 然后返回 false。调用方可以重试或丢弃。
     *
     * 如果队列未满，元素被添加到 back 位置，size+1，
     * 然后 broadcast() 唤醒所有在 pop() 上阻塞的消费者线程。
     *
     * @param item  要推入的元素（const 引用）
     * @return true=推入成功, false=队列已满
     */
    bool push(const T& item) {
        m_mutex.lock();

        // 队列已满：通知等待者，返回失败
        if (m_size >= m_max_size) {
            m_cond.broadcast();      // 唤醒所有等待线程（他们需要检查状态）
            m_mutex.unlock();
            return false;
        }

        // 计算新的 back 位置（循环数组，取模实现绕回）
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;

        // 如果是第一个元素，front 需要指向它
        if (m_size == 0) {
            m_front = m_back;
        }

        m_size++;

        m_cond.broadcast();          // 通知消费者有数据了
        m_mutex.unlock();
        return true;
    }

    // ============================================================
    // 出队操作（消费者）—— 阻塞版本
    // ============================================================

    /**
     * @brief 从队列头部取出一个元素（消费者操作，阻塞等待）
     *
     * 如果队列为空，调用线程将阻塞在条件变量上，
     * 直到有生产者 push() 元素并 broadcast() 唤醒。
     *
     * 被唤醒后，使用 while 循环（而非 if）重新检查队列状态，
     * 以防止虚假唤醒（spurious wakeup）。
     *
     * @param item  [out] 接收取出的元素
     * @return true=成功, false=条件变量等待失败
     */
    bool pop(T& item) {
        m_mutex.lock();

        // while 而非 if：防止虚假唤醒
        while (m_size <= 0) {
            // wait() 原子地：释放锁 → 进入等待 → 被唤醒后重新获取锁
            if (!m_cond.wait(m_mutex.get())) {
                m_mutex.unlock();
                return false;   // 等待失败（通常是系统错误）
            }
        }

        // 从 front 取出元素，指针后移（循环取模）
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;

        m_mutex.unlock();
        return true;
    }

    // ============================================================
    // 出队操作（消费者）—— 超时版本
    // ============================================================

    /**
     * @brief 从队列头部取出一个元素（消费者操作，带超时）
     *
     * 如果队列为空，最多等待 ms_timeout 毫秒。
     * 超时后返回 false，调用方可以检查返回值并决定后续操作。
     *
     * 超时版本常用于：
     *   - 优雅关闭：定时检查 m_stop 标志
     *   - 非阻塞查询：不希望无限等待
     *
     * @param item        [out] 接收取出的元素
     * @param ms_timeout  最大等待时间（毫秒）
     * @return true=成功取出, false=超时或出错
     */
    bool pop(T& item, int ms_timeout) {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, nullptr);

        m_mutex.lock();

        // 队列为空，等待指定时间
        if (m_size <= 0) {
            // 计算绝对超时时间
            t.tv_sec  = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000000;   // 毫秒 → 纳秒

            // 带超时的条件变量等待
            if (!m_cond.timed_wait(m_mutex.get(), t)) {
                m_mutex.unlock();
                return false;   // 超时
            }
        }

        // 再次检查（可能仍为空，例如虚假唤醒后超时）
        if (m_size <= 0) {
            m_mutex.unlock();
            return false;
        }

        // 取出元素
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;

        m_mutex.unlock();
        return true;
    }
private:
    // ---- 同步原语 ----
    MutexLock    m_mutex;       // 互斥锁，保护所有成员变量
    ConditionVar m_cond;        // 条件变量，用于消费者阻塞等待

    // ---- 数据存储 ----
    T*  m_array;       // 循环数组（动态分配）
    int m_size;        // 当前队列中的元素数量
    int m_max_size;    // 数组最大容量
    int m_front;       // 队首索引（下一次 pop 的位置）
    int m_back;        // 队尾索引（上一次 push 的位置）
};


#endif // XWEBSERVER_BLOCK_QUEUE_H