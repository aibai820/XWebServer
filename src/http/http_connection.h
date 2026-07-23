/**
 * @file    http_connection.h
 * @brief   HTTP 连接处理类
 *
 * 阶段 1（已完成）：简单的 echo——收到什么返回什么
 * 阶段 2（已完成）：完整的 HTTP/1.1 协议解析 + 静态文件服务
 * 阶段 3（已完成）：集成线程池，实现 Reactor/Proactor 模式
 * 阶段 4（已完成）：CGI 登录/注册 + MySQL 数据库集成 + 定时器协同
 *
 * 关键设计：
 *   - 每个连接有一个独立对象，状态全部自包含
 *   - m_epollfd 是静态成员，所有连接共享同一个 epoll 实例
 *   - m_user_count 是静态成员，跟踪全局活跃连接数
 *   - users_ 是静态成员，缓存数据库中的用户凭据（避免每次查 DB）
 */
#ifndef XWEBSERVER_HTTP_CONNECTION_H
#define XWEBSERVER_HTTP_CONNECTION_H

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <map>
#include <string>


#ifdef HAVE_MYSQL
#include <mysql/mysql.h>
#else
struct MYSQL;
#endif


// ============================================================
// 常量定义
// ============================================================
constexpr int READ_BUFFER_SIZE  = 2048;   // 读缓冲区大小
constexpr int WRITE_BUFFER_SIZE = 1024;   // 写缓冲区大小
constexpr int FILENAME_LEN      = 200;    // 文件路径最大长度

// 前置声明：避免循环依赖
class connection_pool;

class HttpConnection
{
public:
    // ----------------------------------------------------------
    // HTTP 方法
    // ----------------------------------------------------------
    enum METHOD
    {
        GET = 0,
        POST = 1
    };

    // ----------------------------------------------------------
    // 主状态机：解析 HTTP 请求的三个阶段
    // ----------------------------------------------------------
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, // 正在解析请求行
        CHECK_STATE_HEADER,          // 正在解析头部
        CHECK_STATE_CONTENT          // 正在解析正文
    };

    // ----------------------------------------------------------
    // HTTP 请求处理结果
    // ----------------------------------------------------------
    enum HTTP_CODE {
        NO_REQUEST,          // 请求不完整，需要继续读取
        GET_REQUEST,         // 请求解析完毕
        BAD_REQUEST,         // 请求语法错误
        NO_RESOURCE,         // 文件不存在
        FORBIDDEN_REQUEST,   // 文件无权限
        FILE_REQUEST,        // 文件请求成功
        INTERNAL_ERROR       // 服务器内部错误
    };

    // ----------------------------------------------------------
    // 行解析状态（从状态机）
    // ----------------------------------------------------------
    enum LINE_STATUS{
        LINE_OK = 0,  // 完整读入一行
        LINE_BAD,     // 行语法错误
        LINE_OPEN     // 行不完整，需要继续读取
    };

public:
    HttpConnection() = default;
    ~HttpConnection() = default;

    // ----------------------------------------------------------
    // 初始化连接（被 WebServer 在 accept 后调用）
    // @param sockfd     accept 返回的客户端 socket 文件描述符
    // @param addr       客户端地址
    // @param doc_root   文档根目录（静态文件路径）
    // @param TRIGMode   触发模式：0=LT, 1=ET
    // @param close_log  是否关闭日志：0=启用, 1=禁用（默认 1，向后兼容）
    // @param sql_user   MySQL 用户名（默认空，阶段 4/CGI 使用）
    // @param sql_passwd MySQL 密码（默认空）
    // @param sql_dbname MySQL 数据库名（默认空）
    // ----------------------------------------------------------
    void init(int sockfd, const sockaddr_in& addr,
              const char* doc_root, int TRIGMode,
              int close_log = 1,
              const char* sql_user = "",
              const char* sql_passwd = "",
              const char* sql_dbname = "");

    // 关闭连接：从 epoll 移除、close socket、活跃连接数 -1
    void close_conn();
    
    // ----------------------------------------------------------
    // I/O 操作
    // ----------------------------------------------------------

    // 从 socket 读数据到内部缓冲区
    // @return true=成功, false=连接断开或出错
    bool read_once();

    // HTTP 请求处理入口
    // 主状态机：process_read() → process_write() → 注册 EPOLLOUT
    void process();

    // 非阻塞写：使用 writev 发送 HTTP 响应头和文件内容
    // @return true=写完成（或等待 EPOLLOUT）, false=连接应关闭
    bool write();

    // ==========================================================
    // 静态成员：全局共享
    // ==========================================================
    static int  epoll_fd_;       // 共享的 epoll 文件描述符
    static int  user_count_;     // 当前活跃的连接数

    // 访问器
    int sockfd() const { return sockfd_; }
    const sockaddr_in& address() const{ return address_; }

    // ==========================================================
    // 用户凭据缓存（阶段 4：CGI 登录/注册）
    //
    // 服务器启动时从 MySQL user 表加载全部用户名和密码到内存。
    // 登录验证直接查内存 map，避免每次都查数据库。
    // 注册时先查内存 map（防重复），再 INSERT 到 MySQL
    // 并同步更新 map（保证一致性）。
    // ==========================================================

    /**
     * @brief 从 MySQL 加载用户凭据到内存 map
     *
     * 服务器启动时调用一次（由 WebServer::sql_pool() 触发）。
     * 使用 RAII 获取数据库连接，执行 SELECT username,passwd FROM user，
     * 将结果存入静态成员 users_ map。
     *
     * @param connPool  数据库连接池单例指针
     */
    static void initmysql_result(connection_pool* connPool);

    // ==========================================================
    // 线程池协同字段（阶段 3）
    // ==========================================================
    int m_state = 0;      // 任务类型：0=读事件, 1=写事件（Reactor 模式使用）
    int improv = 0;       // 同步标志：工作线程处理完毕后设为 1，主线程据此判断是否完成
    int timer_flag = 0;   // 定时器标志：1=连接已断开/出错，需要清理（阶段 4 使用）

