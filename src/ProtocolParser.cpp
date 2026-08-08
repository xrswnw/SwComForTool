#include "ProtocolParser.h"
#include <QDebug>

// Qt 6.11+ QString::arg(T,int,int,QChar) 不再隐式接受 unsigned，统一转 int
static QString hex2(int v) { return QString("%1").arg(v, 2, 16, QChar('0')); }

ProtocolParser::ProtocolParser(QObject *parent) : QObject(parent) {}

quint32 ProtocolParser::crc32(const quint8 *data, int len)
{
    quint32 crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (quint32)data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }
    return crc;
}

quint32 ProtocolParser::crc32(const QByteArray &data)
{
    return crc32(reinterpret_cast<const quint8*>(data.constData()), data.size());
}

quint32 ProtocolParser::updateCrc32(quint32 crc, const quint8 *data, int len) const
{
    return crc32(data, len);
}

quint32 ProtocolParser::calcBindVerify(const quint8 *fwData, int fwLen, quint32 deviceUidHash)
{
    // MPEG-2 CRC-32 对 [固件数据 + UID哈希(4字节,小端序)] 一体计算
    // poly=0x04C11DB7, init=0xFFFFFFFF, 无反射, 无最终XOR — 与MCU端一致
    quint8 uidBytes[4] = {
        static_cast<quint8>(deviceUidHash & 0xFF),
        static_cast<quint8>((deviceUidHash >> 8) & 0xFF),
        static_cast<quint8>((deviceUidHash >> 16) & 0xFF),
        static_cast<quint8>((deviceUidHash >> 24) & 0xFF)
    };

    quint32 crc = 0xFFFFFFFF;

    for (int i = 0; i < fwLen; i++) {
        crc ^= (quint32)fwData[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }

    for (int i = 0; i < 4; i++) {
        crc ^= (quint32)uidBytes[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }

    return crc;
}

QByteArray ProtocolParser::makeFrame(quint8 devAddr, quint8 fc, const QByteArray &data) const
{
    QByteArray frame;
    frame.append(static_cast<char>(0x53));
    frame.append(static_cast<char>(0x77));
    frame.append(static_cast<char>(devAddr));
    frame.append(static_cast<char>(0x00)); // Reserved
    quint16 length = 1 + data.size() + 4; // FC + Data + CRC32
    frame.append(static_cast<char>(length & 0xFF));
    frame.append(static_cast<char>((length >> 8) & 0xFF));
    frame.append(static_cast<char>(fc));
    frame.append(data);

    // CRC32 over Header+DevAddr+Rsv+Length+FC+Data
    QByteArray crcData;
    crcData.append(static_cast<char>(0x53));
    crcData.append(static_cast<char>(0x77));
    crcData.append(static_cast<char>(devAddr));
    crcData.append(static_cast<char>(0x00));
    crcData.append(static_cast<char>(length & 0xFF));
    crcData.append(static_cast<char>((length >> 8) & 0xFF));
    crcData.append(static_cast<char>(fc));
    crcData.append(data);
    quint32 crcVal = crc32(reinterpret_cast<const quint8*>(crcData.constData()), crcData.size());
    frame.append(static_cast<char>(crcVal & 0xFF));
    frame.append(static_cast<char>((crcVal >> 8) & 0xFF));
    frame.append(static_cast<char>((crcVal >> 16) & 0xFF));
    frame.append(static_cast<char>((crcVal >> 24) & 0xFF));

    return frame;
}

FrameData ProtocolParser::parseFrame(const QByteArray &raw) const
{
    FrameData result;
    if (raw.size() < 11) {
        result.error = QString("帧长度不足: %1 < 11").arg(raw.size());
        return result;
    }
    if (static_cast<quint8>(raw[0]) != 0x53 || static_cast<quint8>(raw[1]) != 0x77) {
        result.error = QString("帧头错误: 0x%1 0x%2").arg(hex2(static_cast<int>(raw[0])), hex2(static_cast<int>(raw[1])));
        return result;
    }

    result.devAddr = static_cast<quint8>(raw[2]);
    quint16 length = static_cast<quint8>(raw[4]) | (static_cast<quint8>(raw[5]) << 8);
    result.fc = static_cast<quint8>(raw[6]);

    int dataLen = length - 1 - 4; // FC(1) + CRC32(4)
    if (dataLen < 0 || dataLen > 1024) {
        result.error = QString("length 异常: %1").arg(length);
        return result;
    }
    if (raw.size() < 7 + dataLen + 4) {
        result.error = QString("帧长度不匹配: 期望%1 实际%2").arg(7 + dataLen + 4).arg(raw.size());
        return result;
    }

    result.data = raw.mid(7, dataLen);

    // Verify CRC32 (允许 CRC 错误以兼容设备合并回传多帧场景)
    QByteArray crcCheckData = raw.left(7 + dataLen);
    quint32 crcStored = static_cast<quint8>(raw[7 + dataLen])
                       | (static_cast<quint8>(raw[8 + dataLen]) << 8)
                       | (static_cast<quint8>(raw[9 + dataLen]) << 16)
                       | (static_cast<quint8>(raw[10 + dataLen]) << 24);
    quint32 crcCalc = crc32(reinterpret_cast<const quint8*>(crcCheckData.constData()), crcCheckData.size());
    if (crcStored != crcCalc) {
        // CRC 不匹配但帧头/length 合理, 标记 error 但仍返回 valid (设备协议: 帧后追加 data)
        result.error = QString("CRC校验失败: 帧=%1 计算=%2").arg(static_cast<unsigned>(crcStored), 8, 16, QChar('0')).arg(static_cast<unsigned>(crcCalc), 8, 16, QChar('0'));
        // 不 return, 允许上层提取 data
    }

    result.valid = true;
    return result;
}

QByteArray ProtocolParser::makeHandshakeFrame(quint8 devAddr) const
{
    return makeFrame(devAddr, 0x01);
}

QByteArray ProtocolParser::makeEnterBootFrame(quint8 devAddr) const
{
    return makeFrame(devAddr, 0x02);
}

QByteArray ProtocolParser::makeDeviceInfoFrame(quint8 devAddr) const
{
    return makeFrame(devAddr, 0x07);
}

QByteArray ProtocolParser::makeResetFrame(quint8 devAddr) const
{
    // FC=0x08 软件复位: 空参数, 设备回 1B result, 之后延时 20ms NVIC_SystemReset
    return makeFrame(devAddr, 0x08);
}

DeviceInfo ProtocolParser::parseDeviceInfo(const QByteArray &data) const
{
    DeviceInfo info;
    // Round 019 协议 (2026-07-19):
    //   HANDSHAKE data 28B:
    //     [0]result | [1]ProtoVer | [2]status | [3..14]UID(12B)
    //     | [15..18]uidHash(4 LE) | [19]layer | [20..23]upgradeCount(4 LE)
    //     | [24..27]baudRate(4 LE)
    //   DEVICE_INFO data 34B (App=swVersion, Boot=bootVersion):
    //     [0]result | [1]addr | [2..17]hwVersion(16B) | [18..33]version(16B)
    //   兼容旧 Round 010 格式: 12B 简化 HANDSHAKE / 77B DEVICE_INFO
    if (data.size() < 1) return info;

    info.result = static_cast<quint8>(data[0]);

    // ---- Round 019 HANDSHAKE 28B ----
    if (data.size() >= 28 && (data.size() <= 30 || data[3] != 0x00)) {
        // HANDSHAKE 28B 第3字节是 UID 第1字节 (高位, 通常非0)
        info.protoVersion = static_cast<quint8>(data[1]);
        info.deviceStatus  = static_cast<quint8>(data[2]);
        if (data.size() >= 15) {
            for (int i = 0; i < 12; i++) info.uid.append(static_cast<char>(data[3 + i]));
            info.uidHash = static_cast<quint8>(data[15])
                         | (static_cast<quint8>(data[16]) << 8)
                         | (static_cast<quint8>(data[17]) << 16)
                         | (static_cast<quint8>(data[18]) << 24);
        }
        if (data.size() >= 20) info.layer = static_cast<quint8>(data[19]);
        if (data.size() >= 24) {
            info.upgradeCount = static_cast<quint8>(data[20])
                              | (static_cast<quint8>(data[21]) << 8)
                              | (static_cast<quint8>(data[22]) << 16)
                              | (static_cast<quint8>(data[23]) << 24);
        }
        if (data.size() >= 28) {
            info.baudRate = static_cast<quint8>(data[24])
                          | (static_cast<quint8>(data[25]) << 8)
                          | (static_cast<quint8>(data[26]) << 16)
                          | (static_cast<quint8>(data[27]) << 24);
        }
        info.valid = true;
        return info;
    }

    // ---- Round 019 DEVICE_INFO 34B ----
    if (data.size() >= 34 && data[1] <= 0x7F) {
        info.deviceAddr   = static_cast<quint8>(data[1]);
        info.hwVersion    = QString::fromLatin1(data.mid(2, 16).constData()).trimmed();
        info.swVersion    = QString::fromLatin1(data.mid(18, 16).constData()).trimmed();
        info.bootVersion  = info.swVersion;   /* Boot 层时该字段实为 bootVersion */
        info.projectName  = info.hwVersion;
        info.firmwareVersion = info.swVersion;
        info.valid = true;
        return info;
    }

    // ---- 兼容旧 Round 010 12B 简化 HANDSHAKE ----
    //   [0]result [1]proto [2-5]uidHash [6]layer [7]status [8-11]baudRate
    if (data.size() >= 12) {
        info.protoVersion = static_cast<quint8>(data[1]);
        info.uidHash = static_cast<quint8>(data[2])
                     | (static_cast<quint8>(data[3]) << 8)
                     | (static_cast<quint8>(data[4]) << 16)
                     | (static_cast<quint8>(data[5]) << 24);
        info.layer        = static_cast<quint8>(data[6]);
        info.deviceStatus = static_cast<quint8>(data[7]);
        info.baudRate     = static_cast<quint8>(data[8])
                         | (static_cast<quint8>(data[9]) << 8)
                         | (static_cast<quint8>(data[10]) << 16)
                         | (static_cast<quint8>(data[11]) << 24);
        info.valid = true;
        return info;
    }

    // ---- 兼容旧 Round 010 77B DEVICE_INFO ----
    if (data.size() >= 18) {
        info.uidHash = static_cast<quint8>(data[13])
                     | (static_cast<quint8>(data[14]) << 8)
                     | (static_cast<quint8>(data[15]) << 16)
                     | (static_cast<quint8>(data[16]) << 24);
    }
    if (data.size() >= 19) info.protoVersion = static_cast<quint8>(data[17]);
    if (data.size() >= 20) info.layer        = static_cast<quint8>(data[18]);
    if (data.size() >= 21) info.deviceStatus = static_cast<quint8>(data[19]);
    if (data.size() >= 22) info.deviceAddr   = static_cast<quint8>(data[20]);
    if (data.size() >= 26) {
        info.upgradeCount = static_cast<quint8>(data[21])
                          | (static_cast<quint8>(data[22]) << 8)
                          | (static_cast<quint8>(data[23]) << 16)
                          | (static_cast<quint8>(data[24]) << 24);
    }
    if (data.size() >= 57) {
        info.hwVersion = QString::fromLatin1(data.mid(25, 16).constData()).trimmed();
        info.swVersion = QString::fromLatin1(data.mid(41, 16).constData()).trimmed();
        info.projectName = info.hwVersion;
        info.firmwareVersion = info.swVersion;
    }
    if (data.size() >= 73) {
        info.bootVersion = QString::fromLatin1(data.mid(57, 16).constData()).trimmed();
    }
    info.valid = true;
    return info;
}

QByteArray ProtocolParser::makeUpgradeStartFrame(quint8 devAddr, quint32 fwSize, quint32 fwCrc, quint32 fwVer, quint32 bindVerify, quint8 verifyLevel) const
{
    QByteArray data;
    data.append(static_cast<char>(fwSize & 0xFF));
    data.append(static_cast<char>((fwSize >> 8) & 0xFF));
    data.append(static_cast<char>((fwSize >> 16) & 0xFF));
    data.append(static_cast<char>((fwSize >> 24) & 0xFF));
    data.append(static_cast<char>(fwCrc & 0xFF));
    data.append(static_cast<char>((fwCrc >> 8) & 0xFF));
    data.append(static_cast<char>((fwCrc >> 16) & 0xFF));
    data.append(static_cast<char>((fwCrc >> 24) & 0xFF));
    data.append(static_cast<char>(fwVer & 0xFF));
    data.append(static_cast<char>((fwVer >> 8) & 0xFF));
    data.append(static_cast<char>((fwVer >> 16) & 0xFF));
    data.append(static_cast<char>((fwVer >> 24) & 0xFF));
    data.append(static_cast<char>(bindVerify & 0xFF));
    data.append(static_cast<char>((bindVerify >> 8) & 0xFF));
    data.append(static_cast<char>((bindVerify >> 16) & 0xFF));
    data.append(static_cast<char>((bindVerify >> 24) & 0xFF));
    data.append(static_cast<char>(verifyLevel)); // 新增: 校验级别
    return makeFrame(devAddr, 0x03, data);
}

QByteArray ProtocolParser::makeUpgradeDataFrame(quint8 devAddr, quint16 seq, quint16 offset, const QByteArray &fwData) const
{
    QByteArray data;
    data.append(static_cast<char>(seq & 0xFF));
    data.append(static_cast<char>((seq >> 8) & 0xFF));
    data.append(static_cast<char>(offset & 0xFF));
    data.append(static_cast<char>((offset >> 8) & 0xFF));
    data.append(fwData);
    return makeFrame(devAddr, 0x04, data);
}

QByteArray ProtocolParser::makeUpgradeVerifyFrame(quint8 devAddr) const
{
    return makeFrame(devAddr, 0x05);
}

QByteArray ProtocolParser::makeUpgradeExecFrame(quint8 devAddr) const
{
    return makeFrame(devAddr, 0x06);
}

QString ProtocolParser::resultToString(quint8 result, bool isBoot)
{
    if (isBoot) {
        switch (result) {
        case 0x00: return "成功";
        case 0x01: return "固件大小超限或数据不足";
        case 0x02: return "Flash擦除失败";
        default: return QString("未知错误码: 0x%1").arg(hex2(static_cast<int>(result)));
        }
    } else {
        switch (result) {
        case 0x00: return "成功";
        case 0x01: return "设备忙(RF_TIMEOUT)";
        case 0x02: return "CRC校验错误";
        case 0x03: return "帧格式错误";
        case 0x04: return "设备忙(RF_BUSY)";
        case 0x05: return "参数错误";
        case 0x06: return "射频模块错误";
        default: return QString("未知错误码: 0x%1").arg(hex2(static_cast<int>(result)));
        }
    }
}

QString ProtocolParser::verifyResultToString(quint8 result, quint8 detail)
{
    switch (result) {
    case 0x00: return "校验通过";
    case 0x01: return "CRC32不匹配";
    case 0x02: return "固件大小不匹配";
    case 0x03: return detail == 1 ? "Reset向量越界" : "MSP向量无效";
    case 0x04: return "绑定校验失败(固件不属于本机)";
    default: return QString("未知校验错误: 0x%1").arg(hex2(static_cast<int>(result)));
    }
}

QString ProtocolParser::verifyLevelToString(quint8 level)
{
    switch (level) {
    case 0: return "全级(CRC+Size+向量+绑定)";
    case 1: return "中级(CRC+Size+向量)";
    case 2: return "基本级(CRC+Size)";
    default: return QString("未知级别: %1").arg(level);
    }
}

// ============================ 15693 (FC=0x15) ============================

QByteArray ProtocolParser::makeRfidFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params) const
{
    QByteArray data;
    data.append(static_cast<char>(subCmd));
    data.append(params);
    return makeFrame(devAddr, 0x15, data);
}

QByteArray ProtocolParser::makeRfidInventory(quint8 devAddr) const
{
    return makeRfidFrame(devAddr, RFID_SUB_INVENTORY);
}

QByteArray ProtocolParser::makeRfidReadBlock(quint8 devAddr, const QByteArray &uid, quint8 addr, quint8 count) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(addr));
    params.append(static_cast<char>(count));
    return makeRfidFrame(devAddr, RFID_SUB_READ_BLOCK, params);
}

