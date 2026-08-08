#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QHash>
#include <cstdint>

// 帧格式: [0x53][0x77][DevAddr][Rsv=0x00][Length_LE16][FC][Data...][CRC32_LE32]
// 帧头 "Sw" (0x53 0x77), 与 Sw01 协议对齐; CRC32 仍为 MPEG-2 变体
//   (poly=0x04C11DB7, init=0xFFFFFFFF, 无反射, 无最终XOR), 与设备一致
// 响应格式: 响应帧 FC = 请求FC XOR 0xFF

struct FrameData {
    quint8  devAddr = 0;
    quint8  fc      = 0;
    QByteArray data;
    bool     valid  = false;
    QString  error;
};

struct DeviceInfo {
    QString projectName;       // 保留兼容，新协议不再返回此字段
    quint8  protoVersion = 0;
    QString firmwareVersion;   // 保留兼容，新协议中的swVersion
    quint8  deviceAddr   = 0;
    bool     valid        = false;
    // Round 019 新增字段
    quint8  result = 0;            // 协议层 result (0=OK)
    QString uid;                  // STM32 12B 唯一 ID hex 字符串
    quint32 uidHash = 0;           // CRC32(UID)
    QString hwVersion;
    QString swVersion;
    QString bootVersion;
    quint8  layer = 0xFF;
    quint8  deviceStatus = 0;
    quint32 upgradeCount = 0;
    quint32 baudRate = 0;
};

// ============================ 15693 (FC=0x15) ============================

// 15693 子命令码
enum RfidSubCmd {
    RFID_SUB_INVENTORY  = 0x00,
    RFID_SUB_READ_BLOCK = 0x01,
    RFID_SUB_WRITE_BLOCK= 0x02,
    RFID_SUB_DSFID      = 0x03,
    RFID_SUB_EAS        = 0x04,
    RFID_SUB_LOCK_BLOCK = 0x05,
    RFID_SUB_STAY_QUIET = 0x06,
    RFID_SUB_WRITE_AFI  = 0x07,
    RFID_SUB_LOCK_AFI   = 0x08
};

// ============================ 14443A (FC=0x14) ============================

// 14443A 子命令码 (见 App_Protocol.html §8)
enum Rfid14443ASubCmd {
    RFID_14443A_GETUID        = 0x00,  // GetUid 读 UID
    RFID_14443A_HALT          = 0x01,  // Halt 静默
    RFID_14443A_AUTHM1        = 0x02,  // AuthM1 M1 认证 [uid 4B][mode][key 6B][blockAddr]
    RFID_14443A_RATS          = 0x10,  // ISO14443A-4 入口
    RFID_14443A_TRANS_APDU    = 0x11,  // ISO14443A-4 APDU 透传
    RFID_14443A_TOPAZ_READ    = 0x12,  // Topaz 读块 [addr]
    RFID_14443A_TOPAZ_WRITE   = 0x13,  // Topaz 写块 [addr][data 8B]
    RFID_14443A_WRITE_BLOCK_M0  = 0x20,  // WriteBlockM0 写 4B (Ultralight 0xA2, 无认证) [blockAddr][data 4B]
    RFID_14443A_READ_BLOCK_M0   = 0x21,  // ReadBlockM0 读 16B (Ultralight 0x30 4页连读, 无认证) [blockAddr]
    RFID_14443A_AUTH_READ_BLOCK  = 0x30,  // AuthReadBlockM1 读 16B (Classic 0x30, 一键) [blockAddr][mode][key 6B][uid 4B]
    RFID_14443A_AUTH_WRITE_BLOCK = 0x31,  // AuthWriteBlockM1 写 16B (Classic 0xA0, 一键) [blockAddr][mode][key 6B][uid 4B][data 16B]
    RFID_14443A_AUTH_INC        = 0x32,  // AuthIncrement 值块加 (0xC1+自动Transfer) [valueAddr][transAddr][mode][key 6B][uid 4B][value 4B]
    RFID_14443A_AUTH_DEC        = 0x33,  // AuthDecrement 值块减 (0xC0+自动Transfer) [valueAddr][transAddr][mode][key 6B][uid 4B][value 4B]
    RFID_14443A_AUTH_TRANSFER   = 0x34,  // AuthTransfer 值块裸提交 (0xB0) [transAddr][mode][key 6B][uid 4B]
    RFID_14443A_AUTH_RESTORE    = 0x35   // AuthRestore 值块恢复/复制 (0xC2+自动Transfer) [valueAddr][transAddr][mode][key 6B][uid 4B][value 4B](卡端忽略)
};