private:
    // ---- 内部辅助：重置请求状态 ----
    void init_request();
    
    // ---- HTTP 解析（从状态机）----
    HTTP_CODE process_read();                // 主机状态
    bool process_write(HTTP_CODE ret);       // 构建HTTP响应

    // ---- 行解析 ----
    LINE_STATUS parse_line();
    char* get_line() { return read_buf_ + start_line_; } // 获取当前行指针

    // ---- 各阶段解析 ----
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);

    // ---- 文件处理 ----
    HTTP_CODE do_request();                  // URL路由 + stat + mmap
    void unmap();                            // munmap释放文件映射

    // ---- 响应构建 ----
    bool add_response(const char* format, ...);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char* content);

    // ---- 工具：获取 MIME 类型 ----
    static const char* get_mime_type(const char* url);

    // ---- 成员变量 ----
    int  sockfd_ = -1;                       // 客户端 socket fd
    sockaddr_in address_;                    // 客户端网络地址
    
    // 读缓冲区
    char read_buf_[READ_BUFFER_SIZE] = {};   // 读缓冲区
    int  read_idx_     = 0;                  // 读缓冲区已用长度
    int  checked_idx_  = 0;                  // 已解析到的位置
    int  start_line_   = 0;                  // 当前解析行的起始位置
    
    // 写缓冲区（HTTP 响应头）
    char write_buf_[WRITE_BUFFER_SIZE] = {}; // 写缓冲区
    int  write_idx_    = 0;                  // 写缓冲区已用长度

    // 主状态机
    CHECK_STATE check_state_ = CHECK_STATE_REQUESTLINE;
    METHOD      method_      = GET;

    // 请求解析结果
    char* url_        = nullptr;             // 请求 URL（指向 read_buf_）
    char* version_    = nullptr;             // HTTP 版本（指向 read_buf_）
    char* host_       = nullptr;             // Host 头部（指向 read_buf_）
    int   content_length_ = 0;               // Content-Length 值
    bool  linger_     = false;               // Connection: keep-alive

    // 文件服务
    char real_file_[FILENAME_LEN] = {};      // 实际文件路径（doc——root + url）
    char* file_addr_ = nullptr;              // mmap 映射的文件内容
    struct stat file_stat_ = {};             // 文件的stat信息
    
    // writev 所需的 iovec 结构
    // iv_[0]: HTTP 响应头 (在 write_buf_ 中)
    // iv_[1]: 文件内容 (mmap 映射)
    struct iovec iv_[2] = {};
    int iv_count_ = 0;

    // 发送进度
    int bytes_to_send_  = 0;                 // 总共需要发送的字节数
    int bytes_have_send_ = 0;                // 已经发送的字节数
    
    // 服务器配置（来自初始化）
    const char* doc_root_ = "static";        // 文档根目录
    int  trig_mode_ = 0;                     // 触发模式：0=LT, 1=ET

    // ==========================================================
    // CGI / POST 支持（阶段 4 新增）
    // ==========================================================
    int   cgi_        = 0;          // 是否启用 CGI 处理：0=否(GET), 1=是(POST)
    char* post_body_  = nullptr;    // POST 请求正文指针（指向 read_buf_ 中 content 起始位置）
    char  sql_user_[100]   = {};   // MySQL 用户名（从 WebServer 传入）
    char  sql_passwd_[100] = {};   // MySQL 密码
    char  sql_dbname_[100] = {};   // MySQL 数据库名
    MYSQL* mysql_       = nullptr; // 当前请求的 MySQL 连接（由线程池 RAII 获取）

    // 静态用户凭据缓存
    static std::map<std::string, std::string> users_;  // username → password
    static MutexLock users_lock_;                       // 保护 users_ 的并发访问
};

// ============================================================
// 工具函数
// ============================================================

// 设置文件描述符为非阻塞模式
// 非阻塞 IO 是 epoll ET 模式的前提，也是 Reactor 模式的基础
inline int set_nonblocking(int fd)
{
    int old_flags = fcntl(fd, F_GETFL);
    int new_flags = old_flags | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flags);
    return old_flags; //返回的是旧标志，方便恢复
}

// 向 epoll 注册文件描述符
// @param epollfd    epoll 实例
// @param fd         要注册的文件描述符
// @param one_shot   是否启用 EPOLLONESHOT
// @param trig_mode  触发模式：0=LT, 1=ET
inline void add_fd_to_epoll(int epollfd, int fd, bool one_shot, int trig_mode)
{
    epoll_event event;
    event.data.fd = fd;

    // EPOLLIN:  可读
    // EPOLLRDHUP: 对端关闭连接（Linux 2.6.17+）
    event.events = EPOLLIN | EPOLLRDHUP;

    if (trig_mode == 1) {
        event.events |= EPOLLET;     // 边缘触发
    }
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking(fd);
}

// 从 epoll 移除文件描述符并关闭
inline void remove_fd_from_epoll(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

// 修改 epoll 中已注册的事件（例如从 EPOLLIN 切换到 EPOLLOUT）
// @param ev       要监听的事件类型（EPOLLIN / EPOLLOUT）
// @param trig_mode 触发模式：0=LT, 1=ET
inline void modify_fd_in_epoll(int epollfd, int fd, int ev, int trig_mode)
{
    epoll_event event;
    event.data.fd = fd;

    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    if (trig_mode == 1) {
        event.events |= EPOLLET;
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

#endif  // XWEBSERVER_HTTP_CONNECTION_H