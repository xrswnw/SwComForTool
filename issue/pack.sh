#!/bin/bash
# SwComForTool 打包脚本 (macOS)
# 产物: 自包含 .app bundle (macdeployqt 改 rpath + 拷 Qt 框架/插件 + qt.conf + adhoc 重签)
#   -> issue/Mac_Arm64/      (展开目录, 发到别的 Mac 直接运行; Round024 D2)
#   -> issue/SwComForToolV<ver>_macOS.zip  (分发压缩包)
# 用法: ./issue/pack.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DEPLOY_DIR="$PROJECT_DIR/deploy"
MAC_DIR="$SCRIPT_DIR/Mac_Arm64"

# 从 CMakeLists.txt 提取版本号 (project(SwComForTool VERSION X.Y.Z ...) -> 抓三段, 与 pack.bat 对齐)
VER=$(grep -m1 'project(SwComForTool VERSION' "$PROJECT_DIR/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
VERSION="V${VER}"
PKGNAME="SwComForTool${VERSION}_macOS"

echo "=== SwComForTool 打包 (macOS) ==="
echo "版本: $VERSION"
echo "Mac_Arm64: $MAC_DIR"
echo "zip: $SCRIPT_DIR/${PKGNAME}.zip"

# [1/6] 编译
echo "[1/6] 编译..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
cmake --build . --config Release -j$(sysctl -n hw.ncpu) 2>&1 | tail -5

APP_BUNDLE="$BUILD_DIR/SwComForTool.app"
if [ ! -d "$APP_BUNDLE" ]; then
    echo "[ERROR] 未找到 $APP_BUNDLE"
    exit 1
fi

# [2/6] 部署到 deploy 目录 (清空重建)
echo "[2/6] 部署到 deploy/..."
rm -rf "$DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"
cp -R "$APP_BUNDLE" "$DEPLOY_DIR/"
APP_PATH="$DEPLOY_DIR/SwComForTool.app"

# [3/6] macdeployqt: 改 rpath=@executable_path/../Frameworks + 拷 Qt 框架/插件 + 写 qt.conf
echo "[3/6] macdeployqt..."
QT_DIR="$PROJECT_DIR/Qt/6.8.3/macos"
DEPLOYQT="$QT_DIR/bin/macdeployqt"
if [ -x "$DEPLOYQT" ]; then
    "$DEPLOYQT" "$APP_PATH" -always-overwrite 2>&1 | tail -5
else
    echo "[ERROR] 未找到 macdeployqt: $DEPLOYQT"
    exit 1
fi

# macdeployqt 完成后再次 adhoc 签名整个 bundle (深度), 确保所有拷入的 Qt 框架/插件签名一致
echo "    整包 adhoc 重签..."
codesign --force --deep --sign - "$APP_PATH" 2>&1 | tail -3 || echo "[WARN] codesign 返回非0"

# [4/6] 复制自包含 bundle 到 issue/Mac_Arm64/ (Round024 D2)
echo "[4/6] 复制到 $MAC_DIR ..."
rm -rf "$MAC_DIR"
mkdir -p "$MAC_DIR"
cp -R "$APP_PATH" "$MAC_DIR/"

# 写 VERSION.txt: 版本号 + 构建机 macOS 版本 + 构建时间
MACOS_VER="$(sw_vers -productVersion)"
BUILD_TS="$(date '+%Y-%m-%d %H:%M:%S')"
MINOS="$(otool -l "$MAC_DIR/SwComForTool.app/Contents/MacOS/SwComForTool" | grep -A4 LC_BUILD_VERSION | grep minos | awk '{print $2}')"
cat > "$MAC_DIR/VERSION.txt" <<EOF
SwComForTool $VERSION
构建机 macOS: $MACOS_VER (arm64)
最低运行版本: macOS $MINOS
构建时间: $BUILD_TS
Qt: 6.8.3 (macdeployqt 自包含)
说明: 发到别的 Mac 无需安装 Qt; 首次被 Gatekeeper 拦截请右键->打开;
      USB HID 需在 系统设置->隐私与安全->输入监控 允许。详见 README.txt
EOF

# 写 README.txt
cat > "$MAC_DIR/README.txt" <<'EOF'
SwComForTool macOS 运行说明
==========================
1. 双击 SwComForTool.app 运行 (无需安装 Qt, 已自包含)。
2. 首次运行若被 Gatekeeper 拦截 ("无法打开" / "来自身份不明的开发者"):
   右键 SwComForTool.app -> 打开 -> 仍要打开; 或 系统设置->隐私与安全->仍要打开。
   (本包为 adhoc 签名, 非开发者证书签名, 故需手动放行一次。)
3. USB HID 功能: 首次使用时系统会弹权限请求, 在
   系统设置 -> 隐私与安全 -> 输入监控 中允许 SwComForTool。
4. 串口(COM): 直接可用, 无需额外权限。
5. 最低系统: macOS 13.0 (Ventura) 及以上。兼容 macOS 14/15/26。

如运行异常闪退, 终端执行查看:
  ./SwComForTool.app/Contents/MacOS/SwComForTool
EOF

# [5/6] 压缩 zip (分发用)
echo "[5/6] 压缩 ${PKGNAME}.zip ..."
if [ -f "$SCRIPT_DIR/${PKGNAME}.zip" ]; then
    rm -f "$SCRIPT_DIR/${PKGNAME}.zip"
fi
cd "$DEPLOY_DIR"
zip -r -q "$SCRIPT_DIR/${PKGNAME}.zip" SwComForTool.app

# [6/6] 清理临时 deploy 目录
echo "[6/6] 清理 deploy/ ..."
rm -rf "$DEPLOY_DIR"

echo ""
echo "=== 打包完成 ==="
echo "Mac_Arm64 目录: $MAC_DIR"
ls -la "$MAC_DIR"
echo ""
echo "分发 zip: $SCRIPT_DIR/${PKGNAME}.zip"
ls -lh "$SCRIPT_DIR/${PKGNAME}.zip"
