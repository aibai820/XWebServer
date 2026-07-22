# XWebServer

> 从零开始构建的高并发 C++ Web 服务器 —— 基于 epoll + 线程池 + 非阻塞 IO
>
> 部署平台：NVIDIA Jetson Xavier NX (ARM64, Ubuntu 20.04 LTS)

---

## 项目概述

这是一个教学项目，通过复刻 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer)，深入学习 Linux 服务端开发的核心技术。

### 功能特性（按阶段演进）

| 阶段 | 功能 | 核心技术 |
|------|------|----------|
| ✅ **阶段 1** | epoll echo 服务器 | socket / bind / listen / epoll / 非阻塞 IO |
| ✅ **阶段 2** | HTTP 静态文件服务器 | HTTP 协议解析、状态机、mmap 零拷贝、writev |
| ✅ **阶段 3** | 并发服务器 | 线程池、Reactor/Proactor 模式、信号量同步 |
| ⬜ **阶段 4** | 生产级服务器 | 定时器、异步日志、数据库连接池、压力测试 |

---

## 快速开始

### 环境要求
- **硬件**: NVIDIA Jetson Xavier NX (或任何 ARM64 / x86_64 Linux)
- **系统**: Ubuntu 20.04 LTS（JetPack 5 默认）
- **工具**: g++ 7+, cmake 3.10+, make

```bash
# 1. 克隆项目
git clone <your-repo-url>
cd my-webserver

# 2. 构建
chmod +x build.sh
./build.sh

# 3. 运行
./build/webserver

# 4. 测试（另开终端）
curl http://localhost:9006
# 或者浏览器访问 http://<jetson-ip>:9006
```

### 编译选项

```bash
./build.sh          # Debug 构建（默认，带调试符号）
./build.sh release  # Release 构建（-O2 优化）
./build.sh clean    # 清理构建产物

# 手动编译
cd build && cmake .. && make -j$(nproc)
```

---

## 项目结构

```
my-webserver/
├── CMakeLists.txt               # CMake 构建配置
├── build.sh                     # 便捷构建脚本
├── README.md
├── docs/                        # 设计文档
│   └── architecture.md
├── config/                      # 配置文件（阶段 4）
├── static/                      # Web 静态资源（阶段 2）
├── test/                        # 测试代码（阶段 5）
└── src/
    ├── main.cpp                 # 程序入口
    ├── server.h / .cpp          # WebServer 核心类
    ├── http/
    │   └── http_connection.h/cpp # HTTP 连接处理器
    ├── pool/                    # 线程池 & 连接池（阶段 3-4）
    ├── timer/                   # 定时器模块（阶段 4）
    ├── log/                     # 日志系统（阶段 4）
    └── sync/
        └── locker.h             # 线程同步原语 RAII 封装
```

---

## 学习路线

从头开始构建一个高性能 Web 服务器，建议按以下顺序：

### 阶段 1：epoll echo 服务器（当前）

**目标**：理解 Linux 网络编程和 I/O 多路复用的基础。

**核心概念**：
- `socket()` → `bind()` → `listen()` → `accept()` 标准流程
- `epoll_create()` → `epoll_ctl()` → `epoll_wait()` 事件循环
- 阻塞 IO vs 非阻塞 IO
- LT（水平触发）vs ET（边缘触发）

**自测**：
- [ ] 能解释 epoll 为什么比 select/poll 快？
- [ ] LT 和 ET 的区别是什么？什么场景用哪个？
- [ ] `recv()` 返回 0 意味着什么？
- [ ] 文件描述符为什么需要设为非阻塞？

### 阶段 2：HTTP 静态文件服务器

**核心概念**：
- HTTP/1.1 协议格式（请求行 / 头部 / 正文）
- 状态机模式
- `mmap()` 零拷贝原理
- `writev()` 分散/聚集 I/O
- HTTP 状态码（200 / 400 / 403 / 404 / 500）

**自测**：
- [ ] HTTP GET 和 POST 的区别？
- [ ] `mmap` 为什么比 `read` + `write` 快？
- [ ] 如何处理大文件的分块传输？

### 阶段 3：并发服务器

**核心概念**：
- 线程池设计（生产者-消费者模型）
- Reactor vs Proactor 并发模式
- `EPOLLONESHOT` 的作用
- 信号量 vs 条件变量

**自测**：
- [ ] Reactor 和 Proactor 的本质区别？
- [ ] 线程数量和 CPU 核心数怎么取舍？
- [ ] `EPOLLONESHOT` 防止什么问题？

### 阶段 4：生产级特性

**核心概念**：
- 定时器管理超时连接（统一事件源）
- 异步日志（阻塞队列 + 条件变量）
- 数据库连接池（RAII + 单例）
- 信号处理（`socketpair` 桥接）

**自测**：
- [ ] 为什么信号处理函数里不能做复杂操作？
- [ ] 统一事件源解决了什么问题？
- [ ] 日志系统异步写 vs 同步写的取舍？

---

## Jetson Xavier NX 注意事项

### 平台特性
- **架构**: ARM64 (aarch64)
- **核心数**: 6 核 Carmel (ARM v8.2)
- **内存**: 8GB LPDDR4x
- **系统**: Ubuntu 18.04 (JetPack 4.x) 或 20.04 (JetPack 5.x)

### 编译确认
```bash
# 确认架构
uname -m            # 应输出 aarch64

# 确认编译器
g++ --version       # 应 7.x 或更高

# 确认 cmake
cmake --version     # 应 3.10+

# 查看 CPU 核心数（设置线程池大小时有用）
nproc               # 通常是 6
```

### 性能调优建议
```bash
# 增大 socket 缓冲区（可选）
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608

# 查看当前限制
ulimit -n           # 文件描述符上限（默认 1024 需要调大）

# 临时调大（当前 shell 生效）
ulimit -n 65535
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
