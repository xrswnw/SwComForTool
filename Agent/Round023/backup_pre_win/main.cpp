#include <QApplication>
#include "MainWindow.h"
#include "Version.h"
#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, true);
    QCoreApplication::setSetuidAllowed(true);

    QApplication app(argc, argv);
    app.setApplicationName("SwComForTool");
    app.setOrganizationName("SwComForTool");
    app.setApplicationVersion(COMFORTOOL_VERSION);

    // macOS USB HID 走 IOHIDManager, GUI 进程普通用户直连, 无需 root daemon / AEWP.
    // (libusb 路径已废弃: libusb_get_device_list 破坏进程内 IOHIDManagerOpen,
    //  且旧 bundle ID 曾被拉黑; 详见 HidManagerMac.h 头注释.)
    MainWindow window;
    window.show();

    return app.exec();
}
