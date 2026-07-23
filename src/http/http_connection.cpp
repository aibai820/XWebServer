/**
 * @file    http_connection.h
 * @brief   HTTP 连接处理类
 *
 * 阶段 1(已完成):简单的 echo——收到什么返回什么
 * 阶段 2(已完成）：实现完整的 HTTP/1.1 协议解析和静态文件服务
 * 阶段 3(已完成）：集成线程池，实现 Reactor/Proactor 模式
 *
 * 核心流程：
 *   浏览器请求 → read_once() → process() →
 *     process_read() [状态机: 请求行→头部→正文] →
 *       do_request() [stat + mmap 映射文件] →
 *     process_write() [构建 HTTP 响应头] →
 *     write() [writev 发送 响应头+文件内容] →
 *   EPOLLOUT 续传 或 EPOLLIN 等待下一个请求
 */

#include "http_connection.h"
#include "../pool/sql_connection_pool.h"
#include "../log/log.h"

// 静态成员变量定义（在 .cpp 中定义，.h 中声明）
int HttpConnection::epoll_fd_ = -1;
int HttpConnection::user_count_ = 0;

// 阶段 4: 用户凭据缓存（从 MySQL 加载）
std::map<std::string, std::string> HttpConnection::users_;
MutexLock HttpConnection::users_lock_;

// ============================================================
// HTTP 响应状态信息
// ============================================================
const char* ok_200_title = "OK";

const char* error_400_title = "Bad Request";
const char* error_400_form  =
    "<html><body><h1>400 Bad Request</h1>"
    "<p>Your request has bad syntax or is inherently impossible to satisfy.</p>"
    "</body></html>";

const char* error_403_title = "Forbidden";
const char* error_403_form  =
    "<html><body><h1>403 Forbidden</h1>"
    "<p>You do not have permission to access this resource.</p>"
    "</body></html>";

const char* error_404_title = "Not Found";
const char* error_404_form  =
    "<html><body><h1>404 Not Found</h1>"
    "<p>The requested file was not found on this server.</p>"
    "</body></html>";

const char* error_500_title = "Internal Server Error";
const char* error_500_form  =
    "<html><body><h1>500 Internal Server Error</h1>"
    "<p>There was an unusual problem serving the request.</p>"
    "</body></html>";

// ============================================================
// initmysql_result(): 从 MySQL 加载用户凭据到内存 map
//
// 服务器启动时调用一次。使用 RAII 从连接池获取连接，
// 执行 SELECT 查询，将 (username, password) 对存入 users_ map。
// 之后的登录验证只需查内存 map（O(log n)），无需访问数据库。
// 注册时先查 map 防重复，再 INSERT 到 MySQL 并同步更新 map。
//
// 安全注意：
//   - 生产环境中密码应存储哈希值（如 bcrypt/argon2），而非明文
//   - 这里为教学目的简化，使用明文存储
// ============================================================
#ifdef HAVE_MYSQL
void HttpConnection::initmysql_result(connection_pool* connPool) {
    // RAII 获取数据库连接（离开作用域自动归还）
    MYSQL* mysql = nullptr;
    connectionRAII mysqlcon(&mysql, connPool);

    if (mysql == nullptr) {
        LOG_ERROR("[CGI] 无法获取数据库连接，用户凭据加载失败");
        return;
    }

    // 查询 user 表中的所有用户名和密码
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("[CGI] SELECT 查询失败: %s", mysql_error(mysql));
        return;
    }

    // 存储查询结果
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr) {
        LOG_ERROR("[CGI] mysql_store_result 失败: %s", mysql_error(mysql));
        return;
    }

    // 遍历结果集，加载到 users_ map
    int num_rows = mysql_num_rows(result);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        if (row[0] && row[1]) {
            std::string username(row[0]);
            std::string passwd(row[1]);
            users_[username] = passwd;
        }
    }

    mysql_free_result(result);

    LOG_INFO("[CGI] 已加载 %d 个用户凭据到内存缓存", num_rows);
}
#else
// MySQL 不可用时的空实现
void HttpConnection::initmysql_result(connection_pool* connPool) {
    (void)connPool;
    LOG_WARN("[CGI] MySQL 客户端库不可用，用户凭据缓存为空");
}
#endif

