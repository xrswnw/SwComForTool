// 探针H: 写/读时序测试 —— 钉死 "写时挂起读导致响应丢失" 的规律
//   H1 (10轮): 写 -> 之后才发起读   (探针F模式, App修复后将等效此模式)
//   H2 (10轮): 先挂起读 -> 再写     (App现状模式)
//   H3 (10轮): 先挂起读 -> 写 -> 1s无响应则取消旧读、发起新读 (测试"新读IRP能否捞回丢失的响应")
// 每轮记录 成功/失败 + 响应耗时. 结果写 pc_probe_timing.txt
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

// ---- 单次挂起读的线程: 挂一个 ReadFile, 最多等 waitMs, 记录结果 ----
typedef struct {
    HANDLE hDev;
    int waitMs;
    volatile int done;      // 1=收到数据 0=超时/取消 -1=错误
    int len;
    int ms;
    BYTE data[256];
    OVERLAPPED ov;
    HANDLE finished;
} PreRead;

static DWORD WINAPI preread_thread(LPVOID arg)
{
    PreRead *p = (PreRead*)arg;
    memset(&p->ov, 0, sizeof(p->ov));
    p->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    int t0 = GetTickCount();
    BOOL ok = ReadFile(p->hDev, p->data, caps_in, &got, &p->ov);
    if (ok && got > 0) {
        p->len = (int)got; p->ms = GetTickCount() - t0; p->done = 1;
    } else if (!ok && GetLastError() != ERROR_IO_PENDING) {
        p->done = -1;
    } else {
        DWORD wr = WaitForSingleObject(p->ov.hEvent, p->waitMs);
        if (wr == WAIT_OBJECT_0 && GetOverlappedResult(p->hDev, &p->ov, &got, FALSE)) {
            if (got > 0) { p->len = (int)got; p->ms = GetTickCount() - t0; p->done = 1; }
            else p->done = -1;
        } else {
            CancelIoEx(p->hDev, &p->ov);
            p->done = 0;  // 超时/取消
        }
    }
    CloseHandle(p->ov.hEvent);
    SetEvent(p->finished);
    return 0;
}

// 写握手帧 (重叠, 等待完成); 返回0成功
static int write_hs(HANDLE h)
{
    BYTE ob[256]; memset(ob, 0, sizeof(ob));
    ob[0] = 0x00; ob[1] = 0x02;
    BYTE hs[11] = {0x53,0x77,0xFF,0x00,0x05,0x00,0x01,0x1B,0x28,0xC1,0x5E};
    memcpy(ob + 2, hs, sizeof(hs));
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(h, ob, caps_out, &written, &ov);
    int rc = 0;
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 10000) == WAIT_OBJECT_0)
                GetOverlappedResult(h, &ov, &written, TRUE);
            else { CancelIoEx(h, &ov); rc = -1; }
        } else rc = -2;
    }
    CloseHandle(ov.hEvent);
    return rc;
}

// 普通重叠读(无预挂起): 返回字节数 0=超时
static int plain_read(HANDLE h, int waitMs, int *outMs)
{
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    int rc = 0;
    int t0 = GetTickCount();
    BYTE ib[256];
    BOOL ok = ReadFile(h, ib, caps_in, &got, &ov);
    if (ok && got > 0) rc = (int)got;
    else if (!ok && GetLastError() != ERROR_IO_PENDING) rc = -1;
    else {
        if (WaitForSingleObject(ov.hEvent, waitMs) == WAIT_OBJECT_0
            && GetOverlappedResult(h, &ov, &got, FALSE)) rc = (int)got;
        else { CancelIoEx(h, &ov); rc = 0; }
    }
    if (outMs) *outMs = GetTickCount() - t0;
    CloseHandle(ov.hEvent);
    return rc;
}

static PreRead *arm_preread(HANDLE h, int waitMs)
{
    PreRead *p = (PreRead*)calloc(1, sizeof(PreRead));
    p->hDev = h; p->waitMs = waitMs; p->done = -2;
    p->finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, preread_thread, p, 0, NULL);
    CloseHandle(th);
    return p;
}
static void wait_preread(PreRead *p)
{
    WaitForSingleObject(p->finished, p->waitMs + 2000);
    CloseHandle(p->finished);
    free(p);
}

