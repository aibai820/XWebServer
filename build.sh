#!/bin/bash
# ============================================================
# 构建脚本
# 用法:
#   ./build.sh          # Debug 构建
#   ./build.sh release  # Release 构建
#   ./build.sh clean    # 清理
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
echo "  Xwebserver 构建脚本"
echo "  平台: $(uname -m)"
echo "  类型: $BUILD_TYPE"
echo "============================================"

# 创建 build 目录
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake 配置
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

# 编译（-j 自动检测核心数）
make -j$(nproc)

echo ""
echo "============================================"
echo "  构建完成！"
echo "  可执行文件: $BUILD_DIR/webserver"
echo "  运行: ./$BUILD_DIR/webserver"
echo "  或指定端口: ./$BUILD_DIR/webserver 8080"
echo "============================================"
