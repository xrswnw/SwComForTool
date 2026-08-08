cmake_minimum_required(VERSION 3.20)
project(SwComForTool VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 版本号传入C++
set(COMFORTOOL_VERSION "V${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
configure_file(src/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

# USB HID 传输开关: 默认开启, 适配其他 Mac(需 brew install hidapi)
# 关闭时仅编译 COM 串口版, UI 隐藏 USB 选项
option(COMFORTOOL_ENABLE_USB "Enable USB HID transport" ON)

# 公共源文件
set(COMFORTOOL_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/SerialPortManager.cpp
    src/ProtocolParser.cpp
)
set(COMFORTOOL_HEADERS
    src/ITransport.h
)

if(COMFORTOOL_ENABLE_USB)
    list(APPEND COMFORTOOL_HEADERS src/HidDeviceInfo.h src/ITransport.h src/HidManager.h)
    if(APPLE)
        list(APPEND COMFORTOOL_SOURCES src/HidManagerMac.cpp)
        list(APPEND COMFORTOOL_HEADERS src/HidManagerMac.h)
    elseif(WIN32)
        list(APPEND COMFORTOOL_SOURCES src/HidManagerWin.cpp)
        list(APPEND COMFORTOOL_HEADERS src/HidManagerWin.h)
    endif()
endif()

# 跨平台构建配置
# macOS: 最低兼容 macOS 13 (Qt 6.8.3 要求), 构建 app bundle 便于 macdeployqt 打包
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
    set(CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/Qt/6.8.3/macos")
    add_executable(SwComForTool MACOSX_BUNDLE
        ${COMFORTOOL_SOURCES}
        ${COMFORTOOL_HEADERS}
        res/res.qrc
        res/icon.icns
    )
    set_target_properties(SwComForTool PROPERTIES
        # 旧 ID com.swcomfor.tool 被 macOS 拉黑 → tool2 也被延续拉黑 (Round 102), 换 tool3 全新 ID
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.swcomfor.tool3"
        MACOSX_BUNDLE_ICON_FILE icon.icns
    )
    # .icns 必须显式指定打包位置, 否则不会拷入 bundle 的 Resources/
    set_source_files_properties(res/icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
    # Windows: 优先使用 CMAKE_PREFIX_PATH 环境变量或 CMake 传入
    if(NOT DEFINED CMAKE_PREFIX_PATH)
        if(EXISTS "C:/Qt/6.11.1/msvc2022_arm64")
            set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_arm64")
        elseif(EXISTS "C:/Qt/6.11.1/msvc2022_64")
            set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")
        elseif(EXISTS "C:/Qt/6.8.3/msvc2022_64")
            set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64")
        elseif(EXISTS "C:/Qt/6.11.1/mingw_64")
            set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/mingw_64")
        elseif(EXISTS "C:/Qt/6.8.3/mingw_64")
            set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/mingw_64")
        endif()
    endif()
    add_executable(SwComForTool WIN32
        ${COMFORTOOL_SOURCES}
        ${COMFORTOOL_HEADERS}
        res/res.qrc
        res/icon.ico
    )
    set_target_properties(SwComForTool PROPERTIES
        WIN32_EXECUTABLE TRUE
        RESOURCE "res/icon.ico"
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort Network)

target_link_libraries(SwComForTool PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Network
)

# ---- USB HID 依赖: macOS 用 IOHIDManager(IOKit/CoreFoundation), 不再依赖 libusb ----
# (libusb 路径废弃: libusb_get_device_list 破坏进程内 IOHIDManagerOpen=kIOReturnNotPermitted;
#  libusb_open 抑制 dext 加载. 详见 src/HidManagerMac.h 头注释.)
if(COMFORTOOL_ENABLE_USB)
    target_compile_definitions(SwComForTool PRIVATE COMFORTOOL_ENABLE_USB=1)
endif()

# macOS: IOHIDManager(HID.framework 在 IOKit) + CoreFoundation(CFRunLoop 类型)
if(APPLE)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    if(IOKIT_FRAMEWORK)
        target_link_libraries(SwComForTool PRIVATE ${IOKIT_FRAMEWORK})
    endif()
    if(COREFOUNDATION_FRAMEWORK)
        target_link_libraries(SwComForTool PRIVATE ${COREFOUNDATION_FRAMEWORK})
    endif()
endif()

# Windows: 原生 Win32 HID (SetupAPI 枚举 + ReadFile/WriteFile), 需 setupapi.lib 与 hid.lib
# (均为 Windows 自带系统库, 无需额外安装; HidManagerWin.cpp 内亦 #pragma comment(lib,..) 双保险)
if(WIN32 AND COMFORTOOL_ENABLE_USB)
    target_link_libraries(SwComForTool PRIVATE setupapi hid)
endif()

target_include_directories(SwComForTool PRIVATE src ${CMAKE_BINARY_DIR}/generated)

# macOS: 构建后 adhoc 签名整个 .app bundle.
# 未签名 bundle 曾导致 IOHIDManagerOpen not permitted(与 bundle ID 拉黑叠加), adhoc 签名+新 ID 才稳定.
if(APPLE)
    add_custom_command(TARGET SwComForTool POST_BUILD
        # $<TARGET_FILE_DIR> = .../SwComForTool.app/Contents/MacOS, /../.. = SwComForTool.app
        COMMAND codesign --force --deep --sign - "$<TARGET_FILE_DIR:SwComForTool>/../.."
        COMMENT "Adhoc codesign SwComForTool.app"
    )
endif()
