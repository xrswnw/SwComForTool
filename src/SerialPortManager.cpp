#include "SerialPortManager.h"
#include <QSerialPortInfo>

SerialPortManager::SerialPortManager(QObject *parent)
    : ITransport(parent)
    , m_port(new QSerialPort(this))
{
    connect(m_port, &QSerialPort::readyRead, this, &SerialPortManager::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, &SerialPortManager::onError);
}

SerialPortManager::~SerialPortManager()
{
    if (m_port->isOpen())
        m_port->close();
}

bool SerialPortManager::openPort(const QString &name, int baudRate, int dataBits,
                                  int stopBitsIndex, int parityIndex)
{
    QVariantMap params;
    params["name"] = name;
    params["baudRate"] = baudRate;
    params["dataBits"] = dataBits;
    params["stopBits"] = stopBitsIndex;
    params["parity"] = parityIndex;
    return open(params);
}

bool SerialPortManager::open(const QVariantMap &params)
{
    if (m_port->isOpen())
        m_port->close();

    QString portName = params.value("name").toString();
    int baudRate = params.value("baudRate", 9600).toInt();
    int dataBits = params.value("dataBits", 8).toInt();
    int stopBitsIndex = params.value("stopBits", 0).toInt();
    int parityIndex = params.value("parity", 0).toInt();

    m_port->setPortName(portName);
    m_port->setBaudRate(baudRate);

    switch (dataBits) {
    case 5: m_port->setDataBits(QSerialPort::Data5); break;
    case 6: m_port->setDataBits(QSerialPort::Data6); break;
    case 7: m_port->setDataBits(QSerialPort::Data7); break;
    default: m_port->setDataBits(QSerialPort::Data8); break;
    }

    switch (stopBitsIndex) {
    case 1: m_port->setStopBits(QSerialPort::OneAndHalfStop); break;
    case 2: m_port->setStopBits(QSerialPort::TwoStop); break;
    default: m_port->setStopBits(QSerialPort::OneStop); break;
    }

    switch (parityIndex) {
    case 1: m_port->setParity(QSerialPort::EvenParity); break;
    case 2: m_port->setParity(QSerialPort::OddParity); break;
    default: m_port->setParity(QSerialPort::NoParity); break;
    }

    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (m_port->open(QIODevice::ReadWrite)) {
        emit opened();
        return true;
    }
    return false;
}

void SerialPortManager::close()
{
    if (m_port->isOpen()) {
        m_port->close();
        emit closed();
    }
}

bool SerialPortManager::isOpen() const
{
    return m_port->isOpen();
}

QString SerialPortManager::name() const
{
    return m_port->portName();
}

QString SerialPortManager::errorString() const
{
    return m_port->errorString();
}

void SerialPortManager::writeData(const QByteArray &data)
{
    if (m_port->isOpen())
        m_port->write(data);
}

bool SerialPortManager::waitForBytesWritten(int msecs)
{
    if (m_port->isOpen())
        return m_port->waitForBytesWritten(msecs);
    return false;
}

QByteArray SerialPortManager::readAll()
{
    if (m_port->isOpen())
        return m_port->readAll();
    return {};
}

void SerialPortManager::onReadyRead()
{
    emit dataReceived();
}

void SerialPortManager::onError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::NoError)
        emit portError();
}
