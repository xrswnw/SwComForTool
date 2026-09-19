# ComforTool

基于 Qt6 的串口通信桌面程序。

## 开发规则

- **禁止使用 `rm -f` 等不可恢复的删除命令**，所有删除文件移至 `TrashCan/` 目录
- **备份机制**：必须先执行 `./backup.sh "本轮变更说明"`（备份至 `/Volumes/Record/Person Code/BackupArea/ComforTool`，不可访问时回退 `/Users/swnw/Documents/BackupArea/ComforTool`），文件变更需同步备份并记录日志
- **中文为主**
- **文件路径优于文件名**：存在同名文件时以路径为准

## 技术栈

- Qt6 (Widgets + SerialPort)
- CMake 构建
- C++17

## 目录结构

- `src/` — 源码（传输层抽象 `ITransport` + 串口 `SerialPortManager` + 平台 HID `HidManagerMac/Win`，UI 与业务集中在 `MainWindow.cpp`，协议在 `ProtocolParser`）
- `Qt/6.8.3/macos/` — 本工程自带的 Qt 6.8.3 macOS SDK（aqtinstall 安装，含 SerialPort/Network 等 addon）
- `res/` — 图标与 qrc 资源
- `docs/` — 构建文档（含 `BUILD_WIN10.md`）
- `issue/` — 发布产物（Win_x64 / Win_Arm64 / Mac_Arm64 zip）与问题验证用 C 测试代码
- `Agent/` — 需求与报告
- `TrashCan/` — 回收站（删除文件暂存）
- `.claude/` — 配置；`.venv/` — aqtinstall Python 虚拟环境

## 开发环境（macOS，本机已验证）

- **CMake** 4.x（Homebrew `/opt/homebrew/bin/cmake`，工程要求 ≥ 3.20）
- **Qt 6.8.3**：随工程放在 `Qt/6.8.3/macos`，`CMakeLists.txt` 已内置 `CMAKE_PREFIX_PATH`，**无需系统安装 Qt 或 brew qt**；缺失时可用 `.venv` 里的 aqt 重装（见 `aqtinstall.log`）
- **依赖**：Qt6 Core/Gui/Widgets/SerialPort/Network；HID 用系统 IOHIDManager（IOKit + CoreFoundation 框架，macOS 无需 hidapi/libusb）；最低部署目标 macOS 13
- **无 ninja**：构建用默认 Makefile 生成器

## 构建与运行（macOS）

```bash
# 首次配置（已配置过 build/ 可跳过）
cmake -S . -B build

# 构建（增量）
cmake --build build -j 8

# 运行（app bundle，POST_BUILD 自动 adhoc 签名）
open build/SwComForTool.app
```

- VS Code 任务：`构建 Debug`（默认，构建到 `build-debug/`，带 `-g` 调试符号）、`构建并启动 ComforTool (普通进程)`
- **调试**：用 cpptools 扩展（`ms-vscode.cpptools`，自带 lldb-mi 调试后端，**离线可用**），F5 即可断点调试；Debug 构建在 `build-debug/`，Release 构建在 `build/`，互不干扰。注意：CodeLLDB 扩展在本机不可用——其后端包需从 GitHub Releases 下载，网络被墙会卡死在 "Acquiring platform package"
- USB HID 传输可用 `-DCOMFORTOOL_ENABLE_USB=OFF` 关闭（默认 ON）
- Bundle ID 为 `com.swcomfor.tool3`（旧 ID 被 macOS 拉黑，勿改回）
- 发布打包：`issue/pack.sh`（macdeployqt 后压缩到 `issue/`）

## Windows 构建

见 `docs/BUILD_WIN10.md`（MSVC/MinGW 两种方式，`build_win.bat` 一键脚本，产物统一到 `build/Release/`，需 windeployqt 部署运行时，`issue/pack.bat` 打包）
