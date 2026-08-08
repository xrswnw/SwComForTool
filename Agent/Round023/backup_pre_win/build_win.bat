@echo off
REM ComforTool Windows Build Script
REM Prerequisites: Qt6 + CMake + Visual Studio

echo ========================================
echo  SwComForTool - Windows Build
echo ========================================

REM --- Qt Path ---
set QT_PATH=C:\Qt\6.11.1\msvc2022_arm64

REM --- USB HID 依赖(libusb-1.0) ---
REM 本工程默认启用 USB(HID) 传输, 走 libusb 中断端点直连。Windows 端推荐 vcpkg:
REM   vcpkg install libusb
REM 并在配置时指定 toolchain(或 CMAKE_TOOLCHAIN_FILE)。若无需 USB:
REM   cmake -DCOMFORTOOL_ENABLE_USB=OFF -B build -S .

if not exist "%QT_PATH%" (
    echo [ERROR] Qt not found: %QT_PATH%
    echo Edit QT_PATH in build_win.bat or run:
    echo   cmake -DCMAKE_PREFIX_PATH=your_qt_path -B build -S .
    pause
    exit /b 1
)

REM --- CMake Configure ---
cmake -DCMAKE_PREFIX_PATH="%QT_PATH%" -B build -S .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configure failed
    pause
    exit /b 1
)

REM --- Build ---
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed
    pause
    exit /b 1
)

echo ========================================
echo  Build OK: build\Release\SwComForTool.exe
echo ========================================

REM --- Deploy Qt DLLs ---
echo Deploying Qt runtime...
cd build\Release
"%QT_PATH%\bin\windeployqt.exe" ComforTool.exe
cd ..\..

echo Done! Run build\Release\SwComForTool.exe
pause
