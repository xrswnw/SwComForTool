#pragma once

#include <QObject>
#include <QByteArray>
#include <QVariantMap>

// 传输层抽象接口: 串口(COM) 与 HID(USB) 共用同一接口
// 由 MainWindow 持有, 升级流程与通信调试统一通过 ITransport* 操作
class ITransport : public QObject
{
    Q_OBJECT

public:
    explicit ITransport(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~ITransport() = default;

    // 打开传输通道, 参数由具体实现解析
    //   串口: name/baudRate/dataBits/stopBits/parity
    //   HID : path(或 vid/pid)
    virtual bool open(const QVariantMap &params) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString name() const = 0;       // 端口名/设备路径
    virtual QString errorString() const = 0;
    virtual void writeData(const QByteArray &data) = 0;
    virtual bool waitForBytesWritten(int msecs = 30000) = 0;
    virtual QByteArray readAll() = 0;

signals:
    void dataReceived();
    void portError();
    void opened();
    void closed();
};
