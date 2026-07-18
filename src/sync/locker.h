/**
 * @file    locker.h
 * @brief   POSIX 线程同步原语的 RAII 封装
 *
 * 为什么需要这些封装？
 *   1. RAII（Resource Acquisition Is Initialization）保证锁在离开作用域时自动释放
 *   2. 异常安全：即使函数抛异常，析构函数也会执行 unlock/sem_post
 *   3. 防止忘记解锁导致死锁
 *
 * 使用示例：
 *   locker m_lock;
 *   m_lock.lock();
 *   // ... 临界区代码 ...
 *   m_lock.unlock();  // 也可以用 std::lock_guard 模式
 *
 * 延伸学习：
 *   - C++11 的 std::mutex / std::lock_guard / std::unique_lock 是同样原理
 *   - 这里用 POSIX 接口是为了深入理解底层机制
 */

#ifndef XWEBSERVER_LOCKER_H
#define XWEBSERVER_LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// ============================================================
// 信号量封装
// 信号量用于控制对有限资源的并发访问数量
// 不像互斥锁只能 0/1，信号量可以设置任意初始值
// ============================================================
class Semaphore {
public:
    // 默认构造：初始值为 0，用于生产者-消费者模型
    // 消费者调用 wait() 会阻塞直到生产者调用 post()
    Semaphore() {
        if (sem_init(&sem_, 0, 0) != 0) {
            throw std::exception();
        }
    }

    // 指定初始值的构造：例如连接池初始化时设置 sem(N)，
    // 表示有 N 个可用连接
    explicit Semaphore(int initial_value) {
        if (sem_init(&sem_, 0, initial_value) != 0) {
            throw std::exception();
        }
    }

    ~Semaphore() {
        sem_destroy(&sem_);
    }

    // P 操作（荷兰语 Proberen，尝试）：等待信号量 > 0 然后减 1
    // 如果信号量为 0 则阻塞
    bool wait() {
        return sem_wait(&sem_) == 0;
    }

    // V 操作（荷兰语 Verhogen，增加）：信号量加 1
    // 如果有线程在 wait() 上阻塞，会唤醒其中一个
    bool post() {
        return sem_post(&sem_) == 0;
    }

private:
    sem_t sem_;
};

// ============================================================
// 互斥锁封装
// 互斥锁（Mutual Exclusion）：保证同一时刻只有一个线程访问临界区
// ============================================================
class MutexLock {
public:
    MutexLock() {
        if (pthread_mutex_init(&mutex_, nullptr) != 0) {
            throw std::exception();
        }
    }

    ~MutexLock() {
        pthread_mutex_destroy(&mutex_);
    }

    bool lock() {
        return pthread_mutex_lock(&mutex_) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&mutex_) == 0;
    }

    // 暴露底层 pthread_mutex_t 指针
    // 条件变量需要配合互斥锁使用，所以暴露它是必要的
    pthread_mutex_t* get() {
        return &mutex_;
    }

private:
    pthread_mutex_t mutex_;
};

// ============================================================
// 条件变量封装
// 条件变量让线程等待"某个条件成立"而不是忙等（busy-wait）
// 必须配合互斥锁使用
//
// 典型用法（消费者）：
//   mutex.lock();
//   while (queue.empty()) {        // 用 while 而非 if，防止虚假唤醒
//       cond.wait(mutex.get());     // 原子地释放锁并进入等待
//   }                               // 被唤醒时自动重新获取锁
//   item = queue.pop();
//   mutex.unlock();
// ============================================================
class ConditionVar {
public:
    ConditionVar() {
        if (pthread_cond_init(&cond_, nullptr) != 0) {
            throw std::exception();
        }
    }

    ~ConditionVar() {
        pthread_cond_destroy(&cond_);
    }

    // 等待条件变量被 signal
    // 调用前必须已经 lock 了 mutex
    // 内部会：原子地 unlock mutex → 进入等待 → 被唤醒后 lock mutex
    bool wait(pthread_mutex_t* mutex) {
        return pthread_cond_wait(&cond_, mutex) == 0;
    }

    // 带超时的等待
    // t: 绝对时间（从 Epoch 开始的秒+纳秒），用 clock_gettime() 获取
    // 超时返回 false，而非阻塞到天荒地老
    bool timed_wait(pthread_mutex_t* mutex, struct timespec t) {
        return pthread_cond_timedwait(&cond_, mutex, &t) == 0;
    }

    // 唤醒一个等待线程
    bool signal() {
        return pthread_cond_signal(&cond_) == 0;
    }

    // 唤醒所有等待线程（惊群效应）
    bool broadcast() {
        return pthread_cond_broadcast(&cond_) == 0;
    }

private:
    pthread_cond_t cond_;
};

#endif  // MY_WEBSERVER_LOCKER_H
