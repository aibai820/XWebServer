/**
 * @file    sql_connection_pool.cpp
 * @brief   MySQL 连接池实现
 *
 * 实现 connection_pool 单例和 connectionRAII 的构造/析构逻辑。
 *
 * 线程安全机制：
 *   - 信号量（Semaphore）: 控制可用连接数，当无可用连接时阻塞等待
 *   - 互斥锁（MutexLock）: 保护对 connList 容器的并发读写
 *   - 两者配合：sem_wait 在 lock 之前，避免持锁等待
 *
 * 参考：TinyWebServer 原项目的 sql_connection_pool.cpp
 */
#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#endif
#include <cstdio>
#include <string>
#include <cstring>
#include <cstdlib>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"
#include "../log/log.h"

// ============================================================
// connection_pool 构造与析构
// ============================================================

connection_pool::connection_pool()
    : m_MaxConn(0)
    , m_CurConn(0)
    , m_FreeConn(0)
    , reserve_ptr_(nullptr)
    , m_Port(0)
    , m_close_log(0)
{
}

connection_pool::~connection_pool() 
{
    // 销毁所有连接
    DestroyPool();

    // 释放信号量指针
    if (reserve_ptr_) {
        delete reserve_ptr_;
        reserve_ptr_ = nullptr;
    }
}

// ============================================================
// init(): 初始化连接池
//
// 步骤：
//   1. 保存数据库连接参数
//   2. 循环创建 MaxConn 个 MySQL 连接
//   3. 每个连接通过 mysql_init + mysql_real_connect 建立
//   4. 存入 connList
//   5. 初始化信号量为当前空闲连接数
// ============================================================
void connection_pool::init(std::string url, std::string User,
                           std::string PassWord, std::string DBName,
                           int Port, int MaxConn, int close_log)
{
    // ---- 保存参数 ----
    m_url          = url;
    m_Port         = Port;
    m_User         = User;
    m_PassWord     = PassWord;
    m_DatabaseName = DBName;
    m_close_log    = close_log;

    // ---- 创建数据库连接 ----
#ifdef HAVE_MYSQL
    for (int i = 0; i < MaxConn; i++) {
        MYSQL* con = nullptr;

        // mysql_init: 分配并初始化 MYSQL 对象
        con = mysql_init(con);
        if (con == nullptr) {
            LOG_ERROR("[连接池] MySQL 初始化失败");
            exit(1);
        }

        // mysql_real_connect: 建立到数据库服务器的连接
        // 参数: MYSQL对象, 主机, 用户名, 密码, 数据库名, 端口, Unix socket(NULL), 客户端标志(0)
        con = mysql_real_connect(con, url.c_str(), User.c_str(),
                                  PassWord.c_str(), DBName.c_str(),
                                  Port, nullptr, 0);
        if (con == nullptr) {
            LOG_ERROR("[连接池] MySQL 连接失败: %s", mysql_error(con));
            exit(1);
        }

        // 设置字符集为 UTF-8（支持中文用户名和密码）
        mysql_set_character_set(con, "utf8");

        // 加入连接列表
        connList.push_back(con);
        ++m_FreeConn;
    }

    // ---- 初始化信号量 ----
    // 信号量的初始值 = 空闲连接数 = MaxConn
    // 信号量用于控制并发获取：sem_wait 等待可用连接，sem_post 归还连接
    reserve_ptr_ = new Semaphore(m_FreeConn);

    m_MaxConn = m_FreeConn;

    LOG_INFO("[连接池] 初始化完成: %d 个连接 (host=%s, db=%s)",
             m_MaxConn, url.c_str(), DBName.c_str());
#else
    // MySQL 不可用：连接池保持为空
    reserve_ptr_ = new Semaphore(0);
    m_MaxConn = 0;
    m_FreeConn = 0;
    LOG_WARN("[连接池] MySQL 不可用，连接池为空（仅静态文件服务可用）");
#endif
}

