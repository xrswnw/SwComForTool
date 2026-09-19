#include "HidManagerWin.h"
#include "AppLogger.h"
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidusage.h>   // HIDP_STATUS_SUCCESS

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// ============================ HidManagerWinThread ============================
// 独立 QThread 跑 overlapped ReadFile 循环. close() 时 CancelIoEx 取消, 线程退出.

class HidManagerWinThread : public QThread
{
public:
    HidManagerWinThread(HidManagerWin *owner, HANDLE hDev,
                        int inputReportLen, int outputReportLen)
        : QThread(nullptr)
        , m_owner(owner)
        , m_hDev(hDev)
        , m_inputReportLen(inputReportLen)
        , m_outputReportLen(outputReportLen) {}

    ~HidManagerWinThread() override { stop(); if (isRunning()) wait(3000); }

    void stop()
    {
        m_stop = true;
        if (m_hDev) CancelIoEx(m_hDev, &m_readOv);  // 让阻塞的 ReadFile 返回 ERROR_OPERATION_ABORTED
    }

    HANDLE device() const { return m_hDev; }
    int outputReportLen() const { return m_outputReportLen; }

protected:
    void run() override
    {
        // 重叠读事件
        m_readOv = {};
        m_readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_readOv.hEvent) { return; }

        QByteArray readBuf;
        readBuf.resize(m_inputReportLen);

        while (!m_stop && m_hDev) {
            DWORD bytesReturned = 0;
            BOOL ok = ReadFile(m_hDev, readBuf.data(), m_inputReportLen,
                              &bytesReturned, &m_readOv);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    // 等待完成(短超时循环, 以便及时响应 stop)
                    while (!m_stop) {
                        DWORD wr = WaitForSingleObject(m_readOv.hEvent, 100);
                        if (wr == WAIT_OBJECT_0) break;
                        if (wr == WAIT_TIMEOUT) continue;
                        // WAIT_FAILED 或其它
                        break;
                    }
                    if (m_stop) break;
                    bytesReturned = 0;
                    if (!GetOverlappedResult(m_hDev, &m_readOv, &bytesReturned, FALSE)) {
                        DWORD e = GetLastError();
                        if (e == ERROR_OPERATION_ABORTED) break;  // close 取消
                        // 其它错误: 短歇后继续(避免死循环空转)
                        ResetEvent(m_readOv.hEvent);
                        QThread::msleep(5);
                        continue;
                    }
                } else if (err == ERROR_OPERATION_ABORTED) {
                    break;  // close 取消
                } else {
                    // 设备可能拔出或错误
                    AppLogger::line("HID-ERR", QString("ReadFile err=%1 (%2)").arg(err).arg(AppLogger::winErrText(err)));
                    QThread::msleep(20);
                    continue;
                }
            }
            if (bytesReturned > 0) {
                // readBuf[0] = Report ID (0x00), 后为设备 64B 报文 [0x02, <63B 帧>]
                // 剥首字节(Report ID) 后入队, appendRxFrame 再剥 0x02 前缀 (与 Mac 同语义)
                int dataLen = (int)bytesReturned - 1;
                if (dataLen > 0) {
                    QByteArray raw(readBuf.constData() + 1, dataLen);
                    m_owner->appendRxFrame(raw);
                    QMetaObject::invokeMethod(m_owner, "dataReceived", Qt::QueuedConnection);
                    QMetaObject::invokeMethod(m_owner, "rawReceived", Qt::QueuedConnection,
                                              Q_ARG(QByteArray, raw));
                }
            }
            ResetEvent(m_readOv.hEvent);
        }

        if (m_readOv.hEvent) { CloseHandle(m_readOv.hEvent); m_readOv.hEvent = nullptr; }
    }

private:
    HidManagerWin *m_owner;
    HANDLE m_hDev;
    int m_inputReportLen;
    int m_outputReportLen;
    volatile bool m_stop = false;
    OVERLAPPED m_readOv;
};

// ============================ 枚举工具 ============================

