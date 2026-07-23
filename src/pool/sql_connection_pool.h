/**
 * @file    sql_connection_pool.h
 * @brief   MySQL 数据库连接池 —— 单例模式 + RAII
 *
 * 设计思想：
 *
 *   1. 为什么需要连接池？
 *      每次 HTTP 请求都创建/销毁 MySQL 连接的成本很高：
 *        - TCP 三次握手
 *        - MySQL 认证握手
 *        - 服务器端线程/内存资源分配
 *      连接池预先创建一组连接并保持，请求到来时直接取用，
 *      用完后归还，避免了频繁建立/断开连接的开销。
 *
 *   2. 单例模式（Singleton）
 *      整个服务器只需要一个连接池。所有请求共享池中的连接。
 *      C++11 静态局部变量保证线程安全的懒加载初始化。
 *
 *   3. RAII 获取/释放（connectionRAII）
 *      获取连接：构造函数中调用 GetConnection()
 *      释放连接：析构函数中调用 ReleaseConnection()
 *      确保即使在异常情况下，连接也会被正确归还。
 *      这是 C++ 中管理资源的惯用方法（"资源获取即初始化"）。
 *
 *   4. 信号量控制并发
 *      信号量的初始值为最大连接数。
 *      GetConnection(): sem_wait (P 操作) —— 无可用连接时阻塞
 *      ReleaseConnection(): sem_post (V 操作) —— 归还后唤醒等待者
 *      信号量天然适合"资源池"的并发控制。
 *
 *   5. 互斥锁保护容器
 *      std::list<MYSQL*> 的 push/pop 操作不是线程安全的，
 *      使用 MutexLock 保护对连接列表的访问。
 *
 * 使用方式：
 *   // 初始化（服务器启动时调用一次）
 *   connection_pool* pool = connection_pool::GetInstance();
 *   pool->init("localhost", "root", "password", "mydb", 3306, 8, 0);
 *
 *   // 在请求处理中使用（RAII 自动管理）
 *   MYSQL* mysql = nullptr;
 *   connectionRAII mysqlcon(&mysql, pool);
 *   // ... 使用 mysql 进行查询 ...
 *   // 离开作用域时自动归还连接
 *
 * 参考：TinyWebServer 原项目的数据库连接池设计
 */

#ifndef XWEBSERVER_SQL_CONNECTION_POOL_H
#define XWEBSERVER_SQL_CONNECTION_POOL_H

#include <cstdio>
#include <list>
#include <string>

#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#else
// 当 MySQL 不可用时，定义 MYSQL 为不完整类型（仅用于指针声明）
// CGI 登录/注册功能将不可用，但服务器仍可正常提供静态文件服务
struct MYSQL;
#endif

#include "../sync/locker.h"

// 前置声明 Log 宏（实际包含在 .cpp 中）

/**
 * @brief MySQL 连接池单例类
 *
 * 维护一个 MYSQL* 连接的列表，通过信号量控制可用连接数。
 * 所有公有方法都是线程安全的。
 */
class connection_pool
{
public:
    /**
     * @brief 获取连接池唯一实例（C++11 线程安全单例）
     *
     * 利用 C++11 标准保证：函数内 static 局部变量的初始化
     * 在多线程环境下只执行一次，无需手动加锁。
     *
     * @return connection_pool*  单例指针
     */
    static connection_pool* GetInstance() 
    {
        static connection_pool connPool;
        return &connPool;
    }

    /**
     * @brief 初始化连接池
     *
     * 创建 MaxConn 个 MySQL 连接并放入池中。
     * 初始化信号量为 MaxConn（表示有 MaxConn 个可用连接）。
     *
     * @param url        数据库主机地址（如 "localhost" 或 IP）
     * @param User       数据库用户名
     * @param PassWord   数据库密码
     * @param DBName     数据库名称
     * @param Port       数据库端口（默认 3306）
     * @param MaxConn    连接池最大连接数（建议 8~16）
     * @param close_log  是否关闭日志：0=启用, 1=禁用
     */
    void init(std::string url, std::string User, std::string PassWord,
              std::string DBName, int Port, int MaxConn, int close_log);

