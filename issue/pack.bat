@echo off
setlocal enabledelayexpansion

REM SwComForTool Package Script (Windows)
REM 前置: build\Release\SwComForTool.exe 存在 (先跑 build_win.bat)

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..
set DEPLOY_DIR=%PROJECT_DIR%\deploy
set BUILD_EXE=%PROJECT_DIR%\build\Release\SwComForTool.exe

REM --- Qt Path: 自动探测 (与 build_win.bat / CMakeLists 一致) ---
set QT_PATH=
if exist "C:\Qt\6.11.1\msvc2022_arm64"      set QT_PATH=C:\Qt\6.11.1\msvc2022_arm64
if not defined QT_PATH if exist "C:\Qt\6.11.1\msvc2022_64"  set QT_PATH=C:\Qt\6.11.1\msvc2022_64
if not defined QT_PATH if exist "C:\Qt\6.8.3\msvc2022_64"   set QT_PATH=C:\Qt\6.8.3\msvc2022_64
if not defined QT_PATH if exist "C:\Qt\6.11.1\mingw_64"     set QT_PATH=C:\Qt\6.11.1\mingw_64
if not defined QT_PATH if exist "C:\Qt\6.8.3\mingw_64"      set QT_PATH=C:\Qt\6.8.3\mingw_64

REM 从 CMakeLists.txt 提取版本号: project(SwComForTool VERSION X.Y ...)
REM 以空格为分隔, 第3个 token 即版本号 (例: project(SwComForTool VERSION 1.0.0 LANGUAGES CXX) -> "1.0.0")
for /f "tokens=3 delims= " %%a in ('findstr /c:"project(SwComForTool VERSION" "%PROJECT_DIR%\CMakeLists.txt"') do set VER=%%a
set "VERSION=V%VER%"
if "%VER%"=="" (
    echo [ERROR] 未能从 CMakeLists.txt 提取版本号 (期望: project(SwComForTool VERSION x.y ...)
    pause
    exit /b 1
)

REM 架构标识: 按构建产物机器判断 (默认 arm64)
set ARCH=WinARM64
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=Win_x64

set PKGNAME=SwComForTool%VERSION%_%ARCH%

echo === SwComForTool Package ===
echo Version: %VERSION%
echo Arch:    %ARCH%
echo Output:  %SCRIPT_DIR%%PKGNAME%.zip
echo Qt:      %QT_PATH%

if not exist "%BUILD_EXE%" (
    echo [ERROR] %BUILD_EXE% not found
    echo 先运行 build_win.bat 生成构建产物
    pause
    exit /b 1
)

REM --- 部署到 deploy 目录 ---
echo Deploying...
if exist "%DEPLOY_DIR%" rd /s /q "%DEPLOY_DIR%"
mkdir "%DEPLOY_DIR%"
copy "%BUILD_EXE%" "%DEPLOY_DIR%\"

if defined QT_PATH (
    if exist "%QT_PATH%\bin\windeployqt.exe" (
        "%QT_PATH%\bin\windeployqt.exe" "%DEPLOY_DIR%\SwComForTool.exe" --release --no-translations --no-opengl-sw
    ) else (
        echo [WARN] windeployqt.exe not found under %QT_PATH%\bin
    )
) else (
    echo [WARN] QT_PATH 未探测到, 跳过 windeployqt (请手动部署 Qt DLL)
)

REM --- Zip ---
echo Compressing...
if exist "%SCRIPT_DIR%%PKGNAME%.zip" del "%SCRIPT_DIR%%PKGNAME%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%DEPLOY_DIR%\*' -DestinationPath '%SCRIPT_DIR%%PKGNAME%.zip'"

echo.
echo === Package Done ===
echo File: %SCRIPT_DIR%%PKGNAME%.zip
pause
