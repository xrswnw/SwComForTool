// UPGRADE_START 隔离测试: 对 SwCcrd(已进Boot, Status=UPG) 发 START, 看设备回不回.
// 分别试 VerifyLevel=FULL(0) / MID(1) / BASIC(2), 各发一次看哪个有响应.
// 编译: clang -o /tmp/test_start issue/test_upgrade_start.c -framework IOKit -framework CoreFoundation
// 运行: /tmp/test_start  (设备须已在 Boot 层)
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
static uint32_t calcBind(const uint8_t *fw,int len,uint32_t uid){
    uint8_t u[4]={uid&0xff,(uid>>8)&0xff,(uid>>16)&0xff,(uid>>24)&0xff};
    uint32_t c=0xFFFFFFFF;
    for(int i=0;i<len;i++){c^=(uint32_t)fw[i]<<24;for(int j=0;j<8;j++)c=(c&0x80000000)?(c<<1)^0x04C11DB7:c<<1;}
    for(int i=0;i<4;i++){c^=(uint32_t)u[i]<<24;for(int j=0;j<8;j++)c=(c&0x80000000)?(c<<1)^0x04C11DB7:c<<1;}
    return c;
}

static IOHIDDeviceRef g_dev=NULL;
static int g_fired=0;
static uint8_t g_last[64]; static int g_lastlen=0;
static void input_cb(void *ctx, IOReturn res, void *s, IOHIDReportType t, uint32_t rid, uint8_t *r, CFIndex len){
    (void)ctx;(void)s;(void)t;(void)rid;
    g_fired=1; g_lastlen=(int)len;
    if(len<=64){memcpy(g_last,r,len);}
    printf("  [input_cb] res=0x%x len=%ld hex=",res,(long)len);
    for(CFIndex i=0;i<len&&i<32;i++)printf("%02x ",r[i]);
    printf("\n");
}
static void matching_cb(void *ctx, IOReturn res, void *s, IOHIDDeviceRef d){
    (void)ctx;(void)s;(void)res;
    if(!d||g_dev)return;
    CFRetain(d);
    if(IOHIDDeviceOpen(d,kIOHIDOptionsTypeNone)!=kIOReturnSuccess){CFRelease(d);return;}
    static uint8_t buf[64];
    IOHIDDeviceRegisterInputReportCallback(d,buf,64,input_cb,NULL);
    IOHIDDeviceScheduleWithRunLoop(d,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    g_dev=d;
}

static void send_pkt(const uint8_t *frame,int flen){
    uint8_t pkt[64]={0}; pkt[0]=0x02; memcpy(pkt+1,frame,flen>63?63:flen);
    g_fired=0; g_lastlen=0;
    IOHIDDeviceSetReport(g_dev,kIOHIDReportTypeOutput,0,pkt,64);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,3.0,false);
}

int main(int argc,char**argv){
    // 参数: argv[1]=addr(1/255), argv[2]=fwsize, argv[3]=lvl
    uint8_t addr = (argc>1)?(uint8_t)atoi(argv[1]):0xFF;
    uint32_t fwsize = (argc>2)?(uint32_t)atoi(argv[2]):22516;  // 默认4字节对齐(22516)
    uint32_t fwcrc=0xB22FE7B6;
    uint32_t uid=0xCF2F74D7;
    uint8_t lvl = (argc>3)?(uint8_t)atoi(argv[3]):1;
    uint32_t bind = calcBind((uint8_t*)"dummy",5,uid);
    printf("addr=0x%02x fwsize=%u lvl=%d\n",addr,fwsize,lvl);

    printf("=== UPGRADE_START 测试 (VerifyLevel=%d) ===\n",lvl);
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
    if(IOHIDManagerOpen(mgr,kIOHIDOptionsTypeNone)!=kIOReturnSuccess){printf("open fail\n");return 2;}
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,1.0,false);
    if(!g_dev){printf("未匹配设备\n");goto done;}

    // 构造 START 帧: 53 77 FF 00 len FC + data(FwSize4 FwCrc4 FwVer4 Bind4 Lvl1) + CRC4
    uint8_t f[32]={0};
    f[0]=0x53;f[1]=0x77;f[2]=addr;f[3]=0x00;
    // data 17B
    int dlen=17;
    uint16_t length=1+dlen+4; // 22=0x16
    f[4]=length&0xff;f[5]=(length>>8)&0xff;
    f[6]=0x03;
    int di=7;
    f[di++]=fwsize&0xff;f[di++]=(fwsize>>8)&0xff;f[di++]=(fwsize>>16)&0xff;f[di++]=(fwsize>>24)&0xff;
    f[di++]=fwcrc&0xff;f[di++]=(fwcrc>>8)&0xff;f[di++]=(fwcrc>>16)&0xff;f[di++]=(fwcrc>>24)&0xff;
    f[di++]=0;f[di++]=0;f[di++]=0;f[di++]=0; // FwVer
    f[di++]=bind&0xff;f[di++]=(bind>>8)&0xff;f[di++]=(bind>>16)&0xff;f[di++]=(bind>>24)&0xff;
    f[di++]=lvl;
    uint32_t c=crc32m(f,7+dlen);
    f[7+dlen]=c&0xff;f[7+dlen+1]=(c>>8)&0xff;f[7+dlen+2]=(c>>16)&0xff;f[7+dlen+3]=(c>>24)&0xff;
    int flen=7+dlen+4;
    printf("发 START 帧(%dB): ",flen);
    for(int i=0;i<flen;i++)printf("%02x ",f[i]);
    printf("\n");
    send_pkt(f,flen);
    printf("  => %s\n", g_fired?"有响应(设备回了)":"无响应(设备静默丢弃)");

done:
    if(g_dev){IOHIDDeviceUnscheduleFromRunLoop(g_dev,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);IOHIDDeviceClose(g_dev,kIOHIDOptionsTypeNone);CFRelease(g_dev);}
    IOHIDManagerClose(mgr,kIOHIDOptionsTypeNone);CFRelease(mgr);
    return 0;
}