QByteArray ProtocolParser::makeRfidWriteBlock(quint8 devAddr, const QByteArray &uid, quint8 addr, quint8 count, const QByteArray &blkData) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(addr));
    params.append(static_cast<char>(count));
    params.append(blkData);
    return makeRfidFrame(devAddr, RFID_SUB_WRITE_BLOCK, params);
}

QByteArray ProtocolParser::makeRfidDsfid(quint8 devAddr, const QByteArray &uid, quint8 mode, quint8 value) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(mode));
    if (mode == 1)  // 写模式带 value
        params.append(static_cast<char>(value));
    return makeRfidFrame(devAddr, RFID_SUB_DSFID, params);
}

QByteArray ProtocolParser::makeRfidEas(quint8 devAddr, const QByteArray &uid, quint8 op) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(op));
    return makeRfidFrame(devAddr, RFID_SUB_EAS, params);
}

QByteArray ProtocolParser::makeRfidLockBlock(quint8 devAddr, const QByteArray &uid, quint8 addr) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(addr));
    return makeRfidFrame(devAddr, RFID_SUB_LOCK_BLOCK, params);
}

QByteArray ProtocolParser::makeRfidStayQuiet(quint8 devAddr, const QByteArray &uid) const
{
    QByteArray params;
    params.append(uid);
    return makeRfidFrame(devAddr, RFID_SUB_STAY_QUIET, params);
}

