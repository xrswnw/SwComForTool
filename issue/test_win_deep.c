// 探针F: PC 深度诊断 —— 切分 "设备没发" vs "PC主机没收到"
//   1) HidD_GetInputReport 控制传输读报告 (完全绕开中断IN管道, 需同步句柄)
//      - 设备应答  => 设备活着, 是中断IN管道在这台PC上不通
//      - 设备无应答 => 设备在该PC上根本不回数据
//   2) 重叠句柄: 写握手帧(等10s) + 重叠读(等10s) x2次 (排查慢响应/首帧唤醒)
//   3) 重叠静默监听 5s (排查设备主动上报)
// 只读+发握手帧, 不写不擦设备。
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *rf = NULL;
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    va_start(ap, fmt); if (rf) { vfprintf(rf, fmt, ap); fflush(rf); } va_end(ap);
}

static HANDLE find_dev(DWORD flags)
{
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return NULL;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
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
            say("设备: %s\n", det->DevicePath);
            free(buf); SetupDiDestroyDeviceInfoList(di);
            return h;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(di);
    return NULL;
}

static int caps_in = 65, caps_out = 65;
static void get_caps(HANDLE h)
{
    PHIDP_PREPARSED_DATA prep = NULL;
    if (HidD_GetPreparsedData(h, &prep) && prep) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength)  caps_in  = caps.InputReportByteLength;
            if (caps.OutputReportByteLength) caps_out = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(prep);
    }
}

static void dump(const char *tag, const BYTE *b, int n)
{
    say("%s %dB:", tag, n);
    for (int k = 0; k < n && k < 32; k++) say(" %02X", b[k]);
    say("\n");
}

// 重叠读, 有界等待; 返回读到的字节数(0=超时无数据, -1=错误)
static int ovl_read(HANDLE h, BYTE *ib, int waitMs, int *outMs)
{
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    int rc = 0;
    int t0 = GetTickCount();
    BOOL ok = ReadFile(h, ib, caps_in, &got, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        say("  ReadFile立即失败 err=%lu\n", GetLastError());
        rc = -1;
    } else if (!ok || got == 0) {
        DWORD wr = WaitForSingleObject(ov.hEvent, waitMs);
        if (wr == WAIT_OBJECT_0) {
            if (GetOverlappedResult(h, &ov, &got, FALSE)) rc = (int)got;
            else { say("  GetOverlappedResult err=%lu\n", GetLastError()); rc = -1; }
        } else {
            CancelIoEx(h, &ov);
            rc = 0;
        }
    } else {
        rc = (int)got;
    }
    if (outMs) *outMs = GetTickCount() - t0;
    CloseHandle(ov.hEvent);
    return rc;
}

int main(void)
{
    rf = fopen("pc_probe_deep.txt", "w");
    say("SwComForTool PC 深度探针 (探针F)\n");

    // ===== 测试1: 控制传输读报告 (同步句柄, 绕开中断IN管道) =====
    HANDLE hs = find_dev(0);
    if (!hs) { say("未找到设备\n"); goto end; }
    get_caps(hs);
    say("caps: In=%dB Out=%dB\n", caps_in, caps_out);

    BYTE rb[256]; memset(rb, 0, sizeof(rb));
    rb[0] = 0x00;
    say("\n== 测试1: HidD_GetInputReport 控制传输读 ==\n");
    if (HidD_GetInputReport(hs, rb, caps_in)) {
        dump("[控制读OK]", rb, caps_in);
        say("  => 设备能通过控制传输回数据, 中断IN管道在这台PC被吞!\n");
    } else {
        say("控制读失败 err=%lu\n", GetLastError());
    }
    CloseHandle(hs);

    // ===== 测试2/3: 重叠句柄 =====
    HANDLE h = find_dev(FILE_FLAG_OVERLAPPED);
    if (!h) { say("重叠句柄打开失败\n"); goto end; }
    get_caps(h);

    BYTE ob[256]; memset(ob, 0, sizeof(ob));
    ob[0] = 0x00; ob[1] = 0x02;
    BYTE hsf[11] = {0x53,0x77,0xFF,0x00,0x05,0x00,0x01,0x1B,0x28,0xC1,0x5E};
    memcpy(ob + 2, hsf, sizeof(hsf));

    say("\n== 测试2: 写握手帧(重叠) + 重叠读10s, 试2次 ==\n");
    for (int round = 1; round <= 2; round++) {
        OVERLAPPED wov; memset(&wov, 0, sizeof(wov));
        wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD written = 0;
        BOOL wok = WriteFile(h, ob, caps_out, &written, &wov);
        if (!wok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(wov.hEvent, 10000) == WAIT_OBJECT_0)
                GetOverlappedResult(h, &wov, &written, TRUE);
            else { CancelIoEx(h, &wov); say("[T2.%d] 写超时10s\n", round); }
        } else if (!wok) {
            say("[T2.%d] 写失败 err=%lu\n", round, GetLastError());
        } else {
            say("[T2.%d] 写完成 %luB\n", round, written);
        }
        CloseHandle(wov.hEvent);
        if (written == 0) continue;

        BYTE ib[256]; int ms = 0;
        int n = ovl_read(h, ib, 10000, &ms);
        if (n > 0) { dump("[T2收到]", ib, n); say("  耗时%dms\n", ms); }
        else if (n == 0) say("[T2.%d] 10s无中断IN数据\n", round);
        else say("[T2.%d] 读错误\n", round);
    }

    say("\n== 测试3: 重叠静默监听5s(不发任何数据) ==\n");
    {
        BYTE ib[256]; int ms = 0;
        int n = ovl_read(h, ib, 5000, &ms);
        if (n > 0) dump("[主动上报]", ib, n);
        else say("5s内无主动上报\n");
    }
    CloseHandle(h);

    say("\n判读指引:\n");
    say(" 控制读OK 或 T2收到   => 设备能回, 中断IN管道在该PC被吞(换USB口/线, 禁USB节电, 查驱动)\n");
    say(" 全部无应答           => 设备在该PC上不回数据(优先换USB口/线再跑本工具)\n");
end:
    if (rf) fclose(rf);
    say("结果已写入 pc_probe_deep.txt\n");
    system("pause");
    return 0;
}
