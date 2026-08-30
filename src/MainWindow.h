#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QByteArray>
#include <QSpinBox>
#include <QPointer>
#include <QTabWidget>
#include <QGroupBox>
#include <QTableWidget>
#include <QTimer>

class ITransport;
class SerialPortManager;
class ProtocolParser;
struct FrameData;   // 前置声明 (定义于 ProtocolParser.h), 供 handleZlrPushFrame 信号参数用
#ifdef COMFORTOOL_ENABLE_USB
#include "HidDeviceInfo.h"  // HidDeviceInfo 定义 (枚举结果)
#endif

// 通信接口类型
enum class TransportType { Com, Usb };

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // Public for static helper functions
    bool waitForResponse(int timeoutMs);
    void appendLog(const QString &prefix, const QByteArray &data, const QColor &color, const QString &note = QString());
    void appendSystemLog(const QString &msg, const QColor &color = QColor("#8870a8"));
    QByteArray &getRxBuffer() { return m_rxBuffer; }
    ProtocolParser *getParser() const { return m_parser; }
    ITransport *transport() const { return m_transport; }
    TransportType transportType() const { return m_transportType; }

    friend int waitBootHandshake(MainWindow *mw, ITransport *tp, quint8 &outDevAddr, quint8 &outStatus, quint32 &outDeviceUidHash);
    friend bool enterBoot(MainWindow *mw, ITransport *tp, quint8 devAddr);
    friend bool sendFirmwareChunk(MainWindow *mw, ITransport *tp, ProtocolParser *parser,
                                 quint8 devAddr, quint16 seq, int offset,
                                 const QByteArray &fwData, int chunkSize);
    friend bool checkUpgradeDoneAfterExec(MainWindow *mw, ITransport *tp);

