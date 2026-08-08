// 复现 daemon 枚举: QCoreApplication 主线程 + 独立 QThread 跑 IOHIDManager 枚举.
// 目的: 判断 Qt + 独立线程 + root 环境下 matching 回调是否触发.
//   成功 → daemon 代码逻辑对, 问题在 daemon 进程的 launch 会话
//   失败 → Qt/线程本身导致 IOHIDManager 不派发
//
// 编译: clang++ -std=c++17 -o /tmp/test_qt_enum issue/test_qt_enum.cpp \
//   -F/Users/swnw/Documents/Software/ComforTool/Qt/6.8.3/macos/lib \
//   -framework QtCore -framework IOKit -framework CoreFoundation \
//   -I/Users/swnw/Documents/Software/ComforTool/Qt/6.8.3/macos/lib/QtCore.framework/Headers \
//   -I/Users/swnw/Documents/Software/ComforTool/Qt/6.8.3/macos/include \
//   -I/Users/swnw/Documents/Software/ComforTool/Qt/6.8.3/macos/include/QtCore
#include <QCoreApplication>
#include <QApplication>
#include <QThread>
#include <QTimer>
#include <QList>
#include <QString>
#include <QTimer>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

struct Info { quint16 vid, pid; QString product; };

static QList<Info> g_list;
static int g_thread_matched = 0;

static void enumMatchingCb(void * /*c*/, IOReturn, void *, IOHIDDeviceRef dev)
{
    if (!dev) return;
    g_thread_matched = 1;
    Info info;
    int v=0, p=0;
    CFNumberRef vn = (CFNumberRef)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDVendorIDKey));
    CFNumberRef pn = (CFNumberRef)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey));
    if (vn) CFNumberGetValue(vn, kCFNumberIntType, &v);
    if (pn) CFNumberGetValue(pn, kCFNumberIntType, &p);
    info.vid = quint16(v); info.pid = quint16(p);
    CFStringRef s = (CFStringRef)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey));
    if (s) info.product = QString::fromCFString(s);
    g_list.append(info);
    printf("[thread matching_cb] VID=0x%04x PID=0x%04x product=%s\n",
           info.vid, info.pid, info.product.toUtf8().constData());
}

class EnumThread : public QThread {
public:
    quint16 vid=0x5377, pid=0x5378;
protected:
    void run() override {
        printf("[EnumThread] run start, tid=%p\n", (void*)QThread::currentThreadId());
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!mgr) { printf("[EnumThread] mgr null\n"); return; }
        CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int v=vid, p=pid;
        CFNumberRef vN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
        CFNumberRef pN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &p);
        CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vN);
        CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pN);
        CFRelease(vN); CFRelease(pN);
        IOHIDManagerSetDeviceMatching(mgr, match);
        CFRelease(match);
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, enumMatchingCb, nullptr);
        IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
        printf("[EnumThread] IOHIDManagerOpen=0x%x\n", r);
        if (r == kIOReturnSuccess) {
            SInt32 res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.5, false);
            printf("[EnumThread] CFRunLoopRunInMode res=%ld\n", (long)res);
            IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        }
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRelease(mgr);
        printf("[EnumThread] run end, matched=%d count=%d\n", g_thread_matched, g_list.size());
    }
};

int main(int argc, char *argv[])
{
    // QApplication: 带 cocoa 平台插件, 会初始化 IOHIDManager 抢占 HID 匹配 (复现 GUI 现象)
    QApplication app(argc, argv);
    printf("=== Qt+Thread IOHIDManager 枚举复现 (uid=%d euid=%d) [QApplication/cocoa] ===\n", getuid(), geteuid());

    EnumThread t;
    t.start();
    t.wait(5000);
    printf("\n=== 结果 ===\n");
    printf("线程内 matching 触发: %s, 枚举到 %d 个设备\n",
           g_thread_matched ? "是" : "否", g_list.size());

    // 再测一次: 主线程直接跑 CFRunLoop (daemon 当前的方式)
    printf("\n--- 对比: 主线程直接 CFRunLoop ---\n");
    g_list.clear(); g_thread_matched = 0;
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (mgr) {
        CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int v=0x5377, p=0x5378;
        CFNumberRef vN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
        CFNumberRef pN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &p);
        CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vN);
        CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pN);
        CFRelease(vN); CFRelease(pN);
        IOHIDManagerSetDeviceMatching(mgr, match);
        CFRelease(match);
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, enumMatchingCb, nullptr);
        if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.5, false);
            IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        }
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRelease(mgr);
    }
    printf("主线程 matching 触发: %s, 枚举到 %d 个设备\n",
           g_thread_matched ? "是" : "否", g_list.size());

    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
