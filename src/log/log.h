/**
 * @file    log.h
 * @brief   异步/同步日志系统 —— 单例模式
 *
 * 设计要点：
 *
 *   1. 单例模式（Singleton）
 *      - C++11 起，函数内 static 局部变量初始化是线程安全的
 *      - 无需手动加锁（"魔法静态变量" / Magic Statics）
 *      - get_instance() 返回 static 局部实例的指针
 *
 *   2. 同步 vs 异步写入
 *      - 同步模式 (max_queue_size=0)：write_log() 直接 fputs() 到文件
 *      - 异步模式 (max_queue_size>=1)：write_log() 将日志字符串 push 到阻塞队列，
 *        后台线程 pop 后写入文件，减少主线程的 I/O 阻塞
 *
 *   3. 日志滚动（Rotation）
 *      - 按天滚动：日期变化时，关闭当前文件，创建新的日期前缀文件
 *      - 按行数滚动：累计行数达到 m_split_lines 时，创建带序号的备份文件
 *        （类似 logrotate 的 rotate 机制）
 *
 *   4. 日志格式
 *      [日期 时间.微秒] [级别]: 用户消息
 *      例: 2026-07-22 14:35:12.123456 [info]: client connected fd=5
 *
 *   5. 四个日志级别
 *      DEBUG (0) - 调试信息，仅开发环境
 *      INFO  (1) - 常规运行信息
 *      WARN  (2) - 警告，不影响运行但需要关注
 *      ERROR (3) - 错误，需要立即处理
 *
 * 使用方式：
 *   Log::get_instance()->init("./ServerLog", 0, 8192, 5000000, 800);
 *   LOG_INFO("Server started on port %d", port);
 *   LOG_ERROR("Failed to open file: %s", strerror(errno));
 *
 * 参考：TinyWebServer 原项目的日志设计
 */

#ifndef XWEBSERVER_LOG_H
#define XWEBSERVER_LOG_H

#include <cstdio>
#include <iostream>
#include <string>
#include <cstdarg>
#include <pthread.h>
#include "block_queue.h"

/**
 * @brief 日志单例类
 *
 * 线程安全的日志系统，支持同步/异步写入、四种级别、
 * 按天和按行数的日志滚动。
 */
class Log
{
public:
    /**
     * @brief 获取 Log 单例实例（C++11 线程安全）
     *
     * 利用 C++11 的 "魔法静态变量" 特性：
     * 函数内的 static 局部变量在首次执行到该语句时初始化，
     * 并且编译器保证多线程环境下只初始化一次。
     *
     * @return Log*  单例指针
     */
    static Log* get_instance()
    {
        static Log instance;
        return &instance;
    }

    /**
     * @brief 异步日志工作线程的入口函数（静态）
     *
     * 因为 pthread_create 需要一个 C 风格的函数指针，
     * 无法直接使用成员函数（有隐式 this 参数），
     * 所以用静态函数做跳板，调用单例的 async_write_log()。
     *
     * @param args  未使用（线程参数，此处不需要）
     * @return nullptr
     */
    static void* flush_log_thread(void* args) 
    {
        (void)args;  // 明确标记未使用参数
        Log::get_instance()->async_write_log();
        return nullptr;
    }

    /**
     * @brief 初始化日志系统
     *
     * @param file_name      日志文件路径（可包含目录），如 "./logs/ServerLog"
     *                       最终文件名会加日期前缀：logs/2026_07_22_ServerLog
     * @param close_log      是否关闭日志：0=启用, 1=禁用所有日志输出
     * @param log_buf_size   内部格式化缓冲区大小（字节，默认 8192）
     * @param split_lines    日志文件最大行数（超出后滚动，默认 5000000）
     * @param max_queue_size 异步队列最大长度：0=同步模式, >=1=异步模式
     *                       （建议异步模式设为 800~1024）
     * @return true=初始化成功, false=打开文件失败
     */
    bool init(const char* file_name, int close_log,
              int log_buf_size = 8192, int split_lines = 5000000,
              int max_queue_size = 0);

    /**
     * @brief 写入一条日志
     *
     * @param level   日志级别：0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
     * @param format  printf 风格的格式化字符串
     * @param ...     可变参数列表
     *
     * 线程安全：内部使用互斥锁保护文件写入和计数器修改。
     * 异步模式下，格式化后的字符串被 push 到阻塞队列。
     * 同步模式下，直接 fputs() 到文件并使用 fflush()。
     */
    void write_log(int level, const char* format, ...);

    /**
     * @brief 强制刷新文件流缓冲区
     *
     * 将用户态缓冲区中的数据立即写入磁盘。
     * 同步模式下每条日志自动 flush，异步模式由后台线程定期 flush。
     */
    void flush();

    /**
     * @brief 获取日志开关状态
     * @return 0=日志启用, 1=日志禁用
     *
     * 供 LOG_DEBUG/LOG_INFO 等宏使用，避免在禁用日志时执行
     * 不必要的函数调用和字符串格式化。
     */
    int get_close_log() const { return m_close_log; }

private:
    /**
     * @brief 私有构造函数（单例模式）
     *
     * 初始化所有成员为安全默认值。
     * 注意：构造函数不打开文件，文件在 init() 中打开。
     */
    Log();

    /**
     * @brief 私有析构函数
     *
     * 关闭日志文件。注意：不删除 m_log_queue，
     * 因为后台线程可能还在使用。简化处理：进程退出时 OS 回收。
     */
    ~Log();

    /**
     * @brief 异步写日志的工作循环
     *
     * 在后台线程中运行，无限循环从阻塞队列 pop() 日志字符串，
     * 然后 fputs() 写入文件。
     *
     * 为什么用 while + pop() 而非检查标志？
     *   pop() 是阻塞的——队列空时线程自动挂起，不消耗 CPU。
     *   进程退出时，OS 会回收线程，无需显式停止。
     */
    void* async_write_log() 
    {
        std::string single_log;

        // 循环从阻塞队列中取出日志字符串并写入文件
        while (m_log_queue->pop(single_log)) {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }

        return nullptr;
    }
private:
    // ---- 文件路径 ----
    char dir_name[128];          // 日志目录路径（含末尾 /）
    char log_name[128];         // 日志文件名（不含目录和日期前缀）

    // ---- 配置 ----
    int       m_split_lines;    // 日志文件最大行数（触发滚动）
    int       m_log_buf_size;   // 内部格式化缓冲区大小
    long long m_count;          // 当前文件已写入的行数（用于行数滚动判断）
    int       m_today;          // 当前日期的 "日" 部分（用于按天滚动判断）

    // ---- 文件 I/O ----
    FILE* m_fp;                  // 当前打开的日志文件指针

    // ---- 缓冲区 ----
    char* m_buf;                 // 内部格式化缓冲区（动态分配，避免栈溢出）

    // ---- 异步日志 ----
    block_queue<std::string>* m_log_queue;   // 阻塞队列（异步模式下使用）
    bool m_is_async;                         // true=异步模式, false=同步模式

    // ---- 线程安全 ----
    MutexLock m_mutex;           // 互斥锁，保护文件写入和计数器

    // ---- 开关 ----
    int m_close_log;             // 日志开关：0=启用, 1=禁用
};

// ============================================================
// 便捷日志宏
//
// 使用宏而非函数的优势：
//   1. 可以自动获取 __VA_ARGS__（可变参数）
//   2. 在 m_close_log != 0 时完全跳过函数调用
//   3. 自动在每条日志后 flush
//
// ##__VA_ARGS__ 的 ## 是 GNU 扩展：
//   当可变参数为空时，## 可以吞掉前面的逗号，避免语法错误
//   例如 LOG_DEBUG("hello") → write_log(0, "hello") 而不是 write_log(0, "hello",)
// ============================================================

/**
 * @brief 输出 DEBUG 级别日志
 * @note  仅在开发调试时使用，生产环境建议关闭
 */
#define LOG_DEBUG(format, ...) \
    if (0 == Log::get_instance()->get_close_log()) { \
        Log::get_instance()->write_log(0, format, ##__VA_ARGS__); \
        Log::get_instance()->flush(); \
    }

/**
 * @brief 输出 INFO 级别日志
 * @note  记录服务器正常运行事件（连接、请求、配置等）
 */
#define LOG_INFO(format, ...) \
    if (0 == Log::get_instance()->get_close_log()) { \
        Log::get_instance()->write_log(1, format, ##__VA_ARGS__); \
        Log::get_instance()->flush(); \
    }

/**
 * @brief 输出 WARN 级别日志
 * @note  记录需要关注但不影响运行的警告（队列满、重试等）
 */
#define LOG_WARN(format, ...) \
    if (0 == Log::get_instance()->get_close_log()) { \
        Log::get_instance()->write_log(2, format, ##__VA_ARGS__); \
        Log::get_instance()->flush(); \
    }

/**
 * @brief 输出 ERROR 级别日志
 * @note  记录错误事件（连接失败、系统调用错误等）
 */
#define LOG_ERROR(format, ...) \
    if (0 == Log::get_instance()->get_close_log()) { \
        Log::get_instance()->write_log(3, format, ##__VA_ARGS__); \
        Log::get_instance()->flush(); \
    }

#endif // XWEBSERVER_LOG_H