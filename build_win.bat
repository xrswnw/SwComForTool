@echo off
REM SwComForTool - Windows Build Script
REM 依赖: Qt6 + CMake + Visual Studio (MSVC).
REM USB HID 走原生 Win32 HID(SetupAPI + ReadFile/WriteFile), 链接系统 setupapi.lib/hid.lib,
REM   无需 libusb/vcpkg 等外部依赖. (详见 src/HidManagerWin.h)

setlocal enabledelayedexpansion

echo ========================================
echo  SwComForTool - Windows Build
echo ========================================

REM --- Qt Path: 自动探测 (与 CMakeLists 一致, 优先 arm64, 回退 x64/mingw) ---
set QT_PATH=
if exist "C:\Qt\6.11.1\msvc2022_arm64"      set QT_PATH=C:\Qt\6.11.1\msvc2022_arm64
if not defined QT_PATH if exist "C:\Qt\6.11.1\msvc2022_64"  set QT_PATH=C:\Qt\6.11.1\msvc2022_64
if not defined QT_PATH if exist "C:\Qt\6.8.3\msvc2022_64"   set QT_PATH=C:\Qt\6.8.3\msvc2022_64
if not defined QT_PATH if exist "C:\Qt\6.11.1\mingw_64"     set QT_PATH=C:\Qt\6.11.1\mingw_64
if not defined QT_PATH if exist "C:\Qt\6.8.3\mingw_64"      set QT_PATH=C:\Qt\6.8.3\mingw_64

REM 允许外部覆盖: set QT_PATH=... 后再运行
if defined QT_PATH_ENV set QT_PATH=%QT_PATH_ENV%

if not defined QT_PATH (
    echo [ERROR] 未在 C:\Qt 探测到 Qt6. 请安装 Qt6 msvc2022, 或:
    echo   set QT_PATH=C:\path\to\Qt6\msvc2022_arm64
    echo   build_win.bat
    pause
    exit /b 1
)
echo Qt: %QT_PATH%

if not exist "%QT_PATH%" (
    echo [ERROR] Qt not found: %QT_PATH%
    pause
    exit /b 1
)

REM --- 切换 Qt 路径时清旧 cache, 避免 CMAKE_PREFIX_PATH 残留导致重配失败 ---
if exist build\CMakeCache.txt (
    for /f "tokens=*" %%i in ('findstr /C:"CMAKE_PREFIX_PATH:PATH" build\CMakeCache.txt 2^>nul') do set OLDCPP=%%i
    if defined OLDCPP (
        echo %OLDCPP% | findstr /C:"%QT_PATH%" >nul
        if errorlevel 1 (
            echo [INFO] Qt path changed, clearing CMakeCache.txt
            if exist build\CMakeCache.txt del build\CMakeCache.txt
        )
    )
)

REM --- CMake Configure (启用 USB HID) ---
if not exist build\CMakeCache.txt (
    cmake -DCMAKE_PREFIX_PATH="%QT_PATH%" -DCOMFORTOOL_ENABLE_USB=ON -B build -S .
) else (
    echo [INFO] Reusing existing build\CMakeCache.txt
)
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
"%QT_PATH%\bin\windeployqt.exe" SwComForTool.exe
set WDQ_ERR=%ERRORLEVEL%
cd ..\..
if %WDQ_ERR% neq 0 (
    echo [WARN] windeployqt returned %WDQ_ERR% (non-fatal)
)

echo Done! Run build\Release\SwComForTool.exe
pause
