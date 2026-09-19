// Win32 HID 写路径隔离测试 (对应 Mac 版 test_handshake_io.c)
// 目的: PC 上握手 WriteFile 失败错误码 1167 (ERROR_DEVICE_NOT_CONNECTED) 定位
//   测试 A: 同步 WriteFile (无 FILE_FLAG_OVERLAPPED)
//   测试 B: overlapped WriteFile (与 HidManagerWin::writeData 同路径)
//   测试 C: HidD_SetOutputReport (控制传输 SET_REPORT, 不走中断 OUT 管道)
//   测试 D: 顺带 HidD_FlushDevice 清理 + 复测
// 编译(VS 开发环境): cl /nologo test_win_write.c setupapi.lib hid.lib
// 运行: 需设备 (VID 0x5377 PID 0x5378) 已连接
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// 与 HidManagerWin 一致: ReportID(0x00) + 0x02 前缀 + 握手帧 11B + 填充 = 65B
static const unsigned char HS[11] = {0x53,0x77,0x01,0x00,0x05,0x00,0x01,0xC5,0x69,0x2F,0x26};
static unsigned char g_buf[65];

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
            printf("  设备路径: %s\n", det->DevicePath);
            free(buf); SetupDiDestroyDeviceInfoList(di);
            return h;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(di);
    return NULL;
}

static void try_write(const char *tag, HANDLE h, BOOL overlapped)
{
    OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(h, g_buf, sizeof(g_buf), &written, overlapped ? &ov : NULL);
    DWORD err = ok ? 0 : GetLastError();
    if (overlapped && !ok && err == ERROR_IO_PENDING) {
        // 997 = 异步已排队, 等事件完成 (与应用 writeData 同逻辑)
        if (WaitForSingleObject(ov.hEvent, 10000) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(h, &ov, &written, TRUE);
            err = ok ? 0 : GetLastError();
        } else {
            CancelIoEx(h, &ov);
            printf("%s: WriteFile 超时(10s)\n", tag);
            CloseHandle(ov.hEvent);
            return;
        }
    }
    printf("%s: WriteFile %s, err=%lu, written=%lu\n", tag, ok ? "OK" : "FAIL", err, written);
    if (!ok && overlapped) CancelIoEx(h, &ov);
    CloseHandle(ov.hEvent);
}

int main(void)
{
    g_buf[0] = 0x00;  // Report ID
    g_buf[1] = 0x02;  // 设备协议前缀
    memcpy(g_buf + 2, HS, sizeof(HS));

    printf("=== Win32 HID 写路径隔离测试 (0x5377/0x5378) ===\n");

    // A: 同步句柄
    HANDLE hSync = open_device(0);
    if (!hSync) { printf("未找到设备 (同步句柄)\n"); return 1; }
    printf("[A 同步写]\n"); try_write("  A", hSync, FALSE);

    // C: 控制传输 HidD_SetOutputReport (同步句柄)
    printf("[C 控制传输 HidD_SetOutputReport]\n");
    BOOL sr = HidD_SetOutputReport(hSync, (PVOID)g_buf, sizeof(g_buf));
    printf("  C: HidD_SetOutputReport %s, err=%lu\n", sr ? "OK" : "FAIL", GetLastError());

    // D: Flush 后重试同步写
    printf("[D Flush 后重试]\n");
    HidD_FlushQueue(hSync);
    try_write("  D", hSync, FALSE);

    CloseHandle(hSync);

    // B: overlapped 句柄 (与应用同路径)
    HANDLE hOv = open_device(FILE_FLAG_OVERLAPPED);
    if (!hOv) { printf("未找到设备 (overlapped 句柄)\n"); return 1; }
    printf("[B overlapped 写 (与 app 同路径)]\n"); try_write("  B", hOv, TRUE);

    // B2: overlapped 句柄上再试控制传输
    printf("[B2 overlapped 句柄 + HidD_SetOutputReport]\n");
    BOOL sr2 = HidD_SetOutputReport(hOv, (PVOID)g_buf, sizeof(g_buf));
    printf("  B2: %s, err=%lu\n", sr2 ? "OK" : "FAIL", GetLastError());

    CloseHandle(hOv);

    printf("\n判读: OK 的路径即设备实际可用的写法; 若只有 HidD_SetOutputReport OK,\n");
    printf("      则中断 OUT 管道有问题(VM穿透/固件), 程序写路径应回退控制传输.\n");
    return 0;
}