// HID 字符串 API (HidD_GetManufacturerString 等) 返回 BOOLEAN (Windef.h: BYTE), 非 bool.
// 用 BOOLEAN 匹配真实签名, 否则 MSVC 报 C2664 无法转换函数指针类型.
static QString hidWideString(HANDLE hDev, BOOLEAN (WINAPI *getter)(HANDLE, PVOID, ULONG))
{
    if (!hDev || !getter) return QString();
    wchar_t buf[256] = {0};
    if (getter(hDev, buf, sizeof(buf) / sizeof(buf[0])) && buf[0]) {
        return QString::fromWCharArray(buf);
    }
    return QString();
}

// ============================ HidManagerWin ============================

HidManagerWin::HidManagerWin(QObject *parent) : ITransport(parent) {}

HidManagerWin::~HidManagerWin()
{
    // 同 Mac: 析构期间阻断信号, 避免 close() emit 触发已析构对象
    blockSignals(true);
    close();
}

QList<HidDeviceInfo> HidManagerWin::enumerate(quint16 vid, quint16 pid)
{
    QList<HidDeviceInfo> list;
#ifdef Q_OS_WIN
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return list;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    DWORD idx = 0;
    while (SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, idx++, &ifData)) {
        // 取接口详情(设备路径)
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &required, nullptr);
        if (required == 0) continue;
        QByteArray detailBuf(required, 0);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, required, nullptr, nullptr))
            continue;
        QString devicePath = QString::fromWCharArray(detail->DevicePath);

        // 打开设备查 VID/PID + 描述(0 access 最轻, 允许共享)
        HANDLE hDev = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                                  0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (hDev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(hDev, &attr)) { CloseHandle(hDev); continue; }

        // VID/PID 过滤: vid=pid=0 表示全量
        if (vid != 0 && pid != 0 &&
            !(attr.VendorID == vid && attr.ProductID == pid)) {
            CloseHandle(hDev);
            continue;
        }

        HidDeviceInfo info;
        info.vendorId = attr.VendorID;
        info.productId = attr.ProductID;
        info.manufacturer = hidWideString(hDev, HidD_GetManufacturerString);
        info.product      = hidWideString(hDev, HidD_GetProductString);
        info.serial       = hidWideString(hDev, HidD_GetSerialNumberString);
        info.path = devicePath;   // Windows 设备接口路径, open() 用它
        list.append(info);
        CloseHandle(hDev);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
#else
    Q_UNUSED(vid) Q_UNUSED(pid)
#endif
    return list;
}

bool HidManagerWin::open(const QVariantMap &params)
{
    close();
    quint16 vid = static_cast<quint16>(params.value("vid", SW_VID).toUInt());
    quint16 pid = static_cast<quint16>(params.value("pid", SW_PID).toUInt());
    if (vid == 0 && pid == 0) { vid = SW_VID; pid = SW_PID; }
    QString path = params.value("path").toString();

#ifdef Q_OS_WIN
    // 若无 path, 用 vid/pid 重新枚举取第一个匹配设备路径
    if (path.isEmpty()) {
        auto devs = enumerate(vid, pid);
        AppLogger::line("HID", QString("VID/PID 枚举 %1:%2 命中 %3 个")
                            .arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toUpper()
                            .arg(devs.size()));
        if (devs.isEmpty()) {
            m_lastError = QString("未发现 VID %1 PID %2 的 HID 设备")
                              .arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toUpper();
            return false;
        }
        path = devs.first().path;
    }

    HANDLE hDev = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (hDev == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        m_lastError = QString("CreateFile 失败: %1 (错误码 %2)").arg(path).arg(err);
        AppLogger::line("HID-ERR", QString("CreateFile err=%1 (%2) path=%3")
                            .arg(err).arg(AppLogger::winErrText(err)).arg(path));
        return false;
    }

    // 读取真实报告长度(对 Report ID=0 设备为 1B ReportID + 64B 数据 = 65)
    int inputReportLen = PACKET_SIZE + 1;
    int outputReportLen = PACKET_SIZE + 1;
    PHIDP_PREPARSED_DATA prep = nullptr;
    if (HidD_GetPreparsedData(hDev, &prep) && prep) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength > 0)  inputReportLen  = (int)caps.InputReportByteLength;
            if (caps.OutputReportByteLength > 0) outputReportLen = (int)caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(prep);
    }
    AppLogger::line("HID", QString("打开成功 %1:%2 输入报文=%3B 输出报文=%4B")
                        .arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toUpper()
                        .arg(inputReportLen).arg(outputReportLen));
    AppLogger::line("HID", QString("路径: %1").arg(path));

    // 起读线程
    m_readerThread = new HidManagerWinThread(this, hDev, inputReportLen, outputReportLen);
    m_readerThread->start();
    m_name = QString("USB %1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toUpper();
    emit opened();
    return true;
#else
    Q_UNUSED(vid) Q_UNUSED(pid) Q_UNUSED(path)
    m_lastError = "HidManagerWin 仅支持 Windows";
    return false;
#endif
}

