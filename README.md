# My WebServer

> 从零开始构建的高并发 C++ Web 服务器 —— 基于 epoll + 线程池 + 非阻塞 IO
>
> 部署平台：NVIDIA Jetson Xavier NX (ARM64, Ubuntu 18.04/20.04)

---

## 项目概述

这是一个教学项目，通过复刻 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer)，深入学习 Linux 服务端开发的核心技术。

### 功能特性（按阶段演进）

| 阶段 | 功能 | 核心技术 |
|------|------|----------|
| ✅ **阶段 1** | epoll echo 服务器 | socket / bind / listen / epoll / 非阻塞 IO |
| ✅ **阶段 2** | HTTP 静态文件服务器 | HTTP 协议解析、状态机、mmap 零拷贝、writev |
| ✅ **阶段 3** | 并发服务器 | 线程池、Reactor/Proactor 模式、信号量同步 |
| ✅ **阶段 4** | 生产级服务器 | 定时器、异步日志、数据库连接池、CGI、压力测试 |

---

## 快速开始

### 环境要求
- **硬件**: NVIDIA Jetson Xavier NX (或任何 ARM64 / x86_64 Linux)
- **系统**: Ubuntu 18.04+ (JetPack 4.x+)
- **工具**: g++ 7+, cmake 3.10+, make
- **可选**: MySQL 5.7+（CGI 登录/注册功能需要）

```bash
# 1. 克隆项目
git clone <your-repo-url>
cd my-webserver

# 2. 构建
chmod +x build.sh
./build.sh

# 3. 运行（纯静态文件服务，无需 MySQL）
./build/webserver

# 4. 测试（另开终端）
curl http://localhost:9006
# 或者浏览器访问 http://<jetson-ip>:9006
```

### 启用 CGI 登录/注册（需要 MySQL）

```bash
# 1. 安装 MySQL 开发库
sudo apt install libmysqlclient-dev mysql-server

# 2. 创建数据库和用户表
sudo mysql -u root -p
CREATE DATABASE webserver;
USE webserver;
CREATE TABLE user(
    username char(50) NULL,
    passwd   char(50) NULL
) ENGINE=InnoDB;

# 3. 修改 src/main.cpp 中的 MySQL 凭据
#    MYSQL_USER     "root"
#    MYSQL_PASSWD   "你的密码"
#    MYSQL_DBNAME   "webserver"

# 4. 重新构建并运行
./build.sh
./build/webserver
# 浏览器访问 http://localhost:9006 → 即可看到登录/注册界面
```

### 编译选项

```bash
./build.sh          # Debug 构建（默认，带调试符号）
./build.sh release  # Release 构建（-O2 优化）
./build.sh clean    # 清理构建产物

# 手动编译
cd build && cmake .. && make -j$(nproc)
```

### 运行参数

```bash
./build/webserver [端口] [根目录] [触发模式] [线程数] [并发模式]

# 示例：
./build/webserver                                           # 全部默认
./build/webserver 8080                                      # 指定端口
./build/webserver 8080 ./static et 8 reactor                # 全参数
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| 端口 | 监听端口 | 9006 |
| 根目录 | 静态文件目录 | ./static |
| 触发模式 | 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET | 0 (LT+LT) |
| 线程数 | 线程池大小 | 8 |
| 并发模式 | proactor 或 reactor | proactor |

---

## 项目结构

```
my-webserver/
├── CMakeLists.txt               # CMake 构建配置
├── build.sh                     # 便捷构建脚本
├── README.md
├── docs/                        # 设计文档
│   └── architecture.md
├── config/                      # 配置文件（预留）
├── static/                      # Web 静态资源
│   ├── index.html               # 首页
│   ├── judge.html               # 入口导航（登录/注册）
│   ├── log.html                 # 登录页面
│   ├── register.html            # 注册页面
│   ├── welcome.html             # 登录后欢迎页面
│   ├── logError.html            # 登录失败页面
│   ├── registerError.html       # 注册失败页面
│   ├── picture.html             # 图片展示页面
│   ├── video.html               # 视频播放页面
│   ├── fans.html                # 关注页面
│   └── test.css                 # 样式文件
├── test/                        # 测试代码（预留）
└── src/
    ├── main.cpp                 # 程序入口（参数解析 + 初始化链）
    ├── server.h / .cpp          # WebServer 核心类（epoll + 定时器 + 信号）
    ├── http/
    │   └── http_connection.h/cpp # HTTP 连接处理（状态机 + CGI + 静态文件）
    ├── pool/
    │   ├── threadpool.h          # 线程池（生产者-消费者 + RAII DB 连接）
    │   └── sql_connection_pool.h/cpp  # 数据库连接池（单例 + 信号量）
    ├── timer/
    │   └── timer.h / .cpp       # 定时器模块（有序链表 + 统一事件源）
    ├── log/
    │   ├── log.h / .cpp         # 日志系统（单例 + 同步/异步 + 滚动）
    │   └── block_queue.h        # 阻塞队列（循环数组 + 条件变量）
    └── sync/
        └── locker.h             # 线程同步原语 RAII 封装
