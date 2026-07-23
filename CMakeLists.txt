cmake_minimum_required(VERSION 3.10)
project(my-webserver VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
# C++ 标准：使用 C++17（Jetson 的 gcc 8.x 完整支持）
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 编译选项
# ============================================================
# 生成 compile_commands.json (给 clangd/LSP 用)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 平台检测：Jetson 是 ARM64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    message(STATUS "检测到 ARM64 平台 (Jetson)")
    add_definitions(-DPLATFORM_ARM64)
endif()

# 编译标志
set(COMMON_FLAGS
    -Wall -Wextra -Wpedantic          # 严格警告
    -Wno-unused-parameter              # 未用参数不警告
    -g                                 # 调试符号
)
# Release 模式下的优化标志
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
# Debug 模式（默认）
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG")

add_compile_options(${COMMON_FLAGS})

# ============================================================
# 依赖库
# ============================================================
# pthread：POSIX 线程（Linux 上通常需要显式链接）
find_package(Threads REQUIRED)

# MySQL 客户端库（阶段 4 启用）
# 使用 PkgConfig 查找 mysqlclient
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MYSQL mysqlclient)
endif()

if(MYSQL_FOUND)
    message(STATUS "找到 MySQL 客户端库: ${MYSQL_LIBRARIES}")
    message(STATUS "  MySQL 包含目录: ${MYSQL_INCLUDE_DIRS}")
    add_definitions(-DHAVE_MYSQL)
else()
    message(WARNING "=============================================")
    message(WARNING "  未找到 MySQL 客户端库 (mysqlclient)")
    message(WARNING "  CGI 登录/注册功能将不可用")
    message(WARNING "  服务器仍可正常提供静态文件服务")
    message(WARNING "  安装 MySQL 开发库:")
    message(WARNING "    Ubuntu: sudo apt install libmysqlclient-dev")
    message(WARNING "    Jetson: sudo apt install libmysqlclient-dev")
    message(WARNING "=============================================")
endif()

# ============================================================
# 源文件（阶段 4：新增 timer/log/sql_connection_pool）
# ============================================================
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/http/http_connection.cpp
    src/timer/timer.cpp                  # 阶段 4 新增：定时器模块
    src/log/log.cpp                      # 阶段 4 新增：日志系统
    src/pool/sql_connection_pool.cpp     # 阶段 4 新增：数据库连接池
)

set(HEADERS
    src/server.h
    src/http/http_connection.h
    src/sync/locker.h
    src/pool/threadpool.h
    src/pool/sql_connection_pool.h       # 阶段 4 新增
    src/timer/timer.h                    # 阶段 4 新增
    src/log/log.h                        # 阶段 4 新增
    src/log/block_queue.h               # 阶段 4 新增
)

# ============================================================
# 可执行目标
# ============================================================
add_executable(webserver ${SOURCES} ${HEADERS})

# 包含目录
target_include_directories(webserver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/http
    ${CMAKE_SOURCE_DIR}/src/pool
    ${CMAKE_SOURCE_DIR}/src/timer       # 阶段 4 新增
    ${CMAKE_SOURCE_DIR}/src/log         # 阶段 4 新增
    ${CMAKE_SOURCE_DIR}/src/sync
)

# 链接库
target_link_libraries(webserver PRIVATE
    Threads::Threads
)

# MySQL 库（可选：如果找到则链接）
if(MYSQL_FOUND)
    target_include_directories(webserver PRIVATE ${MYSQL_INCLUDE_DIRS})
    target_link_libraries(webserver PRIVATE ${MYSQL_LIBRARIES})
endif()

# ============================================================
# 安装
# ============================================================
install(TARGETS webserver DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/my-webserver/static)