// 14443A 认证模式 (AuthM1/Auth前缀命令的 mode 字段)
enum Rfid14443AAuthMode {
    RFID_AUTH_KEYA = 0x60,  // KeyA
    RFID_AUTH_KEYB = 0x61   // KeyB
};

// ============================ 射频控制 (FC=0x16) ============================

// FC=0x16 子命令码 (见 App_Protocol.html §9)
enum RfidCtrlSubCmd {
    RFID_CTRL_INIT  = 0x00,  // [0x00][proto] proto: 0x01=15693, 0x02=14443A
    RFID_CTRL_CLOSE = 0x01,  // [0x01] 关射频
    RFID_CTRL_OPEN  = 0x02,  // [0x02] 开射频
    RFID_CTRL_DELAY = 0x03   // [0x03][ms] 延时 0..255ms
};

// proto 参数 (RFID_CTRL_INIT 用)
enum RfidProto {
    RFID_PROTO_15693  = 0x01,
    RFID_PROTO_14443A = 0x02
};

// 15693 响应(单片或分片中的单片)
struct RfidResponse {
    quint8  subCmd = 0;
    quint8  seq    = 0;
    quint8  total  = 0;
    quint8  result = 0xFF;   // 仅片0含 result, 后续片为 0
    QByteArray payload;      // 去掉 [subCmd][seq][total] 后的有效载荷
    bool    valid   = false;
    bool    isFrag0 = false;  // 是否为片0(含 result)
    QString error;
};

// 15693 分片重组器: 按 (devAddr,subCmd) 收齐 total 片后合并 payload
class RfidFragReassembler
{
public:
    // 投递一片; 成功合并出完整 payload 时返回 true, outFull 填合并结果
    //   片0: outFull = [result][data...]; 后续片: data 续传追加
    bool addFragment(const RfidResponse &frag, QByteArray &outFull);
    void reset();
    bool done() const { return m_done; }
    quint8 total() const { return m_total; }
    quint8 received() const { return m_received; }

private:
    bool   m_started = false;
    bool   m_done    = false;
    quint8 m_subCmd  = 0;
    quint8 m_total   = 0;
    quint8 m_received= 0;
    QHash<quint8, QByteArray> m_frags; // seq -> payload
    QByteArray m_assembled;
};

class ProtocolParser : public QObject
{
    Q_OBJECT

public:
    explicit ProtocolParser(QObject *parent = nullptr);

    // 帧组装: 请求帧
    QByteArray makeFrame(quint8 devAddr, quint8 fc, const QByteArray &data = QByteArray()) const;

    // 帧解析: 从原始数据中解析帧
    FrameData parseFrame(const QByteArray &raw) const;

    // 握手: 组装握手请求帧
    QByteArray makeHandshakeFrame(quint8 devAddr) const;

    // 进入Boot: 组装进入Bootloader帧
    QByteArray makeEnterBootFrame(quint8 devAddr) const;

    // 设备信息: 组装请求帧
    QByteArray makeDeviceInfoFrame(quint8 devAddr) const;

    // 解析设备信息响应
    DeviceInfo parseDeviceInfo(const QByteArray &data) const;

    // 软件复位: 组装复位请求帧 (空参数, 设备回 OK 后 20ms 软复位)
    QByteArray makeResetFrame(quint8 devAddr) const;

    // 固件更新帧
    // verifyLevel: 0=全级(CRC+Size+向量+绑定), 1=中级(CRC+Size+向量), 2=基本级(CRC+Size)
    QByteArray makeUpgradeStartFrame(quint8 devAddr, quint32 fwSize, quint32 fwCrc, quint32 fwVer, quint32 bindVerify, quint8 verifyLevel = 0) const;
    QByteArray makeUpgradeDataFrame(quint8 devAddr, quint16 seq, quint16 offset, const QByteArray &fwData) const;
    QByteArray makeUpgradeVerifyFrame(quint8 devAddr) const;
    QByteArray makeUpgradeExecFrame(quint8 devAddr) const;

