#!/bin/bash
# ComforTool 打包脚本 (macOS)
# 用法: ./issue/pack.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DEPLOY_DIR="$PROJECT_DIR/deploy"

# 从CMakeLists.txt提取版本号
VER=$(grep -m1 'project(ComforTool VERSION' "$PROJECT_DIR/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+')
VERSION="V${VER}"
PKGNAME="ComforTool${VERSION}_macOS"

echo "=== ComforTool 打包 ==="
echo "版本: $VERSION"
echo "输出: $SCRIPT_DIR/${PKGNAME}.zip"

# 编译
echo "[1/4] 编译..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
cmake --build . --config Release -j$(sysctl -n hw.ncpu) 2>&1 | tail -3

# 部署
echo "[2/4] 部署Qt依赖..."
rm -rf "$DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"

# MACOSX_BUNDLE 产物是 ComforTool.app
APP_BUNDLE="$BUILD_DIR/ComforTool.app"
if [ -d "$APP_BUNDLE" ]; then
    cp -R "$APP_BUNDLE" "$DEPLOY_DIR/"
    APP_PATH="$DEPLOY_DIR/ComforTool.app"
else
    # 兼容非bundle产物
    cp "$BUILD_DIR/ComforTool" "$DEPLOY_DIR/"
    APP_PATH="$DEPLOY_DIR/ComforTool"
fi

# 查找macdeployqt
QT_DIR="$PROJECT_DIR/Qt/6.8.3/macos"
DEPLOYQT="$QT_DIR/bin/macdeployqt"

if [ -x "$DEPLOYQT" ]; then
    "$DEPLOYQT" "$APP_PATH" -verbose=0 2>&1 | tail -3
else
    echo "警告: 未找到macdeployqt，跳过Qt依赖部署"
fi

# 复制SerialPort插件 (macdeployqt可能遗漏)
if [ -d "$QT_DIR/plugins/serialport" ]; then
    mkdir -p "$DEPLOY_DIR/ComforTool.app/Contents/PlugIns/serialport"
    cp "$QT_DIR/plugins/serialport/"*.dylib "$DEPLOY_DIR/ComforTool.app/Contents/PlugIns/serialport/" 2>/dev/null || true
fi

# 打包
echo "[3/4] 压缩..."
cd "$DEPLOY_DIR"
if [ -d "ComforTool.app" ]; then
    zip -r -q "$SCRIPT_DIR/${PKGNAME}.zip" ComforTool.app
else
    zip -r -q "$SCRIPT_DIR/${PKGNAME}.zip" ComforTool
fi

# 清理
echo "[4/4] 清理临时目录..."

echo ""
echo "=== 打包完成 ==="
echo "文件: $SCRIPT_DIR/${PKGNAME}.zip"
ls -lh "$SCRIPT_DIR/${PKGNAME}.zip" 2>/dev/null