QByteArray ProtocolParser::makeRfidWriteAfi(quint8 devAddr, const QByteArray &uid, quint8 afi) const
{
    QByteArray params;
    params.append(uid);
    params.append(static_cast<char>(afi));
    return makeRfidFrame(devAddr, RFID_SUB_WRITE_AFI, params);
}

QByteArray ProtocolParser::makeRfidLockAfi(quint8 devAddr, const QByteArray &uid) const
{
    QByteArray params;
    params.append(uid);
    return makeRfidFrame(devAddr, RFID_SUB_LOCK_AFI, params);
}

// 解析 15693 响应: data = [subCmd][seq][total][payload...]
//   读块(0x01): 片0 payload = [result][data...], 后续片纯 data
//   其余子命令: total=1, payload = [result]...
RfidResponse ProtocolParser::parseRfidResponse(const QByteArray &data) const
{
    RfidResponse r;
    if (data.size() < 3) {
        r.error = QString("15693响应过短: %1 < 3").arg(data.size());
        return r;
    }
    r.subCmd = static_cast<quint8>(data[0]);
    r.seq    = static_cast<quint8>(data[1]);
    r.total  = static_cast<quint8>(data[2]);
    r.payload = data.mid(3);
    r.isFrag0 = (r.seq == 0);
    if (r.total == 0) {
        r.error = "total=0 非法";
        return r;
    }
    // 非 0x01 子命令 total 必须为 1
    if (r.subCmd != RFID_SUB_READ_BLOCK && r.total != 1) {
        r.error = QString("子命令0x%1 total=%2(应为1) RFID_FRAG_ERR")
            .arg(hex2(r.subCmd)).arg(r.total);
        return r;
    }
    // 片0含 result (第一个字节); 读块片0 result 后为 data, 其余子命令 result 即 payload[0]
    if (r.isFrag0 && !r.payload.isEmpty()) {
        r.result = static_cast<quint8>(r.payload[0]);
    }
    r.valid = true;
    return r;
}