    // 结果码转文字
    static QString resultToString(quint8 result, bool isBoot = false);

    // 校验结果码
    static QString verifyResultToString(quint8 result, quint8 detail = 0);

    // 校验级别: 0=全级, 1=中级, 2=基本级
    static QString verifyLevelToString(quint8 level);

    // CRC32 (MPEG-2: poly=0x04C11DB7, init=0xFFFFFFFF, no final XOR)
    static quint32 crc32(const quint8 *data, int len);
    static quint32 crc32(const QByteArray &data);

    // 设备绑定码计算: MPEG-2 CRC-32(无反射,poly=0x04C11DB7,init=0xFFFFFFFF,无最终XOR) 对 [固件数据 + UID哈希(4字节,小端)]
    static quint32 calcBindVerify(const quint8 *fwData, int fwLen, quint32 deviceUidHash);

    // ============================ 15693 (FC=0x15) ============================

    // 通用: 组 15693 请求帧 (data = [subCmd] + params, FC=0x15)
    QByteArray makeRfidFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params = QByteArray()) const;

    // 便捷组帧(各子命令)
    QByteArray makeRfidInventory(quint8 devAddr) const;                                                        // [0x00]
    QByteArray makeRfidReadBlock(quint8 devAddr, const QByteArray &uid, quint8 addr, quint8 count) const;      // [0x01][UID 8B][addr][count]
    QByteArray makeRfidWriteBlock(quint8 devAddr, const QByteArray &uid, quint8 addr, quint8 count, const QByteArray &blkData) const; // [0x02][UID 8B][addr][count][data]
    QByteArray makeRfidDsfid(quint8 devAddr, const QByteArray &uid, quint8 mode, quint8 value = 0) const;     // [0x03][UID 8B][mode][value?]
    QByteArray makeRfidEas(quint8 devAddr, const QByteArray &uid, quint8 op) const;                           // [0x04][UID 8B][op]
    QByteArray makeRfidLockBlock(quint8 devAddr, const QByteArray &uid, quint8 addr) const;                   // [0x05][UID 8B][addr]
    QByteArray makeRfidStayQuiet(quint8 devAddr, const QByteArray &uid) const;                                 // [0x06][UID 8B]
    QByteArray makeRfidWriteAfi(quint8 devAddr, const QByteArray &uid, quint8 afi) const;                      // [0x07][UID 8B][afi]
    QByteArray makeRfidLockAfi(quint8 devAddr, const QByteArray &uid) const;                                   // [0x08][UID 8B]

    // 解析 15693 单片响应(去掉帧层, 输入为帧 data 区)
    RfidResponse parseRfidResponse(const QByteArray &data) const;

    // 15693 结果码转文字
    static QString rfidResultToString(quint8 result);

    // 15693 子命令转文字
    static QString rfidSubCmdToString(quint8 subCmd);

    // ============================ 14443A (FC=0x14) ============================

    // 通用: 组 14443A 请求帧 (data = [subCmd] + params, FC=0x14)
    QByteArray makeRfid14443AFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params = QByteArray()) const;

    // 便捷组帧(各子命令, 见 Test_Frames.md §3)
    QByteArray makeRfid14443AGetUid(quint8 devAddr) const;                                                       // [0x00]
    QByteArray makeRfid14443AHalt(quint8 devAddr) const;                                                        // [0x01]
    QByteArray makeRfid14443AAuthM1(quint8 devAddr, const QByteArray &uid, quint8 mode,
                                    const QByteArray &key, quint8 addr) const;                                  // [0x02][UID 4B][mode][key 6B][addr]
    // 以下均为"一键"模式 (Auth 前缀命令): uid 始终在帧 4B, 全 0 设备内部 GetUid, 非 0 直接 AuthM1
    QByteArray makeRfid14443AAuthReadBlockM1(quint8 devAddr, quint8 blockAddr, quint8 mode,
                                             const QByteArray &key, const QByteArray &uid) const;               // [0x30][blockAddr][mode][key 6B][uid 4B]
    QByteArray makeRfid14443AAuthWriteBlockM1(quint8 devAddr, quint8 blockAddr, quint8 mode,
                                              const QByteArray &key, const QByteArray &uid,
                                              const QByteArray &data16) const;                                 // [0x31][blockAddr][mode][key 6B][uid 4B][data 16B]
    QByteArray makeRfid14443AAuthInc(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                     const QByteArray &key, const QByteArray &uid,
                                     const QByteArray &value4) const;                                           // [0x32][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B]
    QByteArray makeRfid14443AAuthDec(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                     const QByteArray &key, const QByteArray &uid,
                                     const QByteArray &value4) const;                                           // [0x33][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B]
    QByteArray makeRfid14443AAuthTransfer(quint8 devAddr, quint8 transAddr, quint8 mode,
                                           const QByteArray &key, const QByteArray &uid) const;                  // [0x34][transAddr][mode][key 6B][uid 4B]
    QByteArray makeRfid14443AAuthRestore(quint8 devAddr, quint8 valueAddr, quint8 transAddr, quint8 mode,
                                         const QByteArray &key, const QByteArray &uid,
                                         const QByteArray &value4) const;                                       // [0x35][valueAddr][transAddr][mode][key 6B][uid 4B][value 4B](卡端忽略)
    // M0 (Ultralight) 无认证读写
    QByteArray makeRfid14443AReadBlockM0(quint8 devAddr, quint8 blockAddr) const;                                // [0x21][blockAddr] 读 16B (4 页连读)
    QByteArray makeRfid14443AWriteBlockM0(quint8 devAddr, quint8 blockAddr, const QByteArray &data4) const;      // [0x20][blockAddr][data 4B] 写 4B 页
    QByteArray makeRfid14443ARats(quint8 devAddr) const;                                                         // [0x10]
    QByteArray makeRfid14443ATransApdu(quint8 devAddr, const QByteArray &apdu) const;                            // [0x11][apdu]
    QByteArray makeRfid14443ATopazRead(quint8 devAddr, quint8 addr) const;                                       // [0x12][addr]
    QByteArray makeRfid14443ATopazWrite(quint8 devAddr, quint8 addr, const QByteArray &data8) const;             // [0x13][addr][data 8B]

    // 解析 14443A 单片响应 (格式与 15693 一致: [subCmd][seq][total][payload], 复用 RfidResponse)
    RfidResponse parseRfid14443AResponse(const QByteArray &data) const;

    // 14443A 结果码转文字
    static QString rfid14443AResultToString(quint8 result);

    // 14443A 子命令转文字
    static QString rfid14443ASubCmdToString(quint8 subCmd);

    // ============================ 射频控制 (FC=0x16) ============================

    // 通用: 组 FC=0x16 请求帧 (data = [subCmd] + params, FC=0x16)
    QByteArray makeRfidCtrlFrame(quint8 devAddr, quint8 subCmd, const QByteArray &params = QByteArray()) const;

    // 便捷组帧(各子命令)
    QByteArray makeRfidCtrlInit(quint8 devAddr, quint8 proto) const;    // [0x00][proto]
    QByteArray makeRfidCtrlClose(quint8 devAddr) const;                // [0x01]
    QByteArray makeRfidCtrlOpen(quint8 devAddr) const;                 // [0x02]
    QByteArray makeRfidCtrlDelay(quint8 devAddr, quint8 ms) const;     // [0x03][ms]

    // 解析 FC=0x16 同步响应: data = [subCmd][0][1][result], 返回 result (0xFF 表示解析失败)
    //   响应格式固定 4B, seq=0 total=1
    quint8 parseRfidCtrlResponse(const QByteArray &data, quint8 expectSubCmd) const;

    // 射频控制子命令转文字
    static QString rfidCtrlSubCmdToString(quint8 subCmd);

signals:
    void logMessage(const QString &msg, bool isTx = false, const QByteArray &rawData = QByteArray());

private:
    quint32 updateCrc32(quint32 crc, const quint8 *data, int len) const;
};
