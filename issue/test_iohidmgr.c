// 最小独立验证: 用 IOHIDManager 匹配/打开 SwCcrd (VID 0x5377 PID 0x5378),
// 打印 matching 回调是否触发、IOHIDDeviceOpen 返回码、设备属性。
// 目的: 定位 AppleUserUSBHostHIDDevice dext 是否把该 HID 接口发布成可被
// 其它 client 经 IOHIDManager 打开的 IOHIDDevice.
//
// 编译: clang -o /tmp/test_iohidmgr issue/test_iohidmgr.c -framework IOKit -framework CoreFoundation
// 运行: /tmp/test_iohidmgr   (普通用户即可; 也可 sudo 对比)
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <unistd.h>

static int g_matched = 0;
static int g_open_ok = 0;
static IOReturn g_open_rc = -1;

static void matching_cb(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)ctx; (void)sender;
    g_matched = 1;
    printf("[matching_cb] result=0x%x device=%p\n", result, (void *)device);
    if (!device) return;

    // 打印设备属性
    CFNumberRef vid = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    CFNumberRef pid = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    CFStringRef prod = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    int v = 0, p = 0;
    if (vid) CFNumberGetValue(vid, kCFNumberIntType, &v);
    if (pid) CFNumberGetValue(pid, kCFNumberIntType, &p);
    char prodBuf[128] = {0};
    if (prod) CFStringGetCString(prod, prodBuf, sizeof(prodBuf), kCFStringEncodingUTF8);
    printf("  VID=0x%04x PID=0x%04x product=\"%s\"\n", v, p, prodBuf);

    // 试打开 (kIOHIDOptionsTypeNone = 与 dext 共享)
    g_open_rc = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    printf("  IOHIDDeviceOpen(None) = 0x%x %s\n", g_open_rc,
           g_open_rc == kIOReturnSuccess ? "OK" : "FAIL");
    if (g_open_rc == kIOReturnSuccess) {
        g_open_ok = 1;
        // 立即关, 这里只验证能否打开
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    } else {
        // 重试独占模式 (kIOHIDOptionsTypeSeizeDevice) 看 dext 是否允许 seize
        IOReturn rc2 = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
        printf("  IOHIDDeviceOpen(Seize) = 0x%x %s\n", rc2,
               rc2 == kIOReturnSuccess ? "OK" : "FAIL");
        if (rc2 == kIOReturnSuccess) IOHIDDeviceClose(device, kIOHIDOptionsTypeSeizeDevice);
    }
    // 通知主流程退出 runloop
    CFRunLoopStop(CFRunLoopGetCurrent());
}

int main(void)
{
    printf("=== IOHIDManager 探测 SwCcrd (0x5377/0x5378) ===\n");
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) { printf("IOHIDManagerCreate FAIL\n"); return 1; }

    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid = 0x5377, pid = 0x5378;
    CFNumberRef vNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vNum);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pNum);
    CFRelease(vNum); CFRelease(pNum);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, matching_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    printf("IOHIDManagerOpen = 0x%x %s\n", r, r == kIOReturnSuccess ? "OK" : "FAIL");
    if (r != kIOReturnSuccess) {
        CFRelease(mgr);
        return 2;
    }

    printf("跑 runloop 最多 4 秒等待 matching 回调...\n");
    // 用定时器 4s 后强制退出
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 4.0, false);

    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);

    printf("\n=== 结果 ===\n");
    printf("matching 回调触发: %s\n", g_matched ? "是" : "否(3-4s 内未派发 → dext 未发布该接口供 client 匹配)");
    if (g_matched) {
        printf("IOHIDDeviceOpen(None): %s (rc=0x%x)\n", g_open_ok ? "成功" : "失败", g_open_rc);
    }
    return 0;
}
