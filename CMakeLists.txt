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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 生成 compile_commands.json (给 clangd/LSP 用)

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

# MySQL 可选（阶段 4 才用到，先注释掉）
# find_package(PkgConfig)
# pkg_check_modules(MYSQL REQUIRED mysqlclient)

# ============================================================
# 源文件
# ============================================================
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/http/http_connection.cpp
)

set(HEADERS
    src/server.h
    src/http/http_connection.h
    src/sync/locker.h
    src/pool/threadpool.h
)

# ============================================================
# 可执行目标
# ============================================================
add_executable(webserver ${SOURCES} ${HEADERS})

target_include_directories(webserver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(webserver PRIVATE
    Threads::Threads
    # ${MYSQL_LIBRARIES}  # 阶段 4 启用
)

# ============================================================
# 安装
# ============================================================
install(TARGETS webserver DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/my-webserver/static)