// ============================================================
// MIME 类型映射
// 根据文件扩展名返回对应的 Content-Type
// ============================================================
const char* HttpConnection::get_mime_type(const char* url)
{
    // strrchr：从右往左找‘.’，即取最后一个后缀
    const char* dot = strrchr(url, '.');
    if (!dot)
    {
        // 无后缀 → 默认 text/plain（比 application/octet-stream 更安全）
        return "text/plain";
    }

    // 按字母顺序排列，二分查找友好
    if (strcasecmp(dot, ".css")   == 0) return "text/css";
    if (strcasecmp(dot, ".gif")   == 0) return "image/gif";
    if (strcasecmp(dot, ".htm")   == 0) return "text/html";
    if (strcasecmp(dot, ".html")  == 0) return "text/html";
    if (strcasecmp(dot, ".ico")   == 0) return "image/x-icon";
    if (strcasecmp(dot, ".jpeg")  == 0) return "image/jpeg";
    if (strcasecmp(dot, ".jpg")   == 0) return "image/jpeg";
    if (strcasecmp(dot, ".js")    == 0) return "text/javascript";
    if (strcasecmp(dot, ".json")  == 0) return "application/json";
    if (strcasecmp(dot, ".mp4")   == 0) return "video/mp4";
    if (strcasecmp(dot, ".pdf")   == 0) return "application/pdf";
    if (strcasecmp(dot, ".png")   == 0) return "image/png";
    if (strcasecmp(dot, ".svg")   == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".txt")   == 0) return "text/plain";
    if (strcasecmp(dot, ".xml")   == 0) return "text/xml";

    return "text/plain";
}


// ============================================================
// 公共接口：初始化连接
// ============================================================
void HttpConnection::init(int sockfd, const sockaddr_in& addr,
                          const char* doc_root, int trig_mode,
                          int close_log,
                          const char* sql_user,
                          const char* sql_passwd,
                          const char* sql_dbname)
{
    sockfd_   = sockfd;
    address_  = addr;
    doc_root_ = doc_root;
    trig_mode_ = trig_mode;

    // 阶段 4: 存储 MySQL 凭据（后续 CGI 处理时使用）
    if (sql_user && sql_user[0] != '\0') {
        strncpy(sql_user_, sql_user, sizeof(sql_user_) - 1);
        strncpy(sql_passwd_, sql_passwd, sizeof(sql_passwd_) - 1);
        strncpy(sql_dbname_, sql_dbname, sizeof(sql_dbname_) - 1);
    }

    // 注册到 epoll（one_shot 防止多线程竞争，Phase 3 用到）
    add_fd_to_epoll(epoll_fd_, sockfd_, true, trig_mode_);
    user_count_++;

    // 重置请求相关状态
    init_request();

    printf("[连接] 新客户端 fd=%d 来自 %s:%d  (当前在线: %d)\n",
           sockfd_,
           inet_ntoa(address_.sin_addr),
           ntohs(address_.sin_port),
           user_count_);
}

// ============================================================
// 内部辅助：重置单次请求状态
// 每次处理完一个请求后调用，准备处理下一个请求（keep-alive 场景）
// ============================================================
void HttpConnection::init_request()
{
    bytes_to_send_  = 0;
    bytes_have_send_ = 0;
    read_idx_       = 0;
    checked_idx_    = 0;
    start_line_     = 0;
    write_idx_      = 0;
    content_length_ = 0;
    url_            = nullptr;
    version_        = nullptr;
    host_           = nullptr;
    file_addr_      = nullptr;
    iv_count_       = 0;
    linger_         = false;
    method_         = GET;
    check_state_    = CHECK_STATE_REQUESTLINE;
    m_state         = 0;       // 重置任务类型
    improv          = 0;       // 重置同步标志
    timer_flag      = 0;       // 重置定时器标志
    cgi_            = 0;       // 重置 CGI 标志
    post_body_      = nullptr; // 重置 POST 正文指针
    mysql_          = nullptr; // 重置 MySQL 连接指针（由线程池 RAII 管理）

    memset(read_buf_,  '\0', READ_BUFFER_SIZE);
    memset(write_buf_, '\0', WRITE_BUFFER_SIZE);
    memset(real_file_, '\0', FILENAME_LEN);
    memset(&file_stat_, 0, sizeof(file_stat_));
    memset(iv_,         0, sizeof(iv_));
}


