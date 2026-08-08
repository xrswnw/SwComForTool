@echo off
setlocal enabledelayedexpansion

REM ComforTool Package Script (Windows)
REM Prerequisites: build\Release\ComforTool.exe exists, windeployqt already run

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..
set DEPLOY_DIR=%PROJECT_DIR%\deploy
set QT_PATH=C:\Qt\6.11.1\msvc2022_arm64

REM Extract version from CMakeLists.txt
for /f "tokens=2,3 delims= )" %%a in ('findstr /c:"project(ComforTool VERSION" "%PROJECT_DIR%\CMakeLists.txt"') do set VER=%%a
set VER=%VER: =%
set VERSION=V%VER%
set PKGNAME=ComforTool%VERSION%_WinARM64

echo === ComforTool Package ===
echo Version: %VERSION%
echo Output: %SCRIPT_DIR%%PKGNAME%.zip

if not exist "%DEPLOY_DIR%\ComforTool.exe" (
    echo [ERROR] %DEPLOY_DIR%\ComforTool.exe not found
    echo Run these steps first:
    echo   1. cmake -S %PROJECT_DIR% -B %PROJECT_DIR%\build -DCMAKE_PREFIX_PATH="%QT_PATH%" 
    echo   2. cmake --build %PROJECT_DIR%\build --config Release
    echo   3. mkdir %DEPLOY_DIR%
    echo   4. cd %PROJECT_DIR%\build\Release
    echo   5. "%QT_PATH%\bin\windeployqt.exe" ComforTool.exe --dir %DEPLOY_DIR% --release --no-translations --no-opengl-sw
    echo   6. copy ComforTool.exe %DEPLOY_DIR%\
    echo   7. copy "%QT_PATH%\bin\Qt6SerialPort.dll" %DEPLOY_DIR%\
    pause
    exit /b 1
)

REM Zip
echo Compressing...
if exist "%SCRIPT_DIR%%PKGNAME%.zip" del "%SCRIPT_DIR%%PKGNAME%.zip"
powershell -Command "Compress-Archive -Path '%DEPLOY_DIR%\*' -DestinationPath '%SCRIPT_DIR%%PKGNAME%.zip'"

echo.
echo === Package Done ===
echo File: %SCRIPT_DIR%%PKGNAME%.zip
pause