private slots:
    void onOpen();
    void onFuncHandshake();
    void refreshPorts();
    void onSearchBtn();                // Search 按钮: 刷新 HID 列表, 存在 VID 0x5377 则自动连接
    void sendData();
    void onReadyRead();
    void onPortError();
    void onHandshakeResponse();
    void onSaveLog();
    void onUpdateFirmware();
    void onUpgradeResponse();
    void onUpgradeTimeout();
    void onDebugToggle();
    void onDebugPack();
    void onIfaceChanged();
    void onTransportOpened();
    void onTransportClosed();
    void onPathLabelClicked();         // 点击路径 Label 弹 HID 设备选择菜单
    void updatePathLabel();             // 根据 m_hidDevices/m_selectedHudIdx 刷新 m_pathLabel 显示
    void onSccdToggle(bool checked);   // 切换 Sccd 区域显示/隐藏 + 按钮底色
    void onZlrToggle(bool checked);    // 切换 ZLR5401 区域显示/隐藏 + 按钮底色
    void on14443AInventory();          // 14443A: 盘点单标签 → makeRfidInventory → 填 UID
    void on14443AReadBlock();          // 14443A: 读块
    void on14443AWriteBlock();         // 14443A: 写块
    void on14443AHalt();               // 14443A: Halt (0x01)
    void on14443ARats();              // 14443A: RATS (0x10)
    void on14443ATransApdu();         // 14443A: TransAPDU (0x11)
    void on14443ATopazRead();         // 14443A: Topaz Read (0x12)
    void on14443ATopazWrite();        // 14443A: Topaz Write (0x13)
    void on14443AWalletInit();        // 14443A: 钱包初始化 (写值块0 + Inc 0x0A + 读验证)
    void on14443AWalletGet();         // 14443A: 获取余额 (读值块前4B → 十进制填余额)
    void on14443AWalletInc();         // 14443A: 钱包增值 (0x32)
    void on14443AWalletDec();         // 14443A: 钱包减值 (0x33)
    void rfid14443AValueOp(quint8 subCmd, const QString &name);  // 0x32/0x33 公共流程
    bool walletKeyUid(quint8 &authMode, QByteArray &keyBytes, QByteArray &uidBytes);  // 取选中 Key + uid
    void update14443AWalletBalance(const QByteArray &block16);  // 读回值块前4B LE → 十进制填余额
    void on15693Inventory();           // 15693: 盘点
    void on15693ReadBlock();           // 15693: 读块
    void on15693WriteBlock();          // 15693: 写块
    void on15693LockBlock();           // 15693: 锁块 (0x05)
    void on15693StayQuiet();           // 15693: 静默 (0x06)
    void on15693Dsfid();               // 15693: DSFID 读/写/锁 (0x03)
    void on15693Eas();                 // 15693: EAS 置位/复位/锁定 (0x04)
    void on15693WriteAfi();            // 15693: 写 AFI (0x07)
    void on15693LockAfi();             // 15693: 锁 AFI (0x08)
    bool uid15693Bytes(QByteArray &uidBytes);  // 15693 UID 输入框取 8B uid
    void onFuncEnterBoot();            // FC=0x02 进 Boot 升级模式
    void onFuncReset();                // FC=0x08 软件复位
    void onRfOpen();                   // FC=0x16 开启射频 (按当前 Tab 协议 INIT+DELAY+OPEN)
    void onRfClose();                  // FC=0x16 关闭射频
    void onRfReset();                  // FC=0x16 复位射频 (CLOSE+INIT+OPEN)
    void on14443AAuthKeyA();           // 14443A: KeyA 选中 (互斥)
    void on14443AAuthKeyB();           // 14443A: KeyB 选中 (互斥)

    // ===== ZLR5401: 电机控制 (FC=0x0A) =====
    void onMotorMove(int dir);             // 公共 MOVE 流程 (dir 0=正 1=反)
    void onMotorMoveCw();              // 正转 MOVE dir=0
    void onMotorMoveCcw();             // 反转 MOVE dir=1
    void onMotorStop();                // STOP
    void onMotorSpeed();               // SPEED (只发速度)
    void onMotorTorque();              // TORQUE (只发转矩)
    void onMotorQuery();               // QUERY (读状态/故障/步数)
    void onMotorClear();               // CLEAR (清故障)
    void onMotorTest();                // TEST: 电机行程测试 (passes 次往返)
    void onMotorHealth();              // HEALTH: 读健康/堵转监测
    void onMotorStats();               // STATS: 读运行统计

    // ===== ZLR5401: UHF (FC=0x0B) =====
    void onUhfOpen();                  // OPEN
    void onUhfClose();                 // CLOSE
    void onUhfInventory();             // INVENTORY
    void onUhfQuery();                 // QUERY
    void onUhfGetTags();               // GET_TAGS
    void onUhfSetConfig();             // SET_CONFIG
    void onUhfGetConfig();             // GET_CONFIG
    void onUhfGetStatus();             // GET_STATUS (状态/错误/回波)
    void onUhfCheckAnt();              // CHECK_ANT (主动回波检测)
    void onUhfReadTag();               // READ_TAG
    void onUhfWriteTag();              // WRITE_TAG
    void onUhfSetScan();               // SCAN_START (自动扫描, cycle)
    void onUhfScanStop();              // SCAN_STOP (停止自动扫描)
    void onUhfGetDump();               // GET_DUMP (诊断)

    // ===== ZLR5401: AM (FC=0x0C) =====
    void onAmGetConfig();              // GET_CONFIG
    void onAmSetConfig();              // SET_CONFIG
    void onAmQuery();                  // QUERY
    void onAmGetStatus();              // GET_STATUS (监控: link + 事件累计 + 最近事件ms)
    void onAmSetMode();                // SET_MODE (仅切工作模式)
    void onAmGetWave();                // GET_WAVE (波形采集, 阻塞~1s)
    void onAmGetWavePage();            // GET_WAVE_PAGE (取一页波形)

    // ===== ZLR5401: 开锁器 (FC=0x0D) =====
    void onLockerConfigure();          // CONFIGURE
    void onLockerAdd();                // ADD
    void onLockerStart();              // START
    void onLockerCancel();             // CANCEL
    void onLockerQuery();              // QUERY
    void onLockerGetEvent();           // GET_EVENT

    // ===== ZLR5401: RGB (FC=0x0E) =====
    void onRgbSet();                   // SET: 按勾选组合下发 mask
    void onRgbClear();                 // 全灭 mask=0x00 + 清勾选

    // ===== ZLR5401: 自检 (FC=0x0F) =====
    void onSelfTestQuery();            // QUERY: 读锁存错误位 + 实时诊断快照
    void onSelfTestRerun();            // RERUN: 重探外设 (阻塞~3s)
    void onSelfTestClear();            // CLEAR: 按掩码清指定位

private:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void setupUI();
    void applyStylesheet();
    void updateConnectionState(bool isConnected);
    void enableFuncButtons(bool enable);
    void resetUiToInitialState();   // 断开(含 USB 拔出)后: UI 复位到初始状态
    void startUpgrade(const QString &binPath);
    void flashControls(bool disable);
    void endUpgrade(bool success);
    void setTransportSignals(ITransport *tp);
    void applyTransportVisibility();
    bool sendRfidAndParse(quint8 fc, quint8 subCmd, const QByteArray &req,
                           QByteArray *outPayload, QString *outErrNote);
    bool sendFcAndCheckResult(quint8 fc, const QString &note, int timeoutMs = 3000);
    bool sendRfidCtrl(quint8 subCmd, const QByteArray &req, QString *outErrNote);  // FC=0x16 同步收发
    // ZLR5401 通用子命令收发: 打包 subCmd+argData → 发 → 校验 func^0xFF + data[0]==subCmd
    //   成功返回 true, outPayload=data.mid(2) (去掉 cmd+err), outErrNote 填错误说明
    //   timeoutMs 默认 3000; 长命令如 SELFTEST RERUN(~3s) 可传 8000
    bool sendZlrSubCmd(quint8 fc, quint8 subCmd, const QByteArray &argData,
                       QByteArray *outPayload, QString *outErrNote, int timeoutMs = 3000);
    bool rfOpenWithProto(quint8 proto);   // 按协议 INIT+DELAY+OPEN 时序开启射频
    bool rfEnsureOpen14443A();            // 14443A 操作前确保射频已开 (未开则开启)
    bool rfEnsureOpen15693();             // 15693 操作前确保射频已开 (未开则开启)
    void enableRfFuncButtons(bool enable);  // 启用/禁用 Sccd 内卡操作按钮 (射频开启后启用)
    void update14443AButtonsByCardType();   // 按当前卡类型(M1/M0)启用/禁用 14443A 操作按钮

    SerialPortManager *m_serialManager;
    ITransport *m_hidManager;     // USB HID 传输实例 (平台门面: macOS HidManagerMac / Windows HidManagerWin), 仅用 ITransport 接口
    ITransport *m_transport;          // 当前激活传输
    TransportType m_transportType;
    ProtocolParser *m_parser;

    QComboBox *m_ifaceCombo;          // 端口: USB(默认) / COM(暂不启用)
    QLabel *m_pathLabel;              // 路径(原通信接口): VID:PID 显示, 点击弹菜单
    QPushButton *m_searchBtn;         // Search 按钮: 刷新 HID 列表 + 自动连接 VID 0x5377
    QPushButton *m_openBtn;           // 连接/断开按钮(动态样式)
    QList<HidDeviceInfo> m_hidDevices; // 当前 HID 设备列表
    int m_selectedHudIdx = 0;         // 路径菜单选中索引
    QPushButton *m_refreshBtn = nullptr; // 刷新按钮 (Handshake Area 里)

    QComboBox *m_baudRateCombo = nullptr;  // (已废弃, COM 暂不启用)
    QComboBox *m_dataBitsCombo = nullptr;  // (已废弃)
    QComboBox *m_stopBitsCombo = nullptr;  // (已废弃)
    QComboBox *m_parityCombo = nullptr;    // (已废弃)
    QLabel *m_statusLabel = nullptr;            // (已废弃)
    QLabel *m_versionLabel = nullptr;           // (已废弃)

    QPushButton *m_debugBtn;
    QPushButton *m_updateBtn;
    QPushButton *m_sccdBtn;           // FUN 区切换 Sccd Area 的开关
    QPushButton *m_funcHandshakeBtn;  // 右列旧"握手"按钮 (保留, 默认隐藏, 顶层 6 个 FC 按钮替代)
    QLabel *m_funcStatusLabel;
    QLineEdit *m_verAddrEdit;        // 设备地址 (0x--)
    QLineEdit *m_verSwEdit;          // 软件版本 (SW:...)
    QLineEdit *m_verHwEdit;          // 硬件版本 (HW:...)

    // App Protocol 顶层 FC 按钮 (独立按钮, 与通信调试/更新固件同级)
    //   握手按钮单独放在右列版本信息下方 (m_funcHandshakeBtn)
    //   14443A / 15693 不在此处, 由 Sccd Area 内 Tab 承载
    QPushButton *m_protoEnterBootBtn = nullptr;   // FC=0x02 进 Boot
    QPushButton *m_protoResetBtn     = nullptr;   // FC=0x08 复位

    // Sccd 区域
    QGroupBox *m_sccdBox = nullptr;        // Sccd Area 主框 (独立 QGroupBox, 与 Function Area 同级)
    QPushButton *m_sccdBtn15693 = nullptr;  // 切到 15693 tab
    QPushButton *m_sccdBtn14443A = nullptr; // 切到 14443A tab
    QPushButton *m_sccdBtn14443B = nullptr; // 切到 14443B tab
    QTabWidget *m_sccdTabs = nullptr;       // 三个协议 Tab

    // ZLR5401 区域 (Sccd 下方扩展, 参照 Sccd 交互)
    QPushButton *m_zlrBtn = nullptr;         // Supported Devices 区: ZLR5401 开关 (可勾选)
    QGroupBox   *m_zlrBox = nullptr;         // ZLR5401 Area 主框 (默认隐藏)
    QTabWidget  *m_zlrTabs = nullptr;        // 电机 / UHF / AM / 开锁器 四个 Tab
    // 电机 (FC=0x0A)
    QSpinBox *m_zlrMotorAngle  = nullptr;    // 角度(°), 0=持续运行
    QSpinBox *m_zlrMotorSteps  = nullptr;    // 每转微步数 (默认 3200)
    QSpinBox *m_zlrMotorSpeed  = nullptr;    // 微步/秒 (1~2000)
    QSpinBox *m_zlrMotorTorque = nullptr;    // 转矩% (6~100)
    QTextEdit *m_zlrMotorOut   = nullptr;    // 电机输出区
    QPushButton *m_zlrMotorCwBtn = nullptr;  // 正转
    QPushButton *m_zlrMotorCcwBtn = nullptr; // 反转
    QPushButton *m_zlrMotorStopBtn = nullptr;
    QPushButton *m_zlrMotorClearBtn = nullptr;
    QPushButton *m_zlrMotorSpeedBtn = nullptr;
    QPushButton *m_zlrMotorTorqueBtn = nullptr;
    QSpinBox *m_zlrMotorTestPasses = nullptr;  // 行程测试往返次数
    QPushButton *m_zlrMotorTestBtn = nullptr;  // 电机行程测试
    QPushButton *m_zlrMotorHealthBtn = nullptr; // HEALTH (健康/堵转)
    QPushButton *m_zlrMotorStatsBtn = nullptr;  // STATS (运行统计)
    QPushButton *m_zlrMotorStateBtn = nullptr;  // 获取状态 (QUERY, 位于"其他"区)
    // UHF (FC=0x0B)
    QSpinBox *m_zlrUhfPower    = nullptr;    // 功率 dBm (5~30)
    QLineEdit *m_zlrUhfEpc      = nullptr;   // EPC (读/写标签定位)
    QTextEdit *m_zlrUhfOut      = nullptr;   // UHF 输出区
    QComboBox *m_zlrUhfBand     = nullptr;   // 频段 Region (0x01/06/08/FF)
    QSpinBox *m_zlrUhfBank      = nullptr;   // 读/写标签 memory bank
    QSpinBox *m_zlrUhfAddr      = nullptr;   // 字地址
    QSpinBox *m_zlrUhfCnt       = nullptr;   // 读的字数 / 写字节数
    QLineEdit *m_zlrUhfData     = nullptr;   // 写标签数据 (hex)
    QPushButton *m_zlrUhfOpenBtn = nullptr;
    QPushButton *m_zlrUhfCloseBtn = nullptr;
    QPushButton *m_zlrUhfInvBtn = nullptr;
    QPushButton *m_zlrUhfQueryBtn = nullptr;
    QPushButton *m_zlrUhfGetTagsBtn = nullptr;
    QPushButton *m_zlrUhfSetCfgBtn = nullptr;
    QPushButton *m_zlrUhfGetCfgBtn = nullptr;
    QPushButton *m_zlrUhfStatusBtn = nullptr;   // GET_STATUS
    QPushButton *m_zlrUhfAntBtn = nullptr;      // CHECK_ANT
    QPushButton *m_zlrUhfReadBtn = nullptr;     // READ_TAG
    QPushButton *m_zlrUhfWriteBtn = nullptr;    // WRITE_TAG
    QSpinBox *m_zlrUhfCycle     = nullptr;      // SCAN_START cycle(ms) 每轮盘存超时
    QPushButton *m_zlrUhfScanBtn = nullptr;     // SCAN_START 自动扫描
    QPushButton *m_zlrUhfScanStopBtn = nullptr; // SCAN_STOP
    QPushButton *m_zlrUhfDumpBtn = nullptr;     // GET_DUMP 诊断
    QTableWidget *m_zlrUhfTagTable = nullptr;   // 标签表格 (EPC 双击填入输入框)
    // AM (FC=0x0C)
    QLineEdit *m_zlrAmThr  = nullptr; QLineEdit *m_zlrAmHit = nullptr; QLineEdit *m_zlrAmFreq = nullptr;
    QLineEdit *m_zlrAmDelay = nullptr; QLineEdit *m_zlrAmLen = nullptr; QLineEdit *m_zlrAmInvert = nullptr;
    QLineEdit *m_zlrAmSync = nullptr; QLineEdit *m_zlrAmVolt = nullptr; QLineEdit *m_zlrAmMode = nullptr;
    QLineEdit *m_zlrAmMains = nullptr;         // 市电频率 0=50Hz 1=60Hz
    QTextEdit *m_zlrAmOut = nullptr;         // AM 输出区
    QPushButton *m_zlrAmGetBtn = nullptr;
    QPushButton *m_zlrAmSetBtn = nullptr;
    QPushButton *m_zlrAmQueryBtn = nullptr;
    QPushButton *m_zlrAmStatusBtn = nullptr;   // GET_STATUS (监控)
    QPushButton *m_zlrAmSetModeBtn = nullptr;  // SET_MODE (切工作模式)
    QPushButton *m_zlrAmWaveBtn = nullptr;     // GET_WAVE (波形采集)
    QSpinBox *m_zlrAmWavePage = nullptr;       // GET_WAVE_PAGE 页码
    QPushButton *m_zlrAmWavePageBtn = nullptr; // 取一页波形
    QTextEdit *m_zlrAmWaveOut = nullptr;       // AM 波形输出
    // 开锁器 (FC=0x0D)
    QSpinBox *m_zlrLockerSoftCnt = nullptr;  // 软标总数 N
    QLineEdit *m_zlrLockerHardEpc = nullptr; // 追加硬标签 EPC (hex)
    QTextEdit *m_zlrLockerOut = nullptr;     // 开锁器输出区
    QPushButton *m_zlrLockerCfgBtn = nullptr;
    QPushButton *m_zlrLockerAddBtn = nullptr;
    QPushButton *m_zlrLockerStartBtn = nullptr;
    QPushButton *m_zlrLockerCancelBtn = nullptr;
    QPushButton *m_zlrLockerQueryBtn = nullptr;
    QPushButton *m_zlrLockerEvtBtn = nullptr;
    // RGB (FC=0x0E)
    QCheckBox *m_zlrRgbG = nullptr;            // 绿 使能 (bit0)
    QCheckBox *m_zlrRgbR = nullptr;            // 红 使能 (bit1)
    QCheckBox *m_zlrRgbB = nullptr;            // 蓝 使能 (bit2)
    QPushButton *m_zlrRgbSetBtn = nullptr;     // 下发 RGB
    QPushButton *m_zlrRgbClearBtn = nullptr;   // 全灭
    QLabel *m_zlrRgbOutLabel = nullptr;        // 上次 mask 回显
    // 自检 (FC=0x0F)
    QLabel *m_zlrSelfErrBitsLabel = nullptr;   // 锁存错误位 16bit (二/十六进制)
    QLabel *m_zlrSelfDiagLabel = nullptr;      // 实时诊断快照 (motorCommOk/drvFault/uhfLink/amLink/paramCrc/switchErr)
    QPushButton *m_zlrSelfQueryBtn = nullptr;  // QUERY
    QPushButton *m_zlrSelfRerunBtn = nullptr;  // RERUN (阻塞~3s)
    QSpinBox *m_zlrSelfClearMask = nullptr;    // CLEAR 掩码 (16bit)
    QPushButton *m_zlrSelfClearBtn = nullptr;  // CLEAR 触发
    QTextEdit *m_zlrSelfOut = nullptr;         // 自检输出区


    // 15693 Tab 控件
    QPushButton *m_15693InventoryBtn = nullptr;
    QPushButton *m_15693ReadBtn      = nullptr;
    QPushButton *m_15693WriteBtn     = nullptr;
    QPushButton *m_15693LockBtn      = nullptr;
    QPushButton *m_15693StayQuietBtn = nullptr;
    QPushButton *m_15693DsfidBtn     = nullptr;
    QPushButton *m_15693EasBtn       = nullptr;
    QPushButton *m_15693WriteAfiBtn  = nullptr;
    QPushButton *m_15693LockAfiBtn   = nullptr;
    QLineEdit   *m_15693UidEdit       = nullptr;
    QLineEdit   *m_15693AddrEdit      = nullptr;  // 块地址 (2 hex, 默认 0x00)
    QLineEdit   *m_15693CountEdit     = nullptr;  // 块数 (2 hex, 默认 01)
    QTextEdit   *m_15693InputArea     = nullptr;  // 输入区 (写块/DSFID写/AFI 用)
    QTextEdit   *m_15693OutputArea    = nullptr;  // 输出区 (读块/DSFID读 结果)

    // 14443A Tab 控件
    QLineEdit *m_14443AUidEdit   = nullptr;   // UID 输入区 (只读)
    QLineEdit *m_14443ACardTypeEdit = nullptr; // 卡类型 (只读, 盘点时按 uidType 填入)
    QLineEdit *m_14443AAddrEdit  = nullptr;   // 块地址 (2字符)
    QLineEdit *m_14443AAddrCountEdit = nullptr; // 块数目 (2字符)
    QLineEdit *m_14443AKeyAEdit  = nullptr;   // KeyA 输入框 (12 hex 字符 = 6B)
    QLineEdit *m_14443AKeyBEdit = nullptr;   // KeyB 输入框 (12 hex 字符 = 6B)
    QPushButton *m_14443AKeyABtn = nullptr;   // KeyA 选中按钮 (互斥)
    QPushButton *m_14443AKeyBBtn = nullptr;   // KeyB 选中按钮 (互斥)
    QPushButton *m_14443AInventoryBtn = nullptr; // 盘点
    QPushButton *m_14443AReadBtn   = nullptr;
    QPushButton *m_14443AWriteBtn  = nullptr;
    QPushButton *m_14443AHaltBtn   = nullptr;  // Halt (0x01)
    QPushButton *m_14443ARatsBtn  = nullptr;  // RATS (0x10)
    QPushButton *m_14443ATransApduBtn = nullptr;  // TransAPDU (0x11)
    QPushButton *m_14443ATopazReadBtn = nullptr;  // Topaz Read (0x12)
    QPushButton *m_14443ATopazWriteBtn = nullptr; // Topaz Write (0x13)
    // 钱包组 (M1 值块, 仅 M1 可用): 初始化/获取/增值/减值 + 余额只读框
    QPushButton *m_14443AWalletInitBtn = nullptr;  // 钱包初始化 (写值块0 + Inc 0x0A + 读验证)
    QPushButton *m_14443AWalletGetBtn  = nullptr;  // 获取余额 (读值块前4B → 十进制)
    QPushButton *m_14443AWalletIncBtn  = nullptr;  // 钱包增值 (0x32, value 取输入区)
    QPushButton *m_14443AWalletDecBtn  = nullptr;  // 钱包减值 (0x33, value 取输入区)
    QLineEdit   *m_14443AWalletBalEdit = nullptr;  // 余额 (只读, 6 字符居中)
    quint8       m_14443AWalletBlock = 2;           // 钱包所在块号 (默认块2)
    QTextEdit   *m_14443AInputArea  = nullptr;  // 输入区 (左 50% 中 40%)
    QTextEdit   *m_14443AOutputArea = nullptr;  // 输出区 (右 50% 中 40%)
    bool m_14443ACardIsM1 = false;  // 当前卡是否 Mifare Classic(M1): MF1 S50/S70. M0(Ultralight) 则禁用 M1 专属操作

    // 射频控制按钮 (协议标签右侧)
    QPushButton *m_rfOpenBtn  = nullptr;   // 开启射频
    QPushButton *m_rfCloseBtn = nullptr;   // 关闭射频
    QPushButton *m_rfResetBtn = nullptr;   // 复位射频

    // 14443B Tab 控件 (协议层尚未实现, 占位)

    QWidget *m_debugPanel;
    QLineEdit *m_debugAddrEdit;
    QLineEdit *m_debugFcEdit;
    QSpinBox *m_debugTimeoutSpin;
    QLineEdit *m_debugParamEdit;
    QPushButton *m_debugPackBtn;
    QLabel *m_debugFrameLabel;
    QPushButton *m_debugCloseBtn;

    QTextEdit *m_logDisplay;
    QCheckBox *m_logHexCheck;
    QPushButton *m_clearLogBtn;
    QPushButton *m_saveLogBtn;

    QTextEdit *m_sendEdit;
    QCheckBox *m_sendHexCheck;
    QPushButton *m_sendBtn;
    QCheckBox *m_timerSendCheck;
    QSpinBox *m_timerSendSpin;
    QTimer *m_timerSend;

    QProgressBar *m_logProgressBar; // 日志区内叠加进度条
    QPointer<QProgressDialog> m_waitDialog; // 升级等待遮罩(等待设备重启期间)

    quint8 m_deviceAddr = 0;
    bool m_funcUnlocked = false;
    bool m_upgrading = false;         // 升级流程进行中: close transport 不清空日志区(保留升级期 [TX]/[RX])
    QByteArray m_rxBuffer;
    QTimer *m_replyTimer = nullptr;
    QSpinBox *m_frameGapSpin;  // 帧间间隔(ms)
};

// Static helpers - declared after class
int waitBootHandshake(MainWindow *mw, ITransport *tp, quint8 &outDevAddr, quint8 &outStatus, quint32 &outDeviceUidHash);
bool enterBoot(MainWindow *mw, ITransport *tp, quint8 devAddr);
bool sendFirmwareChunk(MainWindow *mw, ITransport *tp, ProtocolParser *parser,
                       quint8 devAddr, quint16 seq, int offset,
                       const QByteArray &fwData, int chunkSize);
bool checkUpgradeDoneAfterExec(MainWindow *mw, ITransport *tp);