// ============================================================
// 关闭连接
// ============================================================
void HttpConnection::close_conn()
{
    if (sockfd_ != -1)
    {
        printf("[断开] fd=%d %s:%d (当前在线:%d)\n",
                sockfd_,
                inet_ntoa(address_.sin_addr),
                ntohs(address_.sin_port),
                user_count_ - 1);
        // 释放 mmap 映射（如果还有的话）
        unmap();

        remove_fd_from_epoll(epoll_fd_, sockfd_);
        sockfd_ = -1;
        user_count_--;
    }
}

// ============================================================
// 从 socket 读取数据
//
// 两种模式的对比（面试高频题）：
//   LT（水平触发）：只要缓冲区有数据就持续触发
//     → recv 一次即可，如果还有数据 epoll 会再次通知
//   ET（边缘触发）：只在状态变化时触发一次
//     → 必须循环 recv 直到返回 EAGAIN（缓冲区空了）
//     → 否则剩余数据不会被处理
// ============================================================
bool HttpConnection::read_once()
{
    if (read_idx_ >= READ_BUFFER_SIZE)
    {
        return false; // 缓冲区满，防止溢出
    }

    int bytes_read = 0;

    // ---- LT 模式：读一次就够 ----
    if (trig_mode_ == 0) {
        bytes_read = recv(sockfd_,
                          read_buf_ + read_idx_,
                          READ_BUFFER_SIZE - read_idx_,
                          0);
        if (bytes_read <= 0) {
            return false;  // 连接关闭或出错
        }
        read_idx_ += bytes_read;
        return true;
    }
    // ---- ET 模式：循环读到 EAGAIN ----
    else {
        while (true) {
            bytes_read = recv(sockfd_,
                              read_buf_ + read_idx_,
                              READ_BUFFER_SIZE - read_idx_,
                              0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 缓冲区已空，正常
                }
                return false;  // 真正的错误
            } else if (bytes_read == 0) {
                return false;  // 对端关闭
            }
            read_idx_ += bytes_read;
        }
        return true;
    }
}

