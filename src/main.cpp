#include <QApplication>
#include "MainWindow.h"
#include "AppLogger.h"
#include "Version.h"
#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, true);
#ifdef Q_OS_MACOS
    // macOS: 绕过 lldb attach 导致 euid 变化触发 Qt6 setuid 安全检查 fatal.
    // (Windows 无此安全检查, 不需要也不应调用)
    QCoreApplication::setSetuidAllowed(true);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("SwComForTool");
    app.setOrganizationName("SwComForTool");
    app.setApplicationVersion(COMFORTOOL_VERSION);

    // Round_099: 会话日志自动落盘 (exe 同目录 log/session_*.log),
    // 记录启动环境/连接/收发帧/错误码, PC 现场问题可凭该文件远程根因分析.
    AppLogger::init();
    AppLogger::line("SYS", "程序启动");

    // macOS USB HID 走 IOHIDManager, GUI 进程普通用户直连, 无需 root daemon / AEWP.
    // (libusb 路径已废弃: libusb_get_device_list 破坏进程内 IOHIDManagerOpen,
    //  且旧 bundle ID 曾被拉黑; 详见 HidManagerMac.h 头注释.)
    MainWindow window;
    window.show();

    const int ret = app.exec();
    AppLogger::line("SYS", QString("程序退出 code=%1").arg(ret));
    return ret;
}