QString ProtocolParser::rfidResultToString(quint8 result)
{
    switch (result) {
    case 0x00: return "成功";
    case 0x01: return "未知子命令(RFID_SUBCMD_ERR)";
    case 0x02: return "参数错误(RFID_PARAM_ERR)";
    case 0x03: return "无卡/盘点失败(RFID_NO_TAG)";
    case 0x04: return "UID错误(RFID_UID_ERR)";
    case 0x05: return "超时(RFID_TIMEOUT)";
    case 0x06: return "碰撞(RFID_COLLISION)";
    case 0x07: return "CRC错误(RFID_CRC_ERR)";
    case 0x08: return "通信/协议错误(RFID_COMM_ERR)";
    case 0x09: return "写/锁失败(RFID_WRITE_ERR)";
    case 0x0A: return "射频未初始化(RFID_NOT_INIT)";
    case 0x0B: return "分片错误(RFID_FRAG_ERR)";
    default:   return QString("未知结果: 0x%1").arg(hex2(static_cast<int>(result)));
    }
}

QString ProtocolParser::rfidSubCmdToString(quint8 subCmd)
{
    switch (subCmd) {
    case 0x00: return "Inventory 盘点";
    case 0x01: return "ReadBlock 读块";
    case 0x02: return "WriteBlock 写块";
    case 0x03: return "DSFID 读写";
    case 0x04: return "EAS 操作";
    case 0x05: return "LockBlock 锁块";
    case 0x06: return "StayQuiet 静默";
    case 0x07: return "WriteAFI 写AFI";
    case 0x08: return "LockAFI 锁AFI";
    default:   return QString("未知子命令: 0x%1").arg(hex2(static_cast<int>(subCmd)));
    }
}