void HidManagerWin::close()
{
#ifdef Q_OS_WIN
    if (m_readerThread) {
        AppLogger::line("HID", "关闭设备");
        HANDLE hDev = m_readerThread->device();
        m_readerThread->stop();
        if (m_readerThread->isRunning()) m_readerThread->wait(2000);
        delete m_readerThread;
        m_readerThread = nullptr;
        if (hDev && hDev != INVALID_HANDLE_VALUE) { CloseHandle(hDev); }
    }
#endif
    m_name.clear();
    {
        QMutexLocker lock(&m_rxMutex);
        m_rxBuf.clear();
    }
    emit closed();
}

bool HidManagerWin::isOpen() const
{
#ifdef Q_OS_WIN
    return m_readerThread && m_readerThread->isRunning();
#else
    return false;
#endif
}

QString HidManagerWin::name() const { return m_name; }
QString HidManagerWin::errorString() const { return m_lastError; }

void HidManagerWin::writeData(const QByteArray &data)
{
#ifdef Q_OS_WIN
    if (!m_readerThread) return;
    HANDLE hDev = m_readerThread->device();
    if (!hDev || hDev == INVALID_HANDLE_VALUE) return;

    int outLen = m_readerThread->outputReportLen();
    QByteArray buf(outLen, 0);
    // buf[0] = Report ID (0x00); buf[1] = 0x02 设备协议前缀; buf[2..] = 协议帧
    if (buf.size() < 2) return;
    buf[0] = (char)0x00;
    buf[1] = (char)0x02;
    int n = qMin(data.size(), outLen - 2);
    std::memcpy(buf.data() + 2, data.constData(), n);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(hDev, buf.data(), outLen, &written, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 30000) == WAIT_OBJECT_0) {
                GetOverlappedResult(hDev, &ov, &written, TRUE);
            } else {
                CancelIoEx(hDev, &ov);
                m_lastError = "WriteFile 超时";
                AppLogger::line("HID-ERR", "WriteFile 超时(30s)");
                CloseHandle(ov.hEvent);
                emit portError();
                return;
            }
        } else {
            m_lastError = QString("WriteFile 失败: 错误 %1 (%2)").arg(err).arg(AppLogger::winErrText(err));
            AppLogger::line("HID-ERR", QString("%1 发帧 %2B").arg(m_lastError).arg(data.size()));
            CloseHandle(ov.hEvent);
            emit portError();
            return;
        }
    }
    CloseHandle(ov.hEvent);
#else
    Q_UNUSED(data)
#endif
}

bool HidManagerWin::waitForBytesWritten(int /*msecs*/) { return isOpen(); }

QByteArray HidManagerWin::readAll()
{
    QMutexLocker lock(&m_rxMutex);
    QByteArray out = m_rxBuf;
    m_rxBuf.clear();
    return out;
}

void HidManagerWin::appendRxFrame(const QByteArray &raw)
{
    // Round 024/Display: raw = 设备 64B 报文, 首字节 0x02 为设备协议前缀(Read Report ID 已在读线程剥离).
    // 保留原文 (含 0x02 前缀 + 尾部填充), 供日志/解析显示完整原始帧;
    // parseFrame 已容忍可选 0x02 前缀, 与 HidManagerMac 语义一致.
    QMutexLocker lock(&m_rxMutex);
    m_rxBuf.append(raw);
}

void HidManagerWin::onReadData(const QByteArray &raw)
{
    // 读线程已直接 appendRxFrame, 本 slot 保留 backward-compat (同 Mac), 实际不被调用
    Q_UNUSED(raw);
}

#endif // Q_OS_WIN (匹配文件首 #ifdef Q_OS_WIN, 包裹整文件 Win 专属实现)

