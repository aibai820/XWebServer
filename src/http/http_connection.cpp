/**
 * @file    http_connection.h
 * @brief   HTTP 连接处理类
 *
 * 阶段 1:简单的 echo——收到什么返回什么
 * 阶段 2（当前）：实现完整的 HTTP/1.1 协议解析和静态文件服务
 * 阶段 3：集成线程池，实现 Reactor/Proactor 模式
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

// 静态成员变量定义（在 .cpp 中定义，.h 中声明）
int HttpConnection::epoll_fd_ = -1;
int HttpConnection::user_count_ = 0;

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
                          const char* doc_root, int trig_mode) 
{
    sockfd_   = sockfd;
    address_  = addr;
    doc_root_ = doc_root;
    trig_mode_ = trig_mode;

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
    if (!url_)
    {
        return BAD_REQUEST;
    }
    *url_++ = '\0';  // 把空格变成 \0，text 就是纯方法名 

    // 解析方法
    char* method_str = text;
    if (strcasecmp(method_str, "GET") == 0) {
        method_ = GET;
    } else if (strcasecmp(method_str, "POST") == 0) {
        method_ = POST;
    } else {
        // 阶段 2 只支持 GET
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

    // 默认访问 index.html
    if (strlen(url_) == 1 && url_[0] == '/') {
        // url_ 指向 read_buf_，不能直接赋值字符串
        // 用 strcat 追加在 / 后面（注意：strcat 需要目标内存有 \0）
        // 这里 "/" 只占 1 字节 + \0，后面紧跟 parse 时的 \0
        // 实际上需要确保缓冲区足够大。Phase 2 简单处理：
        // 重新指定 url_ 到一个默认页面
        static char default_url[] = "/index.html";
        // 我们不能修改 url_ 指针指向静态内存，因为后续 parse_headers
        // 会用到 read_buf_ 中的内容，这里留作练习。
        // 简化方案：在 do_request() 中处理 "/" → "/index.html" 的转换
    }
    
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
        
    }
}