// 探针E: 逐行复刻 SwComForTool App 的 Win HID I/O 模型
//   FILE_FLAG_OVERLAPPED 句柄 + 后台重叠读线程(与 HidManagerWinThread 相同循环)
//   + 主线程重叠写 [ReportID=00, 0x02前缀, 握手帧] (与 HidManagerWin::writeData 相同)
// 目的: 隔离 "同步句柄读得到 / App 重叠读路径读不到" 的差异
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_hDev = NULL;
static int g_inLen = 65, g_outLen = 65;
static volatile int g_stop = 0;
static volatile int g_rxCount = 0;
static BYTE g_rxBuf[256];
static int g_rxLen = 0;

static HANDLE find_dev(void)
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
        // 与 App 一致: FILE_FLAG_OVERLAPPED
        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        HIDD_ATTRIBUTES attr; attr.Size = sizeof(attr);
        if (h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &attr)
            && attr.VendorID == 0x5377 && attr.ProductID == 0x5378) {
            printf("设备(重叠句柄): %s\n", det->DevicePath);
            free(buf); SetupDiDestroyDeviceInfoList(di);
            return h;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(di);
    return NULL;
}

// ==== 与 HidManagerWinThread::run() 相同的循环 ====
static DWORD WINAPI reader_thread(LPVOID arg)
{
    (void)arg;
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 1;
    BYTE buf[256];
    while (!g_stop && g_hDev) {
        DWORD n = 0;
        BOOL ok = ReadFile(g_hDev, buf, g_inLen, &n, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                while (!g_stop) {
                    DWORD wr = WaitForSingleObject(ov.hEvent, 100);
                    if (wr == WAIT_OBJECT_0 || wr == WAIT_FAILED) break;
                    /* WAIT_TIMEOUT -> continue */
                }
                if (g_stop) break;
                n = 0;
                if (!GetOverlappedResult(g_hDev, &ov, &n, FALSE)) {
                    DWORD e = GetLastError();
                    if (e == ERROR_OPERATION_ABORTED) break;
                    ResetEvent(ov.hEvent); Sleep(5); continue;
                }
            } else if (err == ERROR_OPERATION_ABORTED) {
                break;
            } else {
                printf("[读线程] ReadFile err=%lu\n", err);
                Sleep(20); continue;
            }
        }
        if (n > 0) {
            printf("[读线程] 收到 %luB:", n);
            for (DWORD k = 0; k < n && k < 32; k++) printf(" %02X", buf[k]);
            printf("\n");
            memcpy(g_rxBuf, buf, (n < sizeof(g_rxBuf) ? n : sizeof(g_rxBuf)));
            g_rxLen = (int)n; g_rxCount++;
        }
        ResetEvent(ov.hEvent);
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    return 0;
}

// ==== 与 HidManagerWin::writeData() 相同的写路径 ====
static int app_style_write(const BYTE *frame, int frameLen)
{
    BYTE buf[256]; memset(buf, 0, sizeof(buf));
    buf[0] = 0x00;              // Report ID
    buf[1] = 0x02;              // 设备协议前缀
    memcpy(buf + 2, frame, frameLen);
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(g_hDev, buf, g_outLen, &written, &ov);
    int rc = 0;
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 30000) == WAIT_OBJECT_0) {
                GetOverlappedResult(g_hDev, &ov, &written, TRUE);
                printf("[写] 重叠完成 written=%lu\n", written);
            } else {
                CancelIoEx(g_hDev, &ov);
                printf("[写] 超时30s\n"); rc = -1;
            }
        } else { printf("[写] WriteFile err=%lu\n", err); rc = -2; }
    } else {
        printf("[写] 立即完成 written=%lu\n", written);
    }
    CloseHandle(ov.hEvent);
    return rc;
}

int main(void)
{
    g_hDev = find_dev();
    if (!g_hDev) { printf("未找到设备\n"); return 1; }

    PHIDP_PREPARSED_DATA prep = NULL;
    if (HidD_GetPreparsedData(g_hDev, &prep) && prep) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength)  g_inLen  = caps.InputReportByteLength;
            if (caps.OutputReportByteLength) g_outLen = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(prep);
    }
    printf("caps: In=%dB Out=%dB\n", g_inLen, g_outLen);

    HANDLE th = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    Sleep(500);  // 确保重叠读已挂起

    // 结果落盘: 双击运行时控制台闪退, 也能带走证据
    FILE *rf = fopen("pc_probe_result.txt", "w");
    if (rf) {
        fprintf(rf, "SwComForTool PC 探针 (App重叠I/O复刻)\n");
        fprintf(rf, "句柄: FILE_FLAG_OVERLAPPED  输入=%dB 输出=%dB\n", g_inLen, g_outLen);
    }

    // App 握手帧: makeHandshakeFrame(0xFF)
    BYTE hs[11] = {0x53,0x77,0xFF,0x00,0x05,0x00,0x01,0x1B,0x28,0xC1,0x5E};
    for (int round = 1; round <= 3 && g_rxCount == 0; round++) {
        printf("== 第%d次握手 ==\n", round);
        int wrc = app_style_write(hs, sizeof(hs));
        if (rf) fprintf(rf, "第%d次写: %s\n", round,
                        wrc == 0 ? "完成(65B)" : (wrc == -1 ? "超时30s" : "WriteFile失败"));
        // 等 3s 响应 (与 waitForResponse(3000) 一致)
        for (int t = 0; t < 3000 && g_rxCount == 0; t += 50) Sleep(50);
        printf("3s后 rxCount=%d\n", g_rxCount);
        if (rf) fprintf(rf, "第%d次3s等待: 收到响应 %d 条\n", round, g_rxCount);
    }

    g_stop = 1;
    CancelIoEx(g_hDev, NULL);
    WaitForSingleObject(th, 2000);
    CloseHandle(th); CloseHandle(g_hDev);
    const char *verdict = g_rxCount ? "重叠读线程收到响应 (此机读路径正常)"
                                    : "重叠读线程零响应 (复现App现象: 问题在这台PC/USB链路)";
    printf("结论: %s\n", verdict);
    if (rf) {
        if (g_rxCount && g_rxLen > 0) {
            fprintf(rf, "首条响应 %dB:", g_rxLen);
            for (int k = 0; k < g_rxLen && k < 32; k++) fprintf(rf, " %02X", g_rxBuf[k]);
            fprintf(rf, "\n");
        }
        fprintf(rf, "结论: %s\n", verdict);
        fclose(rf);
        printf("结果已写入 pc_probe_result.txt\n");
    }
    system("pause");
    return 0;
}
