# ComforTool Win10 构建指南

## 前置条件

| 工具 | 版本要求 | 下载地址 |
|------|---------|---------|
| Qt6 | 6.8.3+ (MSVC2022_64 或 MinGW) | https://www.qt.io/download |
| CMake | 3.20+ | https://cmake.org/download/ |
| Visual Studio | 2019 或 2022 (含C++桌面开发工作负载) | https://visualstudio.microsoft.com/ |

## 方式一：使用构建脚本

1. 安装 Qt6 到 `C:\Qt\6.8.3\msvc2022_64`
2. 编辑 `build_win.bat`，修改 `QT_PATH` 为你本机Qt路径
3. 在项目根目录打开 **x64 Native Tools Command Prompt**
4. 执行 `build_win.bat`

## 方式二：手动构建

```cmd
:: 配置 (MSVC + Visual Studio 生成器)
cmake -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -B build -S . -G "Visual Studio 17 2022" -A x64

:: 编译
cmake --build build --config Release

:: 部署 Qt 运行时 (必须, 否则无法运行)
cd build\Release
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe ComforTool.exe
cd ..\..
```

## MinGW 方式

```cmd
cmake -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/mingw_64" -B build -S . -G "MinGW Makefiles"
cmake --build build --config Release
cd build
C:\Qt\6.8.3\mingw_64\bin\windeployqt.exe ComforTool.exe
```

## 注意事项

- `windeployqt` 是必须步骤，它会复制 Qt 运行时 DLL 到可执行文件目录
- 串口在 Windows 下显示为 COM1、COM2 等，已通过 `Q_OS_WIN` 宏自动适配
- 编译产物为 `WIN32_EXECUTABLE`，不会弹出控制台窗口
