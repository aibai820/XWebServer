# My WebServer 架构设计文档

## 整体架构

```
┌─────────────────────────────────────────────────────┐
│                      main.cpp                        │
│                  (入口: 解析参数, 启动服务器)           │
└──────────────────────┬──────────────────────────────┘
                       │
              ┌────────▼────────┐
              │   WebServer     │
              │  (核心编排器)    │
              │                │
              │ ┌─ epoll 事件循环
              │ ├─ 连接管理
              │ ├─ 定时器管理
              │ ├─ 信号处理 (统一事件源)
              │ ├─ 日志系统
              │ └─ 数据库连接池  │
              └───┬─────┬──────┘
                  │     │
    ┌─────────────▼┐   ┌▼─────────────────┐
    │HttpConnection │   │  ThreadPool<T>    │
    │               │   │  (生产者-消费者)   │
    │ ├─HTTP解析    │   │                   │
    │ ├─状态机      │   │ ┌─工作线程 x N    │
    │ ├─CGI路由     │   │ ├─任务队列        │
    │ ├─静态文件    │   │ ├─信号量同步      │
    │ └─mmap/writev │   │ └─DB RAII 集成    │
    └───────────────┘   └───────────────────┘
```

## 数据流（完整 HTTP 请求处理）

```
浏览器请求
    │
    ▼
[epoll_wait] ──► EPOLLIN ──► accept/listenfd ──► 新连接
    │                                                  │
    │                                         [创建定时器 15s]
    │                                         [注册 epoll EPOLLIN]
    │
    ▼
[epoll_wait] ──► EPOLLIN ──► 客户端 socket
    │
    ├── Reactor:  主线程通知线程池 ──► 工作线程 read_once()
    └── Proactor: 主线程 read_once() ──► 通知线程池
                                              │
                                    ┌─────────▼─────────┐
                                    │  process_read()    │
                                    │  状态机解析 HTTP    │
                                    │  请求行 → 头 → 体  │
                                    └─────────┬─────────┘
                                              │
                                    ┌─────────▼─────────┐
                                    │  do_request()      │
                                    │  URL 路由          │
                                    │  ├─ 静态文件       │
                                    │  ├─ CGI 登录/注册  │
                                    │  └─ 短URL映射      │
                                    └─────────┬─────────┘
                                              │
                                    ┌─────────▼─────────┐
                                    │  process_write()   │
                                    │  组装 HTTP 响应    │
                                    │  mmap 映射文件     │
                                    │  writev 发送       │
                                    └─────────┬─────────┘
                                              │
[epoll_wait] ──► EPOLLOUT ──► 客户端 socket
    │
    └── write() ──► 续传数据 / 关闭连接 / keep-alive

统一事件源:
  SIGALRM → sig_handler → pipefd[1] → pipefd[0] → epoll → timer_handler()
  SIGTERM → sig_handler → pipefd[1] → pipefd[0] → epoll → stop_server
```

## 关键技术设计

### 1. 并发模型：Reactor vs Proactor

| 维度 | Reactor | Proactor |
|------|---------|----------|
| I/O 执行者 | 工作线程 | 主线程 |
| 主线程职责 | 事件分发 | 事件分发 + 全部 I/O |
| 工作线程职责 | I/O + 业务逻辑 | 纯业务逻辑 |
| 适用场景 | 通用 | I/O 操作快、业务逻辑重 |

### 2. epoll 触发模式

| trig_mode | 监听 socket | 连接 socket | 适用场景 |
|-----------|------------|-------------|----------|
| 0 | LT | LT | 开发调试、简单可靠 |
| 1 | LT | ET | **推荐生产使用** |
| 2 | ET | LT | — |
| 3 | ET | ET | 极致性能、代码复杂 |

### 3. 定时器设计

- 数据结构：有序双向链表（O(n) 插入，O(1) 删除）
- 超时策略：15 秒无活动自动断开（3 × TIMESLOT）
- 心跳机制：SIGALRM → socketpair → epoll → tick()
- 改进方向：时间轮（O(1) 插入和删除）

### 4. 统一事件源

- 问题：信号处理函数不能调用非异步信号安全的函数
- 方案：socketpair + epoll
  - 信号处理函数只写 1 字节到 pipefd[1]
  - epoll 检测 pipefd[0] 可读
  - 主循环同步处理，此时可调用任何函数

### 5. 日志系统

- 同步模式：直接 fputs + fflush
- 异步模式：阻塞队列 + 后台线程（减少主线程 I/O 阻塞）
- 日志滚动：按天 + 按行数（500 万行阈值）
- 格式：YYYY-MM-DD HH:MM:SS.mmmmmm [level]: message

### 6. 数据库连接池

- 单例模式（C++11 静态局部变量线程安全）
- 信号量控制并发获取：sem_wait/P → sem_post/V
- RAII 管理生命周期：connectionRAII 自动归还
- 内存缓存：启动时加载全部用户凭据到 map

### 7. 线程池优雅关闭

- m_stop 标志（std::atomic<bool>）
- stop() → post 信号量唤醒所有线程
- pthread_join 等待所有线程退出
- 相比 detach 方式，保证完整资源回收

## 性能基准（参考 TinyWebServer）

使用 WebBench 在以下平台测试：

| 连接模式 | QPS |
|----------|-----|
| LT + LT + Proactor | 97,459 |
| LT + ET + Proactor | 100,000+ |
| Jetson Xavier NX (ARM64) | TBD |
