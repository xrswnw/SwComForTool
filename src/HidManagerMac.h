#pragma once

// HidManagerMac - macOS USB HID 传输, 纯 IOHIDManager 实现 ITransport.
//
// 背景: libusb 路径在 macOS 已废弃:
//   1) libusb_get_device_list 会使同进程后续 IOHIDManagerOpen 返回 kIOReturnNotPermitted;
//   2) libusb_open 抑制 AppleUserUSBHostHIDDevice dext 加载.
//   3) 旧 bundle ID 被 macOS 拉黑(CMake 已改 com.swcomfor.tool2).
// 改用 IOHIDManager: 与 dext 共享(kIOHIDOptionsTypeNone), 普通用户即可, 无需提权/daemon.
//
// 设备: SwCcrd VID 0x5377 PID 0x5378, 单 HID 接口, EP1 IN / EP2 OUT 64B 无 Report ID.
//   输出: IOHIDDeviceSetReport(Output, reportID=0, 64B)
//   输入: IOHIDRegisterInputReportCallback 回调得 64B, 首字节 0x02 为设备加的前缀(非 Report ID), 接收时剥除.
//
// 关键: IOHIDManager matching 是 runloop 异步派发; GUI 主线程 Qt cocoa 事件循环运行时
//   主线程 CFRunLoopRunInMode 不派发 matching, 故枚举与打开均在独立 QThread 跑纯 CFRunLoop.

#include "ITransport.h"
#include "HidDeviceInfo.h"  // HidDeviceInfo 定义
#include <QObject>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QThread>

class HidManagerMac : public ITransport
{
    Q_OBJECT
public:
    explicit HidManagerMac(QObject *parent = nullptr);
    ~HidManagerMac() override;

    // 枚举指定 VID:PID 的 HID 设备(默认 Sw01); vid=pid=0 表示全量.
    // 内部独立 QThread 跑纯 CFRunLoop 承载 matching 回调(主线程不派发).
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

    // Sw01 设备
    static constexpr quint16 SW_VID = 0x5377;
    static constexpr quint16 SW_PID = 0x5378;
    static constexpr int PACKET_SIZE = 64;

    // Round 019: CFRunLoop 线程 (hidmac_input_cb) 直接入队, 暴露受控 append 接口
    // 避免 friend + 跨类访问 private 成员的权限问题. 主线程 readAll() 锁内拷贝.
    void appendRxFrame(const QByteArray &frame);   // 锁保护, 去 0x02 前缀, append m_rxBuf

signals:
    // 收到原始 HID 包(未剥前缀), 调试用打印
    void rawReceived(const QByteArray &raw);

private slots:
    void onReadData(const QByteArray &raw);  // 主线程: 剥前缀入 m_rxBuf + emit dataReceived

private:
    // 工作线程(独立 QThread 跑 CFRunLoop) 的回调经此投递/查询
    void onDeviceMatched(void *devRef);  // matching 回调: open 设备 + 注册输入/移除回调
    void onDeviceLost();                   // removal 回调: 通知主线程
    void setOpened(bool ok, const QString &err);

    void *m_workerThread = nullptr;   // HidManagerMacThread* (定义在 cpp)
    QString m_name;
    QString m_lastError;
    QByteArray m_rxBuf;
    QMutex m_rxMutex;

    friend class HidManagerMacThread;
};