    /**
     * @brief 从连接池获取一个可用连接
     *
     * 如果当前无可用连接（所有连接都被占用），
     * 调用线程会阻塞在信号量的 P 操作上，直到有连接被归还。
     *
     * @return MYSQL* 可用连接指针, 或 nullptr（池为空时）
     */
    MYSQL* GetConnection();

    /**
     * @brief 将连接归还到连接池
     *
     * 归还后执行信号量的 V 操作，唤醒可能正在等待的线程。
     *
     * @param conn  要归还的连接指针
     * @return true=归还成功, false=conn 为空
     */
    bool ReleaseConnection(MYSQL* conn);

    /**
     * @brief 获取当前空闲连接数
     * @return 空闲连接数量
     */
    int GetFreeConn();

    /**
     * @brief 销毁连接池：关闭所有 MySQL 连接并清空列表
     *
     * 通常在服务器关闭时调用（或由析构函数自动调用）。
     */
    void DestroyPool();

public:
    // ---- 数据库连接参数（公开，方便外部查看配置） ----
    std::string m_url;            // 数据库主机地址
    int         m_Port;           // 数据库端口
    std::string m_User;           // 数据库用户名
    std::string m_PassWord;       // 数据库密码
    std::string m_DatabaseName;   // 数据库名
    int         m_close_log;      // 日志开关

private:
    /**
     * @brief 私有构造函数（单例模式）
     *
     * 初始化计数器为 0。实际的连接创建在 init() 中完成。
     */
    connection_pool();

    /**
     * @brief 私有析构函数
     * 自动调用 DestroyPool() 关闭所有连接
     */
    ~connection_pool();

    // ---- 连接池参数 ----
    int m_MaxConn;         // 最大连接数（init 时设置）
    int m_CurConn;         // 当前已借出的连接数
    int m_FreeConn;        // 当前空闲的连接数

    // ---- 同步原语 ----
    MutexLock lock;              // 互斥锁：保护 connList 的读写
    std::list<MYSQL*> connList;  // 连接列表（存储所有 MYSQL* 指针）
    Semaphore* reserve_ptr_;     // 信号量指针：控制可用连接数
                                 // 使用指针而非值，因为 Semaphore 需要在 init()
                                 // 中根据 m_FreeConn 动态初始化
};

/**
 * @brief RAII 连接管理器
 *
 * 利用 C++ 的 RAII 机制自动获取和释放 MySQL 连接。
 * 在作用域结束时（包括因异常退出），析构函数自动归还连接。
 *
 * 使用示例：
 *   {
 *       MYSQL* mysql = nullptr;
 *       connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());
 *       // mysql 现在指向一个可用连接
 *       mysql_query(mysql, "SELECT ...");
 *   } // 离开作用域，连接自动归还
 *
 * 这种方式避免了忘记调用 ReleaseConnection 导致的连接泄漏。
 */
class connectionRAII {
public:
    /**
     * @brief 构造函数：从连接池获取一个连接
     *
     * @param SQL      [out] 指向 MYSQL* 的指针，用于接收获取到的连接
     * @param connPool 连接池指针
     *
     * 通过双重指针（MYSQL**）来修改外部的 MYSQL* 变量，
     * 使得调用方可以在获取连接后直接使用。
     */
    connectionRAII(MYSQL** SQL, connection_pool* connPool);

    /**
     * @brief 析构函数：将连接归还到连接池
     *
     * 自动释放（归还）在构造时获取的连接。
     * 即使发生异常，C++ 保证析构函数会被调用。
     */
    ~connectionRAII();

private:
    MYSQL*            conRAII;    // 持有此连接的指针（用于归还）
    connection_pool*  poolRAII;   // 连接池指针（用于归还时找到池）
};

#endif // XWEBSERVER_SQL_CONNECTION_POOL_H