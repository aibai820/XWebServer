/**
 * @file    log.cpp
 * @brief   日志系统实现
 *
 * 实现 Log 单例的构造/析构、init、write_log、flush 方法。
 *
 * 日志滚动逻辑：
 *   - 按天滚动：每天生成一个新文件（日期作为文件名前缀）
 *   - 按行数滚动：当文件行数达到 m_split_lines 时，
 *     关闭当前文件，生成带递增序号的新文件
 *
 * 时间格式：精确到微秒（gettimeofday）
 *
 * 参考：TinyWebServer 原项目的日志实现
 */

#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <cstdarg>
#include <pthread.h>
#include "log.h"

// ============================================================
// 构造函数：初始化所有成员为安全默认值
// ============================================================
Log::Log()
    : m_split_lines(5000000)
    , m_log_buf_size(8192)
    , m_count(0)
    , m_today(0)
    , m_fp(nullptr)
    , m_buf(nullptr)
    , m_log_queue(nullptr)
    , m_is_async(false)
    , m_close_log(0)          // 默认启用日志
{
    memset(dir_name, 0, sizeof(dir_name));
    memset(log_name, 0, sizeof(log_name));
}

// ============================================================
// 析构函数：关闭日志文件
//
// 注意：不负责删除 m_log_queue 和 m_buf。
//   1. m_log_queue：后台线程可能还在 pop()，强行 delete 可能导致 crash
//   2. m_buf：delete[] 放在这里是可以的，但 init() 可能多次调用
//      简化处理：进程退出时 OS 回收所有内存
// 这是一个简化的析构，生产环境可以用引用计数或 shared_ptr 改进。
// ============================================================
Log::~Log() 
{
    if (m_fp != nullptr) {
        fclose(m_fp);
        m_fp = nullptr;
    }
}

// ============================================================
// init(): 初始化日志系统
//
// 步骤：
//   1. 判断是否需要异步模式（max_queue_size >= 1）
//   2. 异步模式：创建阻塞队列 + 启动后台写线程
//   3. 分配内部格式化缓冲区
//   4. 解析日志文件路径（目录 + 文件名）
//   5. 生成带日期前缀的完整文件名
//   6. 打开文件（追加模式）
// ============================================================
bool Log::init(const char* file_name, int close_log,
               int log_buf_size, int split_lines, int max_queue_size)
{
    // ---- 第 1 步：异步模式初始化 ----
    // max_queue_size >= 1 → 启用异步日志
    // max_queue_size == 0 → 同步日志（默认）
    if (max_queue_size >= 1) {
        m_is_async   = true;

        // 创建阻塞队列（容量 = max_queue_size）
        m_log_queue = new block_queue<std::string>(max_queue_size);

        // 启动后台写日志线程
        // flush_log_thread 是静态方法，内部调用单例的 async_write_log()
        pthread_t tid;
        pthread_create(&tid, nullptr, flush_log_thread, nullptr);

        // detach：后台线程独立运行，不需要主线程等待
        // 进程退出时 OS 自动回收线程资源
        pthread_detach(tid);
    }

    // ---- 第 2 步：保存配置 ----
    m_close_log = close_log;

    // 分配内部格式化缓冲区
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    m_split_lines = split_lines;

    // ---- 第 3 步：解析文件路径 ----
    // 获取当前时间（用于日志文件名中的日期前缀）
    time_t t = time(nullptr);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;   // 拷贝到局部变量，因为 localtime 返回静态缓冲区

    char log_full_name[256] = {0};

    // 查找路径中最后一个 '/' 来分离目录和文件名
    const char* p = strrchr(file_name, '/');

    if (p == nullptr) {
        // 没有目录部分，纯文件名
        // 格式: 2026_07_22_文件名
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s",
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday, file_name);
    } else {
        // 有目录部分：分离目录和文件名
        // dir_name  = "/path/to/logs/"  (包含末尾的 /)
        // log_name  = "ServerLog"
        strcpy(log_name, p + 1);                             // 文件名（/ 之后的部分）
        strncpy(dir_name, file_name, p - file_name + 1);     // 目录（含 /）
        dir_name[p - file_name + 1] = '\0';                  // 确保 NUL 结尾

        // 格式: /path/to/logs/2026_07_22_ServerLog
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s",
                 dir_name, my_tm.tm_year + 1900,
                 my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    // ---- 第 4 步：记录当前日期（用于后续按天滚动检测） ----
    m_today = my_tm.tm_mday;

    // ---- 第 5 步：以追加模式打开日志文件 ----
    m_fp = fopen(log_full_name, "a");
    if (m_fp == nullptr) {
        std::cerr << "[Log] 错误：无法创建日志文件 " << log_full_name << std::endl;
        return false;
    }

    printf("[日志] 初始化完成: %s  (模式: %s, 缓冲区: %d, 滚动行数: %d)\n",
           log_full_name,
           m_is_async ? "异步" : "同步",
           m_log_buf_size, m_split_lines);

    return true;
}