int main(void)
{
    rf = fopen("pc_probe_timing.txt", "w");
    say("SwComForTool PC 时序探针 (探针H)\n");
    HANDLE h = find_dev(FILE_FLAG_OVERLAPPED);
    if (!h) { say("未找到设备\n"); goto end; }
    get_caps(h);
    say("caps: In=%dB Out=%dB\n\n", caps_in, caps_out);
    int okH1 = 0, okH2 = 0, okH2rescue = 0, okH3 = 0, okH3rescue = 0;

    // ===== H1: 写 -> 读 (10轮) =====
    say("== H1: 写完才发起读 (探针F模式) ==\n");
    for (int i = 1; i <= 10; i++) {
        int ms = 0;
        if (write_hs(h) != 0) { say("H1.%d 写失败\n", i); continue; }
        int n = plain_read(h, 3000, &ms);
        if (n > 0) { okH1++; say("H1.%d 成功 %dms\n", i, ms); }
        else if (n == 0) say("H1.%d 失败(3s超时)\n", i);
        else say("H1.%d 读错误\n", i);
        Sleep(150);
    }
    say("H1 小计: %d/10\n\n", okH1);

    // ===== H2: 先挂起读 -> 写 (10轮, App现状模式) =====
    say("== H2: 读先挂起再写 (App现状模式) ==\n");
    for (int i = 1; i <= 10; i++) {
        PreRead *p = arm_preread(h, 3500);
        Sleep(80);  // 确保读已挂起
        if (write_hs(h) != 0) { say("H2.%d 写失败\n", i); wait_preread(p); continue; }
        wait_preread(p);
        if (p->done == 1) { okH2++; say("H2.%d 成功 %dms\n", i, p->ms); }
        else if (p->done == 0) { okH2rescue++; say("H2.%d 丢失(挂起读超时)\n", i); }
        else say("H2.%d 读错误\n", i);
        Sleep(150);
    }
    say("H2 小计: 成功%d 丢失%d /10\n\n", okH2, okH2rescue);

    // ===== H3: 先挂起读 -> 写 -> 1s无响应换新读 (10轮) =====
    say("== H3: 挂起读+写, 1s无响应则取消旧读换新读 ==\n");
    for (int i = 1; i <= 10; i++) {
        PreRead *p = arm_preread(h, 1000);   // 只等1s
        Sleep(80);
        if (write_hs(h) != 0) { say("H3.%d 写失败\n", i); wait_preread(p); continue; }
        wait_preread(p);
        if (p->done == 1) { okH3++; say("H3.%d 成功(旧读) %dms\n", i, p->ms); Sleep(150); continue; }
        // 旧读超时被取消(p->done==0) 或异常 -> 发起新读
        int ms = 0;
        int n = plain_read(h, 3000, &ms);
        if (n > 0) { okH3++; okH3rescue++; say("H3.%d 成功(新读捞回!) %dms\n", i, ms); }
        else say("H3.%d 彻底丢失(新读也超时)\n", i);
        Sleep(150);
    }
    say("H3 小计: 成功%d (其中新读捞回%d) /10\n\n", okH3, okH3rescue);

    say("==== 总结 ====\n");
    say("H1 写后读: %d/10   H2 写前挂读: %d/10   H3 换读救援: %d/10\n", okH1, okH2, okH3);
    if (okH3rescue > 0)
        say("判读: 取消旧读/发新读能捞回响应 => App修复方案: 写前取消挂起读, 写完立即重挂\n");
    else if (okH1 == 10 && okH2 <= 3)
        say("判读: 规律确凿 —— 写时不得有挂起读; App需改为\"写前取消读,写后重挂\"或读线程串行化\n");
    else
        say("判读: 规律不明确, 请把本文件发回分析\n");
    CloseHandle(h);
end:
    if (rf) fclose(rf);
    say("结果已写入 pc_probe_timing.txt\n");
    system("pause");
    return 0;
}