// ============================ 14443A (FC=0x14) ============================

QByteArray ProtocolParser::makeRfid14443AFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params) const
{
    // data = [subCmd] + params, FC=0x14
    QByteArray data;
    data.append(static_cast<char>(subCmd));
    data.append(params);
    return makeFrame(devAddr, 0x14, data);
}

QByteArray ProtocolParser::makeRfid14443AGetUid(quint8 devAddr) const
{
    // 0x00 GetUid: 无参数, 设备自动级联 Request/Anticoll/Select
    return makeRfid14443AFrame(devAddr, RFID_14443A_GETUID);
}

QByteArray ProtocolParser::makeRfid14443AHalt(quint8 devAddr) const
{
    return makeRfid14443AFrame(devAddr, RFID_14443A_HALT);
}

QByteArray ProtocolParser::makeRfid14443AAuthM1(quint8 devAddr, const QByteArray &uid, quint8 mode,
                                                  const QByteArray &key, quint8 addr) const
{
    // [0x02][UID 4B][mode][key 6B][addr]
    QByteArray params;
    params.append(uid.left(4));             // M1 UID 4B
    params.append(static_cast<char>(mode));  // 0x60=KeyA / 0x61=KeyB
    params.append(key.left(6));             // 6B key
    params.append(static_cast<char>(addr));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTHM1, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthReadBlockM1(quint8 devAddr, quint8 blockAddr, quint8 mode,
                                                            const QByteArray &key, const QByteArray &uid) const
{
    // [0x30][blockAddr][mode][key 6B][uid 4B] 一键: 内部 GetUid+AuthM1+ReadBlock(0x30)
    //   uid 全 0 → 设备内部 GetUid 拿 uid; uid 非全 0 → 用下发 uid 直接 AuthM1 (跳过盘点)
    QByteArray params;
    params.append(static_cast<char>(blockAddr));
    params.append(static_cast<char>(mode));      // 0x60=KeyA / 0x61=KeyB
    params.append(key.left(6));                  // 6B key
    params.append(uid.left(4));                  // 4B uid (空 = 00 00 00 00)
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_READ_BLOCK, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthWriteBlockM1(quint8 devAddr, quint8 blockAddr, quint8 mode,
                                                           const QByteArray &key, const QByteArray &uid,
                                                           const QByteArray &data16) const
{
    // [0x31][blockAddr][mode][key 6B][uid 4B][data 16B] 一键: 内部 GetUid+AuthM1+WriteBlock(0xA0, 16B)
    QByteArray params;
    params.append(static_cast<char>(blockAddr));
    params.append(static_cast<char>(mode));
    params.append(key.left(6));
    params.append(uid.left(4));
    params.append(data16.left(16));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_WRITE_BLOCK, params);
}

QByteArray ProtocolParser::makeRfid14443AReadBlockM0(quint8 devAddr, quint8 blockAddr) const
{
    // [0x21][blockAddr] Mifare Ultralight 读 16B (0x30, 4 页连读, 无认证)
    QByteArray params;
    params.append(static_cast<char>(blockAddr));
    return makeRfid14443AFrame(devAddr, RFID_14443A_READ_BLOCK_M0, params);
}

QByteArray ProtocolParser::makeRfid14443AWriteBlockM0(quint8 devAddr, quint8 blockAddr, const QByteArray &data4) const
{
    // [0x20][blockAddr][data 4B] Mifare Ultralight 写 4B 页 (0xA2, 无认证)
    QByteArray params;
    params.append(static_cast<char>(blockAddr));
    params.append(data4.left(4));
    return makeRfid14443AFrame(devAddr, RFID_14443A_WRITE_BLOCK_M0, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthInc(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                                  const QByteArray &key, const QByteArray &uid,
                                                  const QByteArray &value4) const
{
    // [0x32][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B] 一键: AuthM1 + Increment(0xC1) + 自动 Transfer
    QByteArray params;
    params.append(static_cast<char>(valueAddr));
    params.append(static_cast<char>(transAddr));
    params.append(static_cast<char>(mode));
    params.append(key.left(6));
    params.append(uid.left(4));
    params.append(value4.left(4));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_INC, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthDec(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                                  const QByteArray &key, const QByteArray &uid,
                                                  const QByteArray &value4) const
{
    // [0x33][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B] 一键: AuthM1 + Decrement(0xC0) + 自动 Transfer
    QByteArray params;
    params.append(static_cast<char>(valueAddr));
    params.append(static_cast<char>(transAddr));
    params.append(static_cast<char>(mode));
    params.append(key.left(6));
    params.append(uid.left(4));
    params.append(value4.left(4));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_DEC, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthTransfer(quint8 devAddr, quint8 transAddr, quint8 mode,
                                                       const QByteArray &key, const QByteArray &uid) const
{
    // [0x34][transAddr][mode][key 6B][uid 4B] 一键: AuthM1 + 裸 Transfer(0xB0), 依赖前序 Inc/Dec/Restore
    QByteArray params;
    params.append(static_cast<char>(transAddr));
    params.append(static_cast<char>(mode));
    params.append(key.left(6));
    params.append(uid.left(4));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_TRANSFER, params);
}

QByteArray ProtocolParser::makeRfid14443AAuthRestore(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                                     const QByteArray &key, const QByteArray &uid,
                                                     const QByteArray &value4) const
{
    // [0x35][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B] 一键: AuthM1 + Restore(0xC2) + 自动 Transfer
    //   value 字段卡端忽略(原样读入源值块), 保留以与 Inc/Dec 同构 17B
    QByteArray params;
    params.append(static_cast<char>(valueAddr));
    params.append(static_cast<char>(transAddr));
    params.append(static_cast<char>(mode));
    params.append(key.left(6));
    params.append(uid.left(4));
    params.append(value4.left(4));
    return makeRfid14443AFrame(devAddr, RFID_14443A_AUTH_RESTORE, params);
}

QByteArray ProtocolParser::makeRfid14443ARats(quint8 devAddr) const
{
    return makeRfid14443AFrame(devAddr, RFID_14443A_RATS);
}

QByteArray ProtocolParser::makeRfid14443ATransApdu(quint8 devAddr, const QByteArray &apdu) const
{
    // [0x11][apdu N B]
    return makeRfid14443AFrame(devAddr, RFID_14443A_TRANS_APDU, apdu);
}

QByteArray ProtocolParser::makeRfid14443ATopazRead(quint8 devAddr, quint8 addr) const
{
    // [0x12][addr]
    QByteArray params;
    params.append(static_cast<char>(addr));
    return makeRfid14443AFrame(devAddr, RFID_14443A_TOPAZ_READ, params);
}

QByteArray ProtocolParser::makeRfid14443ATopazWrite(quint8 devAddr, quint8 addr, const QByteArray &data8) const
{
    // [0x13][addr][data 8B]
    QByteArray params;
    params.append(static_cast<char>(addr));
    params.append(data8.left(8));
    return makeRfid14443AFrame(devAddr, RFID_14443A_TOPAZ_WRITE, params);
}

RfidResponse ProtocolParser::parseRfid14443AResponse(const QByteArray &data) const
{
    // 14443A 响应格式与 15693 一致: [subCmd][seq][total][payload]
    //   片0 payload = [result][data...], 后续片纯 data (读块可分片)
    // 复用 15693 的解析逻辑 (total=1 校验仅对 15693 READ_BLOCK 放宽, 14443A 也允许分片)
    RfidResponse r;
    if (data.size() < 3) {
        r.error = QString("14443A响应过短: %1 < 3").arg(data.size());
        return r;
    }
    r.subCmd = static_cast<quint8>(data[0]);
    r.seq    = static_cast<quint8>(data[1]);
    r.total  = static_cast<quint8>(data[2]);
    r.payload = data.mid(3);
    r.isFrag0 = (r.seq == 0);
    if (r.total == 0) {
        r.error = "total=0 非法";
        return r;
    }
    // 14443A 所有子命令 total=1 (单包, 保留分片头仅为与 15693 对齐)
    //   §8.3: 所有子命令 total=1; 这里仍放宽校验避免旧分片响应误判
    if (r.total != 1) {
        r.error = QString("子命令0x%1 total=%2(应为1) RFID_FRAG_ERR")
            .arg(hex2(r.subCmd)).arg(r.total);
        return r;
    }
    if (r.isFrag0 && !r.payload.isEmpty()) {
        r.result = static_cast<quint8>(r.payload[0]);
    }
    r.valid = true;
    return r;
}

QString ProtocolParser::rfid14443AResultToString(quint8 result)
{
    // 通用 0x00~0x0C (FC=0x14/0x15 共用) + 14443A 专用 0x0D (见 §10)
    switch (result) {
    case 0x00: return "成功";
    case 0x01: return "未知子命令(SUBCMD_ERR)";
    case 0x02: return "参数错误(PARAM_ERR)";
    case 0x03: return "无卡/盘点失败(NO_TAG)";
    case 0x04: return "UID错误(UID_ERR)";
    case 0x05: return "超时(TIMEOUT)";
    case 0x06: return "碰撞(COLLISION)";
    case 0x07: return "CRC错误(CRC_ERR)";
    case 0x08: return "通信/协议错误(COMM_ERR)";
    case 0x09: return "写/锁失败(WRITE_ERR)";
    case 0x0A: return "射频未初始化(NOT_INIT)";
    case 0x0B: return "分片错误(FRAG_ERR)";
    case 0x0C: return "队列满(BUSY)";
    case 0x0D: return "14443A 认证失败(AUTH_ERR)";
    default:   return QString("未知结果: 0x%1").arg(hex2(static_cast<int>(result)));
    }
}

QString ProtocolParser::rfid14443ASubCmdToString(quint8 subCmd)
{
    switch (subCmd) {
    case 0x00: return "GetUid 盘点UID";
    case 0x01: return "Halt 静默";
    case 0x02: return "AuthM1 Mifare认证";
    case 0x10: return "RATS";
    case 0x11: return "TransAPDU 透传";
    case 0x12: return "Topaz ReadBlock";
    case 0x13: return "Topaz WriteBlock";
    case 0x20: return "WriteBlockM0 写4B";
    case 0x21: return "ReadBlockM0 读16B";
    case 0x30: return "AuthReadBlockM1 一键读块";
    case 0x31: return "AuthWriteBlockM1 一键写块";
    case 0x32: return "AuthIncrement 值块加";
    case 0x33: return "AuthDecrement 值块减";
    case 0x34: return "AuthTransfer 值块提交";
    case 0x35: return "AuthRestore 值块恢复";
    default:   return QString("未知子命令: 0x%1").arg(hex2(static_cast<int>(subCmd)));
    }
}

// ============================ 射频控制 (FC=0x16) ============================

QByteArray ProtocolParser::makeRfidCtrlFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params) const
{
    // data = [subCmd] + params, FC=0x16
    QByteArray data;
    data.append(static_cast<char>(subCmd));
    data.append(params);
    return makeFrame(devAddr, 0x16, data);
}

QByteArray ProtocolParser::makeRfidCtrlInit(quint8 devAddr, quint8 proto) const
{
    // [0x00][proto] proto: 0x01=15693, 0x02=14443A
    QByteArray params;
    params.append(static_cast<char>(proto));
    return makeRfidCtrlFrame(devAddr, RFID_CTRL_INIT, params);
}

QByteArray ProtocolParser::makeRfidCtrlClose(quint8 devAddr) const
{
    return makeRfidCtrlFrame(devAddr, RFID_CTRL_CLOSE);
}

QByteArray ProtocolParser::makeRfidCtrlOpen(quint8 devAddr) const
{
    return makeRfidCtrlFrame(devAddr, RFID_CTRL_OPEN);
}

QByteArray ProtocolParser::makeRfidCtrlDelay(quint8 devAddr, quint8 ms) const
{
    // [0x03][ms]
    QByteArray params;
    params.append(static_cast<char>(ms));
    return makeRfidCtrlFrame(devAddr, RFID_CTRL_DELAY, params);
}

quint8 ProtocolParser::parseRfidCtrlResponse(const QByteArray &data, quint8 expectSubCmd) const
{
    // FC=0x16 同步响应: [subCmd][seq=0][total=1][result]
    if (data.size() < 4) return 0xFF;
    quint8 sub = static_cast<quint8>(data[0]);
    if (sub != expectSubCmd) return 0xFF;
    return static_cast<quint8>(data[3]);  // result
}

QString ProtocolParser::rfidCtrlSubCmdToString(quint8 subCmd)
{
    switch (subCmd) {
    case 0x00: return "INIT 协议初始化";
    case 0x01: return "CLOSE 关射频";
    case 0x02: return "OPEN 开射频";
    case 0x03: return "DELAY 延时";
    default:   return QString("未知控制子命令: 0x%1").arg(hex2(static_cast<int>(subCmd)));
    }
}

// ============================ RfidFragReassembler ============================

bool RfidFragReassembler::addFragment(const RfidResponse &frag, QByteArray &outFull)
{
    if (!frag.valid)
        return false;

    if (!m_started) {
        m_started = true;
        m_subCmd  = frag.subCmd;
        m_total   = frag.total;
        m_received = 0;
        m_frags.clear();
        m_assembled.clear();
        m_done = false;
    } else if (frag.subCmd != m_subCmd || frag.total != m_total) {
        // 不同请求的片混入, 重置
        m_subCmd = frag.subCmd;
        m_total  = frag.total;
        m_received = 0;
        m_frags.clear();
        m_assembled.clear();
        m_done = false;
    }

    if (m_frags.contains(frag.seq))
        return false; // 重复片, 忽略

    m_frags.insert(frag.seq, frag.payload);
    m_received++;

    if (m_received < m_total)
        return false; // 未收齐

    // 收齐: 按 seq 顺序合并
    for (quint8 s = 0; s < m_total; ++s) {
        m_assembled.append(m_frags.value(s));
    }
    outFull = m_assembled;
    m_done = true;
    return true;
}

void RfidFragReassembler::reset()
{
    m_started = false;
    m_done = false;
    m_subCmd = 0;
    m_total = 0;
    m_received = 0;
    m_frags.clear();
    m_assembled.clear();
}