// ============================================================
// GetConnection(): 从连接池获取一个可用连接
//
// 步骤：
//   1. 如果 connList 为空 → 返回 nullptr
//   2. sem_wait (P 操作) → 等待可用连接（如果无可用则阻塞）
//   3. 加锁 → 从列表头部取出一个连接 → 解锁
//   4. 更新计数器（空闲-1, 已用+1）
//
// 注意 sem_wait 在 lock 之前：
//   如果先 lock 再 sem_wait，当无可用连接时，线程持锁阻塞，
//   其他线程无法归还连接（ReleaseConnection 也需要锁）→ 死锁！
//   先 sem_wait 确保在持锁之前就已经"预定"了一个连接。
// ============================================================
MYSQL* connection_pool::GetConnection() 
{
    MYSQL* con = nullptr;

    // 防御：连接池为空
    if (connList.empty()) {
        LOG_ERROR("[连接池] 连接列表为空，无法获取连接");
        return nullptr;
    }

    // P 操作：等待可用连接（如果当前无空闲连接，在此阻塞）
    reserve_ptr_->wait();

    // 加锁，从列表取出一个连接
    lock.lock();

    con = connList.front();
    connList.pop_front();

    // 更新计数器
    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();

    return con;
}

// ============================================================
// ReleaseConnection(): 将连接归还到连接池
//
// 步骤：
//   1. 防御：conn 为空 → 返回 false
//   2. 加锁 → 将连接放回列表尾部 → 更新计数器 → 解锁
//   3. sem_post (V 操作) → 唤醒一个等待的线程
//
// 归还后信号量 +1，如果有线程在 GetConnection 中阻塞，
// 将被唤醒并获取这个归还的连接。
// ============================================================
bool connection_pool::ReleaseConnection(MYSQL* conn) 
{
    if (conn == nullptr) {
        return false;
    }

    lock.lock();

    connList.push_back(conn);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    // V 操作：信号量 +1，唤醒等待的线程
    reserve_ptr_->post();

    return true;
}

// ============================================================
// DestroyPool(): 销毁所有数据库连接
//
// 遍历连接列表，对每个连接调用 mysql_close 关闭。
// 同时重置所有计数器。
// ============================================================
void connection_pool::DestroyPool() 
{
    lock.lock();

    if (!connList.empty()) {
#ifdef HAVE_MYSQL
        // 遍历并关闭所有连接
        for (auto it = connList.begin(); it != connList.end(); ++it) {
            MYSQL* con = *it;
            mysql_close(con);
        }
#else
        // MySQL 不可用，列表中的指针无效，直接清空即可
#endif

        m_CurConn  = 0;
        m_FreeConn = 0;
        connList.clear();
    }

    lock.unlock();

    LOG_INFO("[连接池] 所有连接已销毁");
}

// ============================================================
// GetFreeConn(): 获取当前空闲连接数
// ============================================================
int connection_pool::GetFreeConn() {
    return m_FreeConn;
}

// ============================================================
// connectionRAII 构造与析构
// ============================================================

/**
 * @brief 构造函数：从连接池获取连接
 *
 * 通过双重指针 MYSQL** SQL 来修改调用方的 MYSQL* 变量。
 * 这样调用方可以写成:
 *   MYSQL* mysql = nullptr;
 *   connectionRAII mysqlcon(&mysql, pool);
 *   // mysql 现在指向可用连接
 *
 * @param SQL      指向 MYSQL* 的指针（用于输出获取到的连接）
 * @param connPool 连接池指针
 */
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
    *SQL = connPool->GetConnection();

    conRAII  = *SQL;
    poolRAII = connPool;
}

/**
 * @brief 析构函数：归还连接
 *
 * C++ 保证析构函数在离开作用域时被调用，
 * 即使是因为异常而退出作用域。
 * 这确保了连接不会泄漏。
 */
connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}