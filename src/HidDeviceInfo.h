#pragma once

// HID 设备描述(枚举结果). 独立头, 供 HidManagerMac 与 MainWindow 共用.
#include <QString>
#include <QList>

struct HidDeviceInfo {
    QString path;           // 显示用定位串: "hid:VVVV:PPPP" (IOHIDManager) 或 "bus:N:addr:M" (libusb)
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString manufacturer;
    QString product;
    QString serial;
};
