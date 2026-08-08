#pragma once

#include "ITransport.h"
#include <QObject>
#include <QSerialPort>
#include <QByteArray>

// 串口(COM)传输实现, 实现 ITransport 接口
class SerialPortManager : public ITransport
{
    Q_OBJECT

public:
    explicit SerialPortManager(QObject *parent = nullptr);
    ~SerialPortManager() override;

    // ITransport 实现
    bool open(const QVariantMap &params) override;
    void close() override;
    bool isOpen() const override;
    QString name() const override;
    QString errorString() const override;
    void writeData(const QByteArray &data) override;
    bool waitForBytesWritten(int msecs = 30000) override;
    QByteArray readAll() override;

    // 兼容旧接口: 等价于 open()
    bool openPort(const QString &name, int baudRate, int dataBits,
                  int stopBitsIndex, int parityIndex);

private slots:
    void onReadyRead();
    void onError(QSerialPort::SerialPortError error);

private:
    QSerialPort *m_port;
};