```

---

## 架构概述

```
main.cpp (入口)
    │
    ├── server.init()        — 保存配置
    ├── server.trig_mode()   — 解析触发模式
    ├── server.log_write()   — 初始化日志
    ├── server.sql_pool()    — 初始化 DB 连接池 + 加载用户凭据
    ├── server.thread_pool() — 创建线程池
    ├── server.event_listen() — socket/bind/listen/epoll/socketpair/信号/alarm
    └── server.event_loop()  — 主事件循环

事件循环 (event_loop):
    epoll_wait()
        ├── listenfd EPOLLIN      → dealclientdata()  → accept + timer()
        ├── pipefd[0] EPOLLIN     → dealwithsignal()  → 信号处理
        ├── clientfd EPOLLIN      → dealwithread()    → 读 → 线程池
        ├── clientfd EPOLLOUT     → dealwithwrite()   → 写 → 线程池
        └── clientfd EPOLLRDHUP   → deal_timer()      → 关闭连接
    timeout → utils_.timer_handler() → tick() + alarm()

线程池 (ThreadPool):
    Reactor:  工作线程 read_once() → [connectionRAII] → process()
    Proactor: 主线程 read_once() → 工作线程 [connectionRAII] → process()

HTTP 处理 (HttpConnection):
    process_read() [状态机: 请求行→头部→正文]
        → do_request() [URL路由: 静态文件 / CGI登录注册]
    process_write() [构建 HTTP 响应]
    write() [writev 发送]
```

---

## 核心设计决策

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

### 4. 日志系统

- 同步模式：直接 fputs + fflush
- 异步模式：阻塞队列 + 后台线程
- 日志滚动：按天 + 按行数（500 万行阈值）

### 5. 数据库连接池

- 单例模式 + 信号量控制并发数
- RAII 保证连接正确归还
- 用户凭据内存缓存（避免每次查库）

---

## 学习路线

### 阶段 1：epoll echo 服务器

**自测**：
- [ ] 能解释 epoll 为什么比 select/poll 快？
- [ ] LT 和 ET 的区别是什么？什么场景用哪个？
- [ ] `recv()` 返回 0 意味着什么？

### 阶段 2：HTTP 静态文件服务器

**自测**：
- [ ] HTTP GET 和 POST 的区别？
- [ ] `mmap` 为什么比 `read` + `write` 快？
- [ ] 如何处理大文件的分块传输？

### 阶段 3：并发服务器

**自测**：
- [ ] Reactor 和 Proactor 的本质区别？
- [ ] 线程数量和 CPU 核心数怎么取舍？
- [ ] `EPOLLONESHOT` 防止什么问题？

### 阶段 4：生产级特性

**自测**：
- [ ] 为什么信号处理函数里不能做复杂操作？统一事件源如何解决？
- [ ] 异步日志和同步日志的取舍？
- [ ] 连接池的 RAII 管理是如何防止连接泄漏的？
- [ ] 定时器有序链表的时间复杂度？时间轮如何改进？

---

## Jetson Xavier NX 注意事项

### 平台特性
- **架构**: ARM64 (aarch64)
- **核心数**: 6 核 Carmel (ARM v8.2)
- **内存**: 8GB LPDDR4x

### 编译确认
```bash
uname -m            # 应输出 aarch64
g++ --version       # 应 7.x 或更高
cmake --version     # 应 3.10+
nproc               # 通常是 6
```

### 性能调优建议
```bash
# 增大文件描述符上限
ulimit -n 65535

# 增大 socket 缓冲区
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608
```

---

## 压力测试

```bash
# 使用 wrk（需要安装）
wrk -t4 -c100 -d30s http://localhost:9006/

# 使用 Apache Bench
ab -n 100000 -c 1000 http://localhost:9006/

# 使用 WebBench（test_pressure/ 目录中）
./test_pressure/webbench-1.5/webbench -c 1000 -t 30 http://localhost:9006/
```

---

## 参考资源

- 原项目 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer)
- 《Linux 高性能服务器编程》—— 游双
- 《Unix 网络编程》—— W. Richard Stevens
- [Linux epoll 手册](https://man7.org/linux/man-pages/man7/epoll.7.html)

---

## License

Apache License 2.0
