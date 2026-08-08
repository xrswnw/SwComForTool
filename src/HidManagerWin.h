#pragma once

// HidManagerWin - Windows USB HID 传输, 原生 Win32 HID API 实现 ITransport.
//
// 设计与 HidManagerMac 对齐(1:1 接口), 经 HidManager.h 门面供 MainWindow 跨平台使用:
//   - enumerate(vid,pid): SetupAPI 枚举 HID 设备接口, 取 VID/PID/厂商/产品/序列号
//   - open:   CreateFile(overlapped) 打开设备接口, 起独立读线程 overlapped ReadFile
//   - write:  WriteFile(overlapped) 输出报告
//   - read:   读线程 ReadFile 回调入队 m_rxBuf, readAll() 锁内拷贝清空
//
// 设备: SwCcrd VID 0x5377 PID 0x5378, 单 HID 接口, Report ID=0(无报告ID), 报文 64B, 首字节 0x02 为设备协议前缀.
//   Windows HID class driver 约定: Report ID=0 设备的读写缓冲首字节为 Report ID(填 0x00),
//   后接 64B 报文, 共 65B; driver 剥离首字节, 设备侧收到/发出 64B [0x02, <63B 协议帧>],
//   与 macOS IOHIDDeviceSetReport(reportID=0, 64B) 一致.
//
// 报文长度动态适配: open 时 HidP_GetCaps 读取 InputReportByteLength/OutputReportByteLength
//   (对 Report ID=0 设备为 65 = 1B ReportID + 64B 数据), 写缓冲/读缓冲按它分配, 不写死 65.

#include "ITransport.h"
#include "HidDeviceInfo.h"
#include <QObject>
#include <QByteArray>
#include <QList>
#include <QMutex>

class HidManagerWinThread;

class HidManagerWin : public ITransport
{
    Q_OBJECT
public:
    explicit HidManagerWin(QObject *parent = nullptr);
    ~HidManagerWin() override;

    // 枚举指定 VID:PID 的 HID 设备(默认 Sw01); vid=pid=0 表示全量.
    static QList<HidDeviceInfo> enumerate(quint16 vid = SW_VID, quint16 pid = SW_PID);

    // ITransport 实现
    bool open(const QVariantMap &params) override;
    void close() override;
    bool isOpen() const override;
    QString name() const override;
    QString errorString() const override;
    void writeData(const QByteArray &data) override;
    bool waitForBytesWritten(int msecs = 30000) override;
    QByteArray readAll() override;

    // Sw01 设备 (与 HidManagerMac 保持一致)
    static constexpr quint16 SW_VID = 0x5377;
    static constexpr quint16 SW_PID = 0x5378;
    static constexpr int PACKET_SIZE = 64;   // 协议数据部分长度(不含 Report ID)

    // 读线程入队(锁保护, 剥 0x02 前缀, append m_rxBuf). 与 HidManagerMac 同语义.
    void appendRxFrame(const QByteArray &frame);

signals:
    // 收到原始 HID 包(剥 Report ID 后, 含 0x02 前缀), 调试用
    void rawReceived(const QByteArray &raw);

private slots:
    void onReadData(const QByteArray &raw);  // backward-compat slot (实际不被调用, 同 Mac)

private:
    HidManagerWinThread *m_readerThread = nullptr;   // 读线程
    QString m_name;
    QString m_lastError;
    QByteArray m_rxBuf;
    QMutex m_rxMutex;

    friend class HidManagerWinThread;
};
