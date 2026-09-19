// Win32 HID 握手探针 (PC 现场根因定位, Round_099)
// 背景: PC 上 WriteFile 全部成功但设备零响应(rxBuf=0), Mac 端 64B 报文 [02|帧] 正常.
// 本探针逐种尝试 4 种输出报告拼法, 每次写后等 2.5s 读回, 哪种拼法收到响应即固件期望格式.
// 同时尝试 IOCTL 读取原始 HID 报告描述符 (判断 Report ID 编号语义).
// 编译(x64): 见 build_test_probe.bat; 运行: 设备接在 PC 上直接双击/命令行运行.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#ifndef CTL_CODE
#define CTL_CODE(d,f,m,a) (((d)<<16)|((f)<<14)|((m)<<2)|(a))
#endif
#define HID_CTL_CODE(id)  CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#ifndef IOCTL_HID_GET_REPORT_DESCRIPTOR
#define IOCTL_HID_GET_REPORT_DESCRIPTOR HID_CTL_CODE(0x0)
#endif

// App 握手帧 (取自 PC 会话日志 [SYS][TX]): dst=0xFF 广播
static const unsigned char FRAME[11] = {0x53,0x77,0xFF,0x00,0x05,0x00,0x01,0x1B,0x28,0xC1,0x5E};
static unsigned char g_out[65];   // WriteFile 缓冲
static unsigned char g_in[128];   // 读回缓冲

static HANDLE open_device(DWORD flags)
{
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevs(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return NULL;
    SP_DEVICE_INTERFACE_DATA ifd = {0}; ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        char *buf = (char*)calloc(1, need);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL)) { free(buf); continue; }
        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, flags, NULL);
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        if (h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &attr)
            && attr.VendorID == 0x5377 && attr.ProductID == 0x5378) {
            printf("设备: %s\n", det->DevicePath);
            free(buf); SetupDiDestroyDeviceInfoList(di);
            return h;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(di);
    return NULL;
}

static void dump_descriptor(HANDLE h)
{
    BYTE desc[512] = {0};
    DWORD ret = 0;
    // IOCTL 需要非重叠句柄; METHOD_OUT_DIRECT 用 DeviceIoControl 输出缓冲返回描述符
    if (DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR, NULL, 0, desc, sizeof(desc), &ret, NULL) && ret > 0) {
        printf("报告描述符 (%luB):", ret);
        for (DWORD i = 0; i < ret; i++) {
            if (i % 16 == 0) printf("\n  ");
            printf("%02X ", desc[i]);
        }
        printf("\n");
        // 扫描 Report ID 声明 (0x85 = Report ID 主项): 0x85 <id>
        printf("声明的 Report ID:");
        for (DWORD i = 0; i + 1 < ret; i++) {
            if (desc[i] == 0x85) printf(" 0x%02X", desc[i+1]);
        }
        printf("\n");
    } else {
        printf("报告描述符读取失败 err=%lu (不影响后续测试)\n", GetLastError());
    }
}

// 写一次 + 读等待 ms, 返回收到的字节数(>0 表示设备有响应)
static int write_then_read(HANDLE h, const unsigned char *buf, int len, int waitMs)
{
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, len, &written, NULL);
    DWORD werr = ok ? 0 : GetLastError();
    printf("    WriteFile: %s err=%lu written=%lu\n", ok ? "OK" : "FAIL", werr, written);
    if (!ok) return 0;

    OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    BOOL rok = ReadFile(h, g_in, 65, &got, &ov);
    if (!rok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, waitMs) == WAIT_OBJECT_0) {
            rok = GetOverlappedResult(h, &ov, &got, TRUE);
        } else {
            CancelIoEx(h, &ov);
        }
    }
    if (rok && got > 0) {
        printf("    收到 %luB:", got);
        for (DWORD i = 0; i < got && i < 32; i++) printf(" %02X", g_in[i]);
        printf("%s\n", got > 32 ? " ..." : "");
    } else {
        printf("    (无响应)\n");
        got = 0;
    }
    CloseHandle(ov.hEvent);
    return (int)got;
}

static void fill(const char *name, unsigned char rid, int prefix02, int frameOff)
{
    (void)frameOff;
    memset(g_out, 0, sizeof(g_out));
    g_out[0] = rid;
    int p = 1;
    if (prefix02) g_out[p++] = 0x02;
    memcpy(g_out + p, FRAME, sizeof(FRAME));
    printf("  [%s] ReportID=0x%02X%s 报文: ", name, rid, prefix02 ? " +0x02前缀" : "");
    for (int i = 0; i < p + 4; i++) printf("%02X ", g_out[i]);
    printf("...\n");
}

int main(void)
{
    printf("=== Win32 HID 握手探针 (5377:5378) ===\n");

    HANDLE h = open_device(0);  // 同步句柄 (IOCTL + 同步写读)
    if (!h) { printf("未找到设备! (先确认设备已插入本机)\n"); return 1; }

    PHIDP_PREPARSED_DATA prep = NULL;
    if (HidD_GetPreparsedData(h, &prep) && prep) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
            printf("caps: In=%uB Out=%uB Feature=%uB Usage=%04X:%04X\n",
                   caps.InputReportByteLength, caps.OutputReportByteLength,
                   caps.FeatureReportByteLength, caps.UsagePage, caps.Usage);
        }
        HidD_FreePreparsedData(prep);
    }
    dump_descriptor(h);
    printf("\n开始 4 种拼法测试 (每写一次等 2.5s 读回)...\n\n");

    int hits = 0;
    // L1: 当前 App 拼法 — [00, 02, 帧...]
    fill("L1 App现状", 0x00, 1, 0);
    hits += write_then_read(h, g_out, 65, 2500) > 0;

    // L2: ReportID=0x02, 帧直接跟后 (编号报告语义: 0x02 是真报告ID)
    fill("L2 ID=0x02", 0x02, 0, 0);
    hits += write_then_read(h, g_out, 65, 2500) > 0;

    // L3: ReportID=0, 无 0x02 前缀
    fill("L3 ID=0 无前缀", 0x00, 0, 0);
    hits += write_then_read(h, g_out, 65, 2500) > 0;

    // L4: ReportID=0x02 + 0x02 前缀 + 帧
    fill("L4 ID=0x02+前缀", 0x02, 1, 0);
    hits += write_then_read(h, g_out, 65, 2500) > 0;

    CloseHandle(h);
    printf("\n=== 结论: %d 种拼法获得响应 ===\n", hits);
    printf("把本窗口全部输出复制回给分析即可.\n");
    system("pause");
    return 0;
}