// ============================================================
// 行解析（从状态机）
//
// 检查 read_buf_ 中是否有一个完整的行（以 \r\n 结尾）
// 找到后把 \r\n 替换为 \0\0，以便 C 字符串函数直接处理
//
// 返回值：
//   LINE_OK:  完整读入一行
//   LINE_BAD: 语法错误
//   LINE_OPEN: 不完整，等待更多数据
// ============================================================
HttpConnection::LINE_STATUS HttpConnection::parse_line()
{
    char temp;
    for (; checked_idx_ < read_idx_; ++checked_idx_)
    {
        temp = read_buf_[checked_idx_];

        // ---- 遇到 \r ----
        if (temp == '\r')
        {
            // \r 是最后一个字符，下一行还没读到，需要等待
            if (checked_idx_ + 1 == read_idx_) 
            {
                return LINE_OPEN;
            }
            // \r\n 连续出现 → 完整的一行 
            else if (read_buf_[checked_idx_ + 1] == '\n')
            {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            // \r 后面不是 \n → 协议错误
            return LINE_BAD;
        }
        // ---- 遇到 \n ----
        else if (temp == '\n')
        {
            // 上一个字符是 \r → 完整的一行
            if (checked_idx_ > 1 && read_buf_[checked_idx_ - 1] == '\r') {
                read_buf_[checked_idx_ - 1] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 还没找到行尾
    return LINE_OPEN;
}

// ============================================================
// 解析请求行
// 格式：METHOD URL HTTP-VERSION\r\n
// 例：  GET /index.html HTTP/1.1\r\n
//
// 只接受 GET 方法（POST 留给阶段 4 + CGI）
// ============================================================
HttpConnection::HTTP_CODE HttpConnection::parse_request_line(char* text)
{
    // 找 URL 的起始位置（方法名后的空白）
    url_ = strpbrk(text, " \t");
    if (!url_) {
        return BAD_REQUEST;
    }
    *url_++ = '\0';  // 把空格变成 \0，text 就是纯方法名

    // 解析方法
    char* method_str = text;
    if (strcasecmp(method_str, "GET") == 0) {
        method_ = GET;
    } else if (strcasecmp(method_str, "POST") == 0) {
        method_ = POST;
        cgi_    = 1;     // POST 请求启用 CGI 处理
    } else {
        // 不支持的方法（HEAD/PUT/DELETE 等）
        LOG_WARN("[HTTP] 不支持的方法: %s (fd=%d)", method_str, sockfd_);
        return BAD_REQUEST;
    }

    // 跳过 URL 前的空白
    url_ += strspn(url_, " \t");

    // 找版本号的起始位置（URL 后的空白）
    version_ = strpbrk(url_, " \t");
    if (!version_) {

        return BAD_REQUEST;
    }
    *version_++ = '\0';  // 把空格变成 \0，url_ 就是纯 URL

    // 跳过版本号前的空白
    version_ += strspn(version_, " \t");

    // 只接受 HTTP/1.1
    if (strcasecmp(version_, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 处理带 http:// 或 https:// 前缀的 URL（代理请求可能这样）
    if (strncasecmp(url_, "http://", 7) == 0) {
        url_ += 7;
        url_ = strchr(url_, '/');
    }
    if (strncasecmp(url_, "https://", 8) == 0) {
        url_ += 8;
        url_ = strchr(url_, '/');
    }

    // URL 必须以 / 开头
    if (!url_ || url_[0] != '/') {
        return BAD_REQUEST;
    }

    // 默认首页 "/" → 在 do_request() 中处理，先尝试 judge.html，
    // 不存在则回退到 index.html（阶段 4 引入 CGI 后可路由到导航页）

    // 进入下一个状态：解析头部
    check_state_ = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// ============================================================
// 解析 HTTP 头部
// 格式：Field-Name: Value\r\n
//
// 关注的头部：
//   Connection:     keep-alive → m_linger = true
//   Content-Length: 数字      → m_content_length
//   Host:           hostname  → m_host
//
// 遇到空行（只有 \0）→ 头部结束
// ============================================================
HttpConnection::HTTP_CODE HttpConnection::parse_headers(char* text)
{
    // 空行表示头部结束
    if (text[0] == '\0')
    {
        // 如果有Content-Length，还需要读正文
        if (content_length_ != 0)
        {
            check_state_ = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则请求完整
        return GET_REQUEST;
    }
    // Connection 头部
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            linger_ = true;
        }
    }
    // Content-Length 头部
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        content_length_ = atol(text);
    }
    // Host 头部
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        host_ = text;
    }
    // 其他头部忽略（阶段 4 会记录日志）
    else {
        // printf("[头部] 未知: %s\n", text);
    }

    return NO_REQUEST;
}

// ============================================================
// 解析 HTTP 正文（POST 请求的消息体）
//
// POST 请求中，正文通常包含表单数据，格式为:
//   user=myusername&password=mypassword
//
// 阶段 4 完整实现 CGI 处理：
//   收到完整正文后，保存 post_body_ 指针供 do_request() 解析。
//   正文被截断为一个 C 字符串（末尾添加 \0）。
// ============================================================
HttpConnection::HTTP_CODE HttpConnection::parse_content(char* text)
{
    // 判断正文是否完整接收
    // read_idx_ = 已读取的总字节数
    // checked_idx_ = 已解析的字节数（请求行 + 头部）
    // content_length_ = Content-Length 声明的正文长度
    if (read_idx_ >= (content_length_ + checked_idx_)) {
        text[content_length_] = '\0';   // 截断正文为 C 字符串
        post_body_ = text;              // 保存正文指针，供 do_request() 解析
        LOG_DEBUG("[HTTP] fd=%d POST body: %s", sockfd_, post_body_);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// ============================================================
// 主状态机：process_read()
//
// 驱动流程：
//   while (能解析出行) {
//       根据当前状态 → 调用对应解析函数
//       收到完整请求 → do_request()
//       协议错误     → BAD_REQUEST
//   }
//   return NO_REQUEST（数据不完整，等下次 epoll 通知）
// ============================================================
HttpConnection::HTTP_CODE HttpConnection::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE   ret         = NO_REQUEST;
    char*       text        = nullptr;

    // 循环条件说明：
    // - CONTENT 状态时不需要 parse_line()，直接处理正文
    // - 其他状态需要 parse_line() 先取出一行
    while ((check_state_ == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();

        start_line_ = checked_idx_;

        switch (check_state_)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN; // 正文还没读完
            break;
        }
        
        default:
            return INTERNAL_ERROR;
        }
    }
    
    return NO_REQUEST;
}

// ============================================================
// URL 路由 + 文件映射
//
// 阶段 2：纯静态文件路由
// 阶段 4 新增：CGI 登录/注册 + 短 URL 路由
//
// 路由表：
//   /0              → register.html     (新用户注册页面)
//   /1              → log.html           (已有账户登录页面)
//   /2CGISQL.cgi    → CGI 登录校验      (POST: user=xxx&password=yyy)
//   /3CGISQL.cgi    → CGI 注册处理      (POST: user=xxx&password=yyy)
//   /5              → picture.html       (图片页面)
//   /6              → video.html         (视频页面)
//   /7              → fans.html          (关注页面)
//   /               → judge.html 或 index.html
//   其他             → 作为静态文件路径
//
// CGI 处理逻辑：
//   - 登录 (2CGISQL.cgi): 解析 POST body → 在 users_ map 中验证 → 返回结果页
//   - 注册 (3CGISQL.cgi): 解析 POST body → 检查重复 → INSERT 到 MySQL + 更新 map
// ============================================================
HttpConnection::HTTP_CODE HttpConnection::do_request() {
    // 构建实际文件路径（首先填充 doc_root）
    strncpy(real_file_, doc_root_, FILENAME_LEN - 1);
    int root_len = strlen(doc_root_);

    // 获取 URL 最后一个路径组件（用于短 URL 路由检测）
    const char* last_slash = strrchr(url_, '/');
    char flag = (last_slash && *(last_slash + 1)) ? *(last_slash + 1) : '\0';

    // ============================================================
    // CGI 处理：登录校验 / 注册处理
    //
    // 只有 POST 请求才走 CGI 逻辑（GET 请求直接当静态文件处理）
    // URL 格式：
    //   /2CGISQL.cgi  → 登录（flag = '2'）
    //   /3CGISQL.cgi  → 注册（flag = '3'）
    // POST body 格式：user=USERNAME&password=PASSWORD
    // ============================================================
    if (cgi_ == 1 && post_body_ && last_slash && (flag == '2' || flag == '3')) {

        // ---- 解析 POST body: user=name&password=pass ----
        char name[100]     = {0};
        char password[100] = {0};

        // 提取用户名: "user=" 之后到 "&" 之前
        int i = 0;
        for (i = 5; post_body_[i] != '&' && post_body_[i] != '\0'; ++i) {
            name[i - 5] = post_body_[i];
        }
        name[i - 5] = '\0';

        // 提取密码: "&password=" 之后到结尾
        // "user=xxx&password=yyy" → password 从 &password= 之后开始
        // &password= 共 10 个字符
        if (post_body_[i] == '&') {
            int j = 0;
            for (i = i + 10; post_body_[i] != '\0'; ++i, ++j) {
                password[j] = post_body_[i];
            }
            password[j] = '\0';
        }

        LOG_INFO("[CGI] fd=%d %s: user='%s'",
                 sockfd_, (flag == '3') ? "注册" : "登录", name);

        // ---- 注册 (3CGISQL.cgi) ----
        if (flag == '3') {
            // 检查用户名是否已存在（在内存 map 中查）
            if (users_.find(name) == users_.end()) {
#ifdef HAVE_MYSQL
                // 用户名不重复 → 插入到 MySQL 数据库

                // RAII 获取数据库连接
                MYSQL* mysql = nullptr;
                connection_pool* pool = connection_pool::GetInstance();
                connectionRAII mysqlcon(&mysql, pool);

                if (mysql == nullptr) {
                    strncpy(real_file_ + root_len, "/registerError.html",
                            FILENAME_LEN - root_len - 1);
                } else {
                    // 构建 INSERT 语句
                    // 注意：实际生产环境应使用参数化查询（prepared statement）防 SQL 注入
                    char sql_insert[256];
                    snprintf(sql_insert, sizeof(sql_insert),
                             "INSERT INTO user(username, passwd) VALUES('%s','%s')",
                             name, password);

                    // 加锁保护 users_ map 的并发访问
                    users_lock_.lock();
                    int res = mysql_query(mysql, sql_insert);
                    users_lock_.unlock();

                    if (!res) {
                        // 插入成功 → 同步更新内存 map
                        users_lock_.lock();
                        users_.insert(std::make_pair(std::string(name),
                                                      std::string(password)));
                        users_lock_.unlock();

                        // 注册成功 → 跳转到登录页面
                        strncpy(real_file_ + root_len, "/log.html",
                                FILENAME_LEN - root_len - 1);
                        LOG_INFO("[CGI] fd=%d 注册成功: user='%s'", sockfd_, name);
                    } else {
                        // 插入失败
                        strncpy(real_file_ + root_len, "/registerError.html",
                                FILENAME_LEN - root_len - 1);
                        LOG_ERROR("[CGI] fd=%d 注册失败(MySQL): user='%s' error='%s'",
                                  sockfd_, name, mysql_error(mysql));
                    }
                }
#else
                // MySQL 不可用 → 注册功能禁用
                strncpy(real_file_ + root_len, "/registerError.html",
                        FILENAME_LEN - root_len - 1);
                LOG_WARN("[CGI] fd=%d 注册失败(MySQL不可用): user='%s'", sockfd_, name);
#endif
            } else {
                // 用户名已存在
                strncpy(real_file_ + root_len, "/registerError.html",
                        FILENAME_LEN - root_len - 1);
                LOG_WARN("[CGI] fd=%d 注册失败(重复): user='%s'", sockfd_, name);
            }
        }
        // ---- 登录 (2CGISQL.cgi) ----
        else if (flag == '2') {
            // 在内存 map 中验证用户名和密码
            auto it = users_.find(name);
            if (it != users_.end() && it->second == password) {
                // 登录成功 → 跳转到欢迎页面
                strncpy(real_file_ + root_len, "/welcome.html",
                        FILENAME_LEN - root_len - 1);
                LOG_INFO("[CGI] fd=%d 登录成功: user='%s'", sockfd_, name);
            } else {
                // 用户名不存在或密码不匹配
                strncpy(real_file_ + root_len, "/logError.html",
                        FILENAME_LEN - root_len - 1);
                LOG_WARN("[CGI] fd=%d 登录失败: user='%s'", sockfd_, name);
            }
        }

        real_file_[FILENAME_LEN - 1] = '\0';
    }
    // ============================================================
    // 默认首页：/ → judge.html（如果存在），否则 index.html
    //
    // 注意：这个判断必须在 "else if (last_slash)" 之前！
    // 因为 strrchr("/", '/') 返回非空指针，如果先判断 last_slash，
    // "/" 会被 switch 的 default 分支当静态路径处理，永远到不了这里。
    //
    // judge.html 是 CGI 功能的入口页面，提供"新用户"和"已有账户"按钮。
    // 如果 judge.html 不存在（例如未配置数据库的部署），
    // 回退到 index.html 基础页面。
    // ============================================================
    else if (strlen(url_) == 1 && url_[0] == '/') {
        // 先尝试 judge.html
        strncpy(real_file_ + root_len, "/judge.html",
                FILENAME_LEN - root_len - 1);

        // 如果 judge.html 不存在，回退到 index.html
        struct stat judge_stat;
        if (stat(real_file_, &judge_stat) < 0) {
            strncpy(real_file_ + root_len, "/index.html",
                    FILENAME_LEN - root_len - 1);
        }
        real_file_[FILENAME_LEN - 1] = '\0';
    }
    // ============================================================
    // 短 URL 路由（非 CGI POST，或 GET 请求）
    //
    // 这是 TinyWebServer 的一个特殊设计：URL 路径为单字符时
    // 用作"动作标识符"（action flag），映射到不同的 HTML 页面。
    // 这些标识符来自 judge.html 和 welcome.html 中的表单 action。
    //
    //   0 → register.html      (新用户注册)
    //   1 → log.html            (已有账户登录)
    //   5 → picture.html        (图片页面)
    //   6 → video.html          (视频页面)
    //   7 → fans.html           (关注页面)
    // ============================================================
    else if (last_slash) {
        switch (flag) {
        case '0':   // 新用户 → 注册页面
            strncpy(real_file_ + root_len, "/register.html",
                    FILENAME_LEN - root_len - 1);
            break;
        case '1':   // 已有账户 → 登录页面
            strncpy(real_file_ + root_len, "/log.html",
                    FILENAME_LEN - root_len - 1);
            break;
        case '5':   // 图片
            strncpy(real_file_ + root_len, "/picture.html",
                    FILENAME_LEN - root_len - 1);
            break;
        case '6':   // 视频
            strncpy(real_file_ + root_len, "/video.html",
                    FILENAME_LEN - root_len - 1);
            break;
        case '7':   // 关注
            strncpy(real_file_ + root_len, "/fans.html",
                    FILENAME_LEN - root_len - 1);
            break;
        default:
            // 非路由 URL，作为普通静态文件路径处理
            strncpy(real_file_ + root_len, url_,
                    FILENAME_LEN - root_len - 1);
            break;
        }
        real_file_[FILENAME_LEN - 1] = '\0';
    }
    // ============================================================
    // 其他：作为普通静态文件路径
    // ============================================================
    else {
        strncpy(real_file_ + root_len, url_,
                FILENAME_LEN - root_len - 1);
        real_file_[FILENAME_LEN - 1] = '\0';
    }

    LOG_DEBUG("[请求] fd=%d  URL: %s → 文件: %s", sockfd_, url_, real_file_);

    // ---- stat() 获取文件信息 ----
    if (stat(real_file_, &file_stat_) < 0) {
        return NO_RESOURCE;  // 文件不存在 → 404
    }

    // ---- 检查是否有读取权限（other 可读） ----
    if (!(file_stat_.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;  // 无权限 → 403
    }

    // ---- 目录不允许直接访问 ----
    if (S_ISDIR(file_stat_.st_mode)) {
        return BAD_REQUEST;  // 是目录 → 400
    }

    // ---- mmap 零拷贝：把文件映射到内存 ----
    // MAP_PRIVATE: 写时复制（虽然这里是只读访问）
    int fd = open(real_file_, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("[HTTP] fd=%d 无法打开文件: %s (%s)", sockfd_, real_file_, strerror(errno));
        return INTERNAL_ERROR;
    }
    file_addr_ = (char*)mmap(nullptr, file_stat_.st_size,
                             PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  // mmap 后可以立即关闭文件描述符（映射仍然有效）

    if (file_addr_ == MAP_FAILED) {
        file_addr_ = nullptr;
        LOG_ERROR("[HTTP] fd=%d mmap 失败: %s", sockfd_, strerror(errno));
        return INTERNAL_ERROR;
    }

    return FILE_REQUEST;
}

// ============================================================
// 释放 mmap 映射
// ============================================================
void HttpConnection::unmap()
{
    if(file_addr_)
    {
        munmap(file_addr_, file_stat_.st_size);
        file_addr_ = nullptr;
    }
}

// ============================================================
// 构建 HTTP 响应
//
// 根据 process_read() 的返回值，组装 HTTP 响应：
//   200: 状态行 + Content-Type + Content-Length + Connection + 空行 + 文件内容
//   400/403/404/500: 状态行 + Content-Type + Content-Length + 空行 + 错误 HTML
//
// 对于 200 响应，使用 writev 将 响应头和文件内容 一起发送：
//   iv_[0] → write_buf_ (响应头)
//   iv_[1] → file_addr_ (mmap 的文件内容)
// ============================================================
bool HttpConnection::process_write(HTTP_CODE ret)
{
    switch (ret)
    // ---- 500 Internal Server Error ----
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    // ---- 400 Bad Request ----
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    }
    // ---- 404 Not Found ----
    case  NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    // ---- 403 Forbidden ----
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    // ---- 200 OK (文件请求) ----
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (file_stat_.st_size != 0)
        {
            // 文件非空：响应头 + 文件内容
            add_content_type();
            add_headers(file_stat_.st_size);

            // 设置 writev 的 iovec
            iv_[0].iov_base = write_buf_;
            iv_[0].iov_len = write_idx_;
            iv_[1].iov_base = file_addr_;
            iv_[1].iov_len = file_stat_.st_size;
            iv_count_ = 2;
            bytes_to_send_ = write_idx_ + file_stat_.st_size;
            return true;
        }else{
            // 空文件：只返回基本的HTML页面
            const char* ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }

    // 错误响应和空文件响应只有响应头（没有 mmap 文件）
    iv_[0].iov_base = write_buf_;
    iv_[0].iov_len = write_idx_;
    iv_count_ = 1;
    bytes_to_send_ = write_idx_;
    return true;
}

// ============================================================
// process(): HTTP 请求处理入口
//
// 1. 调用 process_read() 解析请求
// 2. 如果请求不完整 → 重新注册 EPOLLIN，等待更多数据
// 3. 调用 process_write() 构建响应
// 4. 重新注册 EPOLLOUT，由 write() 实际发送
// ============================================================
void HttpConnection::process()
{
    HTTP_CODE read_ret = process_read();

    // 请求不完整，继续监听读事件
    if (read_ret == NO_REQUEST) {
        modify_fd_in_epoll(epoll_fd_, sockfd_, EPOLLIN, trig_mode_);
        return;
    }
  
    // 构建 HTTP 响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        // 响应构建失败，关闭连接
        close_conn();
        return;
    }
    
    // 注册写事件，由 write() 函数实际发送数据
    modify_fd_in_epoll(epoll_fd_, sockfd_, EPOLLOUT, trig_mode_);
    
}

// ============================================================
// write(): 非阻塞写
//
// 使用 writev() 一次系统调用发送 响应头 + 文件内容
// 如果发送缓冲区满了（EAGAIN），重新注册 EPOLLOUT 等待下次可写
// 如果发送完成且 keep-alive → 重置状态，准备处理下一个请求
// 如果发送完成但 close   → 返回 false，由调用方关闭连接
//
// writev 的 iovec 调整逻辑：
//   bytes_have_send 累加已发送字节
//   当发送量超过 iv_[0].iov_len（响应头发完了）→ 调整 iv_[1] 继续发文件
//   当发送量还不到 iv_[0].iov_len → 调整 iv_[0] 继续发响应头
// ============================================================
bool HttpConnection::write()
{
    int temp = 0;

    // 没有待发送数据（不应出现的情况）
    if (bytes_to_send_ == 0)
    {
        modify_fd_in_epoll(epoll_fd_, sockfd_, EPOLLIN, trig_mode_);
        init_request();
        return true;
    }

    // 循环发送，直到缓冲区满或全部发完
    while (true)
    {
        temp = writev(sockfd_, iv_, iv_count_);

        if (temp < 0)
        {
            // 发送缓冲区满了，等待EPOLLOUT通知
            if (errno == EAGAIN)
            {
                modify_fd_in_epoll(epoll_fd_, sockfd_, EPOLLOUT, trig_mode_);
                 return true;
            }
            // 真正的错误
            unmap();
            return false;
        }

        bytes_have_send_ += temp;
        bytes_to_send_ -= temp;

        // 调整 iovec偏移
        if (bytes_have_send_ >= static_cast<int>(iv_[0].iov_len))
        {
            // 响应头已经发完，正在发文件内容
            iv_[0].iov_len = 0;
            iv_[1].iov_base = file_addr_ + (bytes_have_send_ - write_idx_);
            iv_[1].iov_len = bytes_to_send_;
        }else{
            // 响应头还没发完
            iv_[0].iov_base = write_buf_ + bytes_have_send_;
            iv_[0].iov_len = iv_[0].iov_len - temp;
        }

        // 全部发送完毕
        if (bytes_to_send_ <= 0)
        {
            unmap();

            // 重新监听读事件
            modify_fd_in_epoll(epoll_fd_, sockfd_, EPOLLIN, trig_mode_);

            // keep-alive:重置状态，等待下一个请求
            if (linger_)
            {
                init_request();
                return true;
            }else{
                // close:通知调用方法关闭连接
                return false;
            }
        }
    }  
}

// ============================================================
// 响应构建辅助函数
// ============================================================

// 向写缓冲区追加格式化字符串
bool HttpConnection::add_response(const char* format, ...)
{
    if (write_idx_ >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(write_buf_ + write_idx_,
                        WRITE_BUFFER_SIZE - 1 - write_idx_,
                        format, arg_list);
    va_end(arg_list);

    if (len >= (WRITE_BUFFER_SIZE - 1 -write_idx_))
    {
        return false; // 写缓冲区溢出
    }
    write_idx_ += len;
    return true;
}

// 状态行：HTTP/1.1 200 OK\r\n
bool HttpConnection::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 响应头部：Content-Length + Connection + 空行
bool HttpConnection::add_headers(int content_length) {
    return add_content_length(content_length)
        && add_linger()
        && add_blank_line();
}

// Content-Type 头部
bool HttpConnection::add_content_type() {
    return add_response("Content-Type:%s\r\n", get_mime_type(real_file_));
}

// Content-Length 头部
bool HttpConnection::add_content_length(int content_length) {
    return add_response("Content-Length:%d\r\n", content_length);
}

// Connection 头部（keep-alive 或 close）
bool HttpConnection::add_linger() {
    return add_response("Connection:%s\r\n",
                        linger_ ? "keep-alive" : "close");
}

// 空行（头部与正文的分隔）
bool HttpConnection::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 正文内容
bool HttpConnection::add_content(const char* content) {
    return add_response("%s", content);
}
