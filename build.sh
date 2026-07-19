#!/bin/bash
# ============================================================
# 构建脚本
# 用法:
#   ./build.sh          # Debug 构建
#   ./build.sh release  # Release 构建
#   ./build.sh clean    # 清理
#
# 注意: 本项目使用 Linux 特定 API (epoll, mmap, writev),
#       必须在 Linux 系统上编译 (或 WSL2).
#       目标平台: Jetson Xavier NX (ARM64 Linux)
# ============================================================

set -e  # 遇错即停

BUILD_DIR="build"
BUILD_TYPE="Debug"

case "$1" in
    clean)
        echo "清理构建目录..."
        rm -rf "$BUILD_DIR"
        echo "完成"
        exit 0
        ;;
    release)
        BUILD_TYPE="Release"
        ;;
esac

echo "============================================"
echo "  my-webserver 构建脚本"
echo "  平台: $(uname -m)"
echo "  系统: $(uname -s)"
echo "  类型: $BUILD_TYPE"
echo "============================================"

# 检查是否为 Linux
if [[ "$(uname -s)" != "Linux" ]]; then
    echo ""
    echo "⚠️  警告: 当前平台不是 Linux!"
    echo "  本项目依赖 Linux 内核 API (epoll, sendfile 等),"
    echo "  请在 Linux 系统或 WSL2 上编译."
    echo "  目标平台: Jetson Xavier NX (ARM64, Ubuntu)"
    echo ""
    read -p "是否继续尝试编译? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# 创建 build 目录
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake 配置（MinGW 环境用特殊 generator）
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]]; then
    cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    mingw32-make -j"$(nproc 2>/dev/null || echo 4)"
else
    cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    make -j"$(nproc 2>/dev/null || echo 4)"
fi

echo ""
echo "============================================"
echo "  构建完成！"
echo "  可执行文件: $BUILD_DIR/webserver"
echo "  运行: ./$BUILD_DIR/webserver"
echo "  或指定端口: ./$BUILD_DIR/webserver 8080"
echo "  指定根目录: ./$BUILD_DIR/webserver 8080 ./static"
echo "============================================"
