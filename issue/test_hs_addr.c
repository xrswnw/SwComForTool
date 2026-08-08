// 握手地址探测: 内置 CRC32, 对 devAddr=0x01 和广播 0xFF 各发握手, 看哪个回包.
// 用法: /tmp/test_hs_addr  [若设备 stall 先拔插]
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint32_t crc32m(const uint8_t *d, int n){
    uint32_t c=0xFFFFFFFF;
    for(int i=0;i<n;i++){c^=(uint32_t)d[i]<<24;for(int j=0;j<8;j++)c=(c&0x80000000)?(c<<1)^0x04C11DB7:c<<1;}
    return c;
}

static IOHIDDeviceRef g_dev=NULL;
static int g_fired=0;
static void input_cb(void *ctx, IOReturn res, void *s, IOHIDReportType t, uint32_t rid, uint8_t *r, CFIndex len){
    (void)ctx;(void)s;(void)t;(void)rid;
    g_fired=1;
    printf("  [input_cb] res=0x%x len=%ld hex=",res,(long)len);
    for(CFIndex i=0;i<len&&i<32;i++)printf("%02x ",r[i]);
    printf("\n");
}
static void matching_cb(void *ctx, IOReturn res, void *s, IOHIDDeviceRef d){
    (void)ctx;(void)s;
    if(!d||g_dev)return;
    CFRetain(d);
    IOReturn r=IOHIDDeviceOpen(d,kIOHIDOptionsTypeNone);
    if(r!=kIOReturnSuccess){printf("open FAIL 0x%x\n",r);CFRelease(d);return;}
    static uint8_t buf[64];
    IOHIDDeviceRegisterInputReportCallback(d,buf,64,input_cb,NULL);
    IOHIDDeviceScheduleWithRunLoop(d,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    g_dev=d;
}

static int try_addr(uint8_t addr){
    // 构造握手帧: 53 77 ADDR 00 05 00 01 [CRC4]
    uint8_t f[11]={0x53,0x77,addr,0x00,0x05,0x00,0x01};
    uint32_t c=crc32m(f,7);
    f[7]=c&0xff; f[8]=(c>>8)&0xff; f[9]=(c>>16)&0xff; f[10]=(c>>24)&0xff;
    printf("\n--- 试 devAddr=0x%02x, 帧=",addr);
    for(int i=0;i<11;i++)printf("%02x ",f[i]);
    printf("\n");

    uint8_t pkt[64]={0};
    pkt[0]=0x02;
    memcpy(pkt+1,f,11);
    g_fired=0;
    IOReturn sr=IOHIDDeviceSetReport(g_dev,kIOHIDReportTypeOutput,0,pkt,64);
    printf("  setReport=0x%x %s\n",sr,sr==kIOReturnSuccess?"OK":"FAIL");
    if(sr!=kIOReturnSuccess){printf("  (stall? 需拔插后再试下一个)\n");return 0;}
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,2.0,false);
    printf("  input 回调: %s\n",g_fired?"是 → 此地址有效!":"否");
    return g_fired;
}

int main(void){
    printf("=== 握手地址探测 ===\n");
    IOHIDManagerRef mgr=IOHIDManagerCreate(kCFAllocatorDefault,kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef m=CFDictionaryCreateMutable(kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    int v=0x5377,p=0x5378;
    CFNumberRef vn=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&v);
    CFNumberRef pn=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&p);
    CFDictionarySetValue(m,CFSTR(kIOHIDVendorIDKey),vn);
    CFDictionarySetValue(m,CFSTR(kIOHIDProductIDKey),pn);
    CFRelease(vn);CFRelease(pn);
    IOHIDManagerSetDeviceMatching(mgr,m);CFRelease(m);
    IOHIDManagerRegisterDeviceMatchingCallback(mgr,matching_cb,NULL);
    IOHIDManagerScheduleWithRunLoop(mgr,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    IOReturn r=IOHIDManagerOpen(mgr,kIOHIDOptionsTypeNone);
    printf("mgrOpen=0x%x\n",r);
    if(r!=kIOReturnSuccess){CFRelease(mgr);return 2;}
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,1.0,false);
    if(!g_dev){printf("未匹配设备(拔插?)\n");goto done;}

    try_addr(0x01);
    try_addr(0xFF);

    printf("\n=== 结论 ===\n");
    printf("若两个都否: 设备非 App 层(可能 Boot), 或 OUT 不该带 0x02 前缀\n");
done:
    if(g_dev){IOHIDDeviceUnscheduleFromRunLoop(g_dev,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);IOHIDDeviceClose(g_dev,kIOHIDOptionsTypeNone);CFRelease(g_dev);}
    IOHIDManagerClose(mgr,kIOHIDOptionsTypeNone);CFRelease(mgr);
    return 0;
}