// ============================================================
// write_log(): 写入一条日志
//
// 流程：
//   1. 获取当前时间（精确到微秒）
//   2. 根据 level 确定级别字符串（[debug]/[info]/[warn]/[erro]）
//   3. 检查是否需要滚动日志文件（按天 / 按行数）
//   4. 格式化日志内容（时间 + 级别 + 用户消息）
//   5. 异步模式：push 到阻塞队列；同步模式：直接 fputs
// ============================================================
void Log::write_log(int level, const char* format, ...)
{
    // ---- 获取高精度时间 ----
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr); // 精确到微秒
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    // ---- 级别字符串 ----
    char s[16] = {0};
    switch (level) {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");    // 保持与原项目一致（erro 而非 error）
        break;
    default:
        strcpy(s, "[info]:");    // 未知级别默认为 info
        break;
    }

    // ---- 日志滚动检查（需要互斥保护） ----
    m_mutex.lock();
    m_count++;

    // 触发滚动的条件：
    //   a) 日期变了（m_today != 当前日期）
    //   b) 行数达到阈值（且阈值 > 0，防止除以零）
    bool need_rotate = false;
    if (m_today != my_tm.tm_mday) {
        need_rotate = true;     // 跨天了
    } else if (m_split_lines > 0 && m_count % m_split_lines == 0) {
        need_rotate = true;     // 行数达到上限
    }

    if (need_rotate) {
        // 刷新并关闭当前文件
        fflush(m_fp);
        fclose(m_fp);

        // 日期尾部字符串
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_",
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday);

        char new_log[256] = {0};

        if (m_today != my_tm.tm_mday) {
            // 按天滚动：用新日期生成文件名
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;   // 更新日期
            m_count = 0;               // 重置行数计数
        } else {
            // 按行数滚动：在文件名后加序号
            // 例: logs/2026_07_22_ServerLog.1
            snprintf(new_log, 255, "%s%s%s.%lld",
                     dir_name, tail, log_name,
                     m_count / m_split_lines);
        }

        // 打开新文件
        m_fp = fopen(new_log, "a");
    }

    // ---- 格式化日志内容 ----
    va_list valst;
    va_start(valst, format);

    std::string log_str;

    m_mutex.lock();

    // 时间戳部分：YYYY-MM-DD HH:MM:SS.mmmmmm [级别]:
    // 占用约 48 字节
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                     my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec,
                     now.tv_usec, s);

    // 用户消息部分（格式化）
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);

    // 追加换行符和 NUL 终止符
    // 示例最终字符串: "2026-07-22 14:35:12.123456 [info]: Server started\n"
    m_buf[n + m]     = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    va_end(valst);

    // ---- 输出日志 ----
    if (m_is_async && !m_log_queue->full()) {
        // 异步模式：推入队列，后台线程负责写盘
        m_log_queue->push(log_str);
    } else {
        // 同步模式（或异步队列已满回退同步）：直接写入文件
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }
}

// ============================================================
// flush(): 强制刷新文件流缓冲区
// ============================================================
void Log::flush() {
    m_mutex.lock();
    // fflush: 将 C 标准库缓冲区中的数据写入内核
    // 确保即使程序异常退出，已写入的日志也不会丢失
    fflush(m_fp);
    m_mutex.unlock();
}
