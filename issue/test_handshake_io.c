// 决定性隔离测试: 纯 C IOHIDManager 打开 SwCcrd, 注册 input 回调,
// 发握手包(带 0x02 报告 ID 前缀), 跑 runloop 看 input 回调是否触发.
// 目的: 区分 "设备没回包" vs "Qt callback 代码问题".
//
// 编译: clang -o /tmp/test_hs issue/test_handshake_io.c -framework IOKit -framework CoreFoundation
// 运行: /tmp/test_hs   (普通用户; 需先拔插确保 dext 干净)
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static IOHIDDeviceRef g_dev = NULL;
static int g_input_fired = 0;

static void input_cb(void *ctx, IOReturn result, void *sender,
                     IOHIDReportType type, uint32_t reportID,
                     uint8_t *report, CFIndex len)
{
    (void)ctx; (void)sender; (void)type; (void)reportID;
    g_input_fired = 1;
    printf("[input_cb] fired! result=0x%x len=%ld hex=", result, (long)len);
    for (CFIndex i = 0; i < len && i < 32; i++) printf("%02x ", report[i]);
    printf("\n");
}

static void matching_cb(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)ctx; (void)sender;
    printf("[matching_cb] result=0x%x device=%p\n", result, (void *)device);
    if (!device) return;
    if (g_dev) return;  // 仅第一个

    CFRetain(device);
    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    printf("  IOHIDDeviceOpen = 0x%x %s\n", r, r == kIOReturnSuccess ? "OK" : "FAIL");
    if (r != kIOReturnSuccess) { CFRelease(device); return; }

    static uint8_t buf[64];
    // 顺序: register → schedule (Apple IOHIDLib 标准顺序)
    IOHIDDeviceRegisterInputReportCallback(device, buf, 64, input_cb, NULL);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    g_dev = device;
    printf("  registered input cb + scheduled to runloop\n");
}

int main(void)
{
    printf("=== 握手收发隔离测试 (SwCcrd 0x5377/0x5378) ===\n");
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) { printf("manager create FAIL\n"); return 1; }

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
    if (r != kIOReturnSuccess) { CFRelease(mgr); return 2; }

    // 等 matching 派发
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
    if (!g_dev) { printf("未匹配到设备 (拔插?)\n"); goto done; }

    // 握手包: 0x02 前缀 + 协议帧 (53 77 01 00 05 00 01 C5 69 2F 26) + 0x00 填充到 64B
    uint8_t pkt[64] = {0};
    pkt[0] = 0x02;
    static const uint8_t hs[11] = {0x53,0x77,0x01,0x00,0x05,0x00,0x01,0xC5,0x69,0x2F,0x26};
    memcpy(pkt + 1, hs, sizeof(hs));
    printf("发送握手包 setReport...\n");
    IOReturn sr = IOHIDDeviceSetReport(g_dev, kIOHIDReportTypeOutput, 0, pkt, 64);
    printf("  IOHIDDeviceSetReport = 0x%x %s\n", sr, sr == kIOReturnSuccess ? "OK" : "FAIL");

    // 跑 runloop 3 秒等 input 回调
    printf("跑 runloop 3 秒等 input 回调...\n");
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3.0, false);

    printf("\n=== 结果 ===\n");
    printf("input 回调触发: %s\n", g_input_fired ? "是 → 设备回包, 问题在 Qt 代码" : "否 → 设备没回包, 问题在包/固件");

done:
    if (g_dev) {
        IOHIDDeviceUnscheduleFromRunLoop(g_dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDDeviceClose(g_dev, kIOHIDOptionsTypeNone);
        CFRelease(g_dev);
    }
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return 0;
}
