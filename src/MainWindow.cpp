#ifdef COMFORTOOL_ENABLE_USB
#include "HidManager.h"  // 平台 HID 门面 (macOS=HidManagerMac / Windows=HidManagerWin), 必须先于 MainWindow.h
#endif
#include "MainWindow.h"
#include "SerialPortManager.h"
#include "ITransport.h"
#include "ProtocolParser.h"

// Qt 6.11+ QString::arg(T,int,int,QChar) 不再隐式接受 unsigned/uint8，统一转 int
static QString hex2(int v) { return QString("%1").arg(v, 2, 16, QChar('0')); }
#include "Version.h"
#include <QSerialPortInfo>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTime>
#include <QTimer>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <QTextStream>
#ifdef Q_OS_MACOS
#include <unistd.h>
#endif
#include <QFileInfo>
#include <QProgressBar>
#include <QProgressDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QCoreApplication>
#include <QtAlgorithms>
#include <QMenu>
#include <QMouseEvent>
#include <QStatusBar>
#ifdef COMFORTOOL_ENABLE_USB
#include "HidDeviceInfo.h"  // HidDeviceInfo
#endif
#include <QFrame>

// ============================ Constructor ============================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_serialManager(new SerialPortManager(this))
#ifdef COMFORTOOL_ENABLE_USB
    , m_hidManager(new HidManager(this))
#else
    , m_hidManager(nullptr)
#endif
    , m_transport(nullptr)
    , m_transportType(TransportType::Usb) // UI 端口固定 USB(COM 暂不启用), 须与下拉一致否则 onOpen/refreshPorts 误走 COM 分支
    , m_parser(new ProtocolParser(this))
    , m_replyTimer(new QTimer(this))
    , m_timerSend(new QTimer(this))
{
    setupUI();
    applyStylesheet();
    refreshPorts();
    updateConnectionState(false);
    enableFuncButtons(false);

    setWindowIcon(QIcon(":/icon.png"));

    // 串口/USB 都监听 ITransport 信号; 仅对当前激活的 transport 响应
    setTransportSignals(m_serialManager);
    if (m_hidManager) {
        setTransportSignals(m_hidManager);
        // HID 原始包不再直接打印到日志(由 SYS/Send 区主动显示完整 [SYS][TX/RX] 一行)
    }

    connect(m_replyTimer, &QTimer::timeout, this, &MainWindow::onUpgradeTimeout);

    // 定时发送
    connect(m_timerSendCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_transport && m_transport->isOpen()) {
            m_timerSend->start(m_timerSendSpin->value());
        } else {
            m_timerSend->stop();
        }
    });
    connect(m_timerSendSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val) {
        if (m_timerSend->isActive())
            m_timerSend->setInterval(val);
    });
    connect(m_timerSend, &QTimer::timeout, this, &MainWindow::sendData);
}

MainWindow::~MainWindow() = default;

// ============================ Transport Helpers ============================

void MainWindow::setTransportSignals(ITransport *tp)
{
    if (!tp) return;
    // 不连 rawReceived/dataReceived: 显示完全由 sendData/onHandshakeResponse 主动控制
    //   系统按钮: appendLog("[SYS][TX/RX]", ...)
    //   发送区按钮: appendLog("[TX/RX]", ...)
    connect(tp, &ITransport::portError, this, [this, tp]() {
        if (tp == m_transport) onPortError();
    });
    connect(tp, &ITransport::opened, this, [this, tp]() {
        if (tp == m_transport) onTransportOpened();
    });
    connect(tp, &ITransport::closed, this, [this, tp]() {
        if (tp == m_transport) onTransportClosed();
    });
}

void MainWindow::onTransportOpened()
{
    updateConnectionState(true);
    statusBar()->showMessage(tr("已连接: %1").arg(m_transport ? m_transport->name() : QString()), 3000);
    enableFuncButtons(true);
    // 同步按钮状态
    m_openBtn->setText(tr("DisConnect"));
    m_openBtn->setProperty("connected", true);
    m_openBtn->style()->unpolish(m_openBtn);
    m_openBtn->style()->polish(m_openBtn);
}

void MainWindow::onTransportClosed()
{
    updateConnectionState(false);
    statusBar()->showMessage(tr("未连接"), 3000);
    enableFuncButtons(false);
    m_openBtn->setText(tr("Connect"));
    m_openBtn->setProperty("connected", false);
    m_openBtn->style()->unpolish(m_openBtn);
    m_openBtn->style()->polish(m_openBtn);
    // 升级流程中 close(EXEC 后软复位重连等)不清空日志区, 保留升级期 [TX]/[RX].
    // 非升级的断开(手动断开 / USB 拔出)才复位 UI.
    if (m_upgrading) return;
    resetUiToInitialState();
}

void MainWindow::onIfaceChanged()
{
    // 切换接口时关闭当前传输并刷新设备列表
    if (m_transport && m_transport->isOpen())
        m_transport->close();
    m_transport = nullptr;
    refreshPorts();
    applyTransportVisibility();
    updateConnectionState(false);
    statusBar()->showMessage(tr("未连接"), 3000);
    enableFuncButtons(false);
}

void MainWindow::applyTransportVisibility()
{
    // 当前 UI 仅显示 USB 路径, COM 路径控件未创建. 此函数保留以兼容其他调用点.
    // 路径 Label 始终显示(USB 默认)
    if (m_pathLabel) m_pathLabel->setVisible(true);
}

// ============================ UI Setup ============================

void MainWindow::setupUI()
{
    setWindowTitle(tr("SwComForTool %1").arg(COMFORTOOL_VERSION));
    resize(1080, 720);   // Round029 D5: 调高默认窗口, 最大化时 LOG 区仍有足够空间

    auto *central = new QWidget;
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(4);

    // ===== Handshake Area =====
    auto *hsGroup = new QGroupBox(tr("Handshake Area"));
    auto *hsLayout = new QHBoxLayout(hsGroup);
    hsLayout->setContentsMargins(8, 4, 8, 4);

    // 1. 端口(原"接口"): QComboBox, 仅显示 USB(COM 暂不启用, 见 tooltip). 仅作显示, 收窄避免过宽.
    hsLayout->addWidget(new QLabel(tr("Port:")));
    m_ifaceCombo = new QComboBox;
    m_ifaceCombo->addItem(tr("USB"), static_cast<int>(TransportType::Usb));
    m_ifaceCombo->setCurrentIndex(0); // 默认且固定 USB
    // 用只读 lineEdit 承载显示, 使 "USB" 文字可在框内居中(QComboBox 闭态文字默认左对齐, 无法直接居中)
    m_ifaceCombo->setEditable(true);
    m_ifaceCombo->lineEdit()->setReadOnly(true);
    m_ifaceCombo->lineEdit()->setAlignment(Qt::AlignCenter);
    m_ifaceCombo->setEnabled(false);  // 固定不可切换
    m_ifaceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents); // 收窄到内容宽度
    m_ifaceCombo->setToolTip(tr("COM 串口暂不启用, 请使用 USB"));
    hsLayout->addWidget(m_ifaceCombo);

    hsLayout->addSpacing(8);

    // 2. 路径(原"通信接口"): QLabel 显示 VID:PID, 点击弹出菜单列 HID 设备
    hsLayout->addWidget(new QLabel(tr("Path:")));
    m_pathLabel = new QLabel(tr("VID 5377:PID 5378"));
    m_pathLabel->setFrameShape(QFrame::Box);
    m_pathLabel->setFrameShadow(QFrame::Raised);
    m_pathLabel->setMinimumWidth(180);
    m_pathLabel->setAlignment(Qt::AlignCenter);
    m_pathLabel->setStyleSheet(
        "QLabel { background: #f8f4ff; border: 1px solid #c8b8e0; border-radius: 3px;"
        " padding: 3px 8px; color: #503070; font-family: Menlo, monospace; }");
    m_pathLabel->setCursor(Qt::PointingHandCursor);
    m_pathLabel->setToolTip(tr("Click to select HID device"));
    m_pathLabel->installEventFilter(this);
    hsLayout->addWidget(m_pathLabel);

    // 2.5 Search 按钮: 刷新 HID 列表, 存在 VID 0x5377 则自动连接
    m_searchBtn = new QPushButton(tr("Search"));
    m_searchBtn->setObjectName("searchBtn");
    m_searchBtn->setMinimumWidth(80);
    m_searchBtn->setToolTip(tr("刷新 HID 设备列表, 存在 VID 0x5377 则自动连接"));
    connect(m_searchBtn, &QPushButton::clicked, this, &MainWindow::onSearchBtn);
    hsLayout->addWidget(m_searchBtn);

    // 3. 连接/断开按钮 (靠右, 默认紫色, 连接后绿色)
    hsLayout->addStretch();
    m_openBtn = new QPushButton(tr("Connect"));
    m_openBtn->setObjectName("openBtn");
    m_openBtn->setMinimumWidth(90);
    m_openBtn->setProperty("connected", false); // 用于动态样式
    connect(m_openBtn, &QPushButton::clicked, this, &MainWindow::onOpen);
    hsLayout->addWidget(m_openBtn);

    mainLayout->addWidget(hsGroup);

    // ===== 功能栏 (两列布局) =====
    auto *funcGroup = new QGroupBox(tr("Function Area"));
    auto *funcMainLayout = new QHBoxLayout(funcGroup);
    funcMainLayout->setContentsMargins(8, 4, 8, 4);

    // 左列: Tool 边框区 (通信调试/使能Boot/更新固件/复位/握手) + Supported Devices
    //   14443A / 15693 不在此处, 由 Sccd 按下后扩展的 Sccd Area 内 Tab 提供
    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(4);
    leftCol->setContentsMargins(0, 4, 0, 4);  // 上下留白

    // 两列网格: 左列容纳 Tool GroupBox (跨 4 行, 占 col0+col1), 右列放 Supported Devices 跨 4 行
    auto *btnGrid = new QGridLayout;
    btnGrid->setHorizontalSpacing(12);  // 两列之间留白
    btnGrid->setVerticalSpacing(6);
    btnGrid->setContentsMargins(0, 0, 0, 0);

    // ===== Tool 边框区 (5 个按钮: 通信调试/使能Boot/更新固件/复位/握手) =====
    auto *toolBox = new QGroupBox(tr("Tool"));
    toolBox->setObjectName("toolBox");
    auto *toolLayout = new QVBoxLayout(toolBox);
    toolLayout->setContentsMargins(8, 10, 8, 6);
    toolLayout->setSpacing(6);
    // 行1: 通信调试 / 使能Boot / 更新固件 / 复位 (等宽)
    auto *toolRow1 = new QHBoxLayout;
    toolRow1->setSpacing(6);
    m_debugBtn = new QPushButton(tr("通信调试"));
    m_debugBtn->setObjectName("debugBtn");
    connect(m_debugBtn, &QPushButton::clicked, this, &MainWindow::onDebugToggle);
    m_updateBtn = new QPushButton(tr("更新固件"));
    m_updateBtn->setObjectName("updateBtn");
    connect(m_updateBtn, &QPushButton::clicked, this, &MainWindow::onUpdateFirmware);
    m_protoEnterBootBtn = new QPushButton(tr("使能Boot"));
    m_protoEnterBootBtn->setObjectName("protoEnterBootBtn");
    connect(m_protoEnterBootBtn, &QPushButton::clicked, this, &MainWindow::onFuncEnterBoot);
    m_protoResetBtn = new QPushButton(tr("复位"));
    m_protoResetBtn->setObjectName("protoResetBtn");
    connect(m_protoResetBtn, &QPushButton::clicked, this, &MainWindow::onFuncReset);
    toolRow1->addWidget(m_debugBtn, 1);       // 等宽 stretch=1
    toolRow1->addWidget(m_protoEnterBootBtn, 1);
    toolRow1->addWidget(m_updateBtn, 1);
    toolRow1->addWidget(m_protoResetBtn, 1);
    toolLayout->addLayout(toolRow1);
    // 行2: HandShask (长度 = 2 × 其他按钮)
    auto *toolRow2 = new QHBoxLayout;
    toolRow2->setSpacing(6);
    m_funcHandshakeBtn = new QPushButton(tr("HandShask"));
    m_funcHandshakeBtn->setObjectName("funcHandshakeBtn");
    connect(m_funcHandshakeBtn, &QPushButton::clicked, this, &MainWindow::onFuncHandshake);
    m_funcHandshakeBtn->setProperty("handshaked", false);
    toolRow2->addWidget(m_funcHandshakeBtn, 2);  // stretch=2 = 行1 1 个按钮的 2 倍
    toolRow2->addStretch();                       // 右侧留白与行1 末按钮对齐
    toolLayout->addLayout(toolRow2);
    btnGrid->addWidget(toolBox, 0, 0, 4, 2);  // 跨 4 行 x 2 列

    // Supported Devices 边框 (跨网格四行, 第 3 列): Sccd (规格同握手)
    auto *supportedBox = new QGroupBox(tr("Supported Devices"));
    supportedBox->setObjectName("supportedDevicesBox");
    auto *supportedLayout = new QVBoxLayout(supportedBox);
    supportedLayout->setContentsMargins(6, 10, 6, 6);
    supportedLayout->setSpacing(6);
    m_sccdBtn = new QPushButton(tr("Sccd"));
    m_sccdBtn->setObjectName("sccdBtn");
    m_sccdBtn->setCheckable(true);
    m_sccdBtn->setToolTip(tr("切换 Sccd Area 显示/隐藏 (RFID 15693 / 14443A / 14443B)"));
    connect(m_sccdBtn, &QPushButton::toggled, this, &MainWindow::onSccdToggle);
    supportedLayout->addWidget(m_sccdBtn);
    // ZLR5401 按钮 (Sccd 下方, 参照 Sccd 交互: 可勾选 + toggle 展开 ZLR5401 Area)
    m_zlrBtn = new QPushButton(tr("ZLR5401"));
    m_zlrBtn->setObjectName("zlrBtn");
    m_zlrBtn->setCheckable(true);
    m_zlrBtn->setToolTip(tr("切换 ZLR5401 Area 显示/隐藏 (电机 / UHF / AM / 开锁器)"));
    connect(m_zlrBtn, &QPushButton::toggled, this, &MainWindow::onZlrToggle);
    supportedLayout->addWidget(m_zlrBtn);
    supportedLayout->addStretch();  // Sccd 下方留白 (占四行高度)
    btnGrid->addWidget(supportedBox, 0, 2, 4, 1);  // 跨 4 行, 第 3 列

    leftCol->addLayout(btnGrid);
    // 设备状态标签 (Tool 区下方)
    m_funcStatusLabel = new QLabel(tr("● 设备未连接"));
    m_funcStatusLabel->setStyleSheet("color: #b0a0c8; font-size: 0.82em;");
    m_funcStatusLabel->setWordWrap(true);
    leftCol->addWidget(m_funcStatusLabel);
    leftCol->addStretch();
    funcMainLayout->addLayout(leftCol, 0);

    // 左列与右列版本信息之间用 stretch 把版本信息推到右侧靠右
    funcMainLayout->addStretch(1);
    funcMainLayout->addSpacing(16);

    // 右列: 版本信息 (缩小至原 40% 宽度, 靠右)
    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(4);

    // 版本信息宽度缩小至原 40% (原 70/140 → 现 28/56), 设 MaximumWidth 限制最大宽度
    const int kVerAddrMinW = 28;   // 地址行最小宽度 (原 70 × 40%)
    const int kVerStrMinW  = 56;   // 软件/硬件行最小宽度 (原 140 × 40%)
    const int kVerBoxMaxW  = 180;  // verBox 最大宽度, 限制整体不超过此值

    auto *verBox = new QGroupBox(tr("Version"));
    verBox->setMaximumWidth(kVerBoxMaxW);   // 限制 GroupBox 最大宽度, 整体缩小靠右
    auto *verLayout = new QVBoxLayout(verBox);
    verLayout->setContentsMargins(6, 4, 6, 4);
    // 行间间距拉大, LineEdit 高度统一固定, 让 GroupBox 整体纵向拉高
    verLayout->setSpacing(6);

    const int kVerLineH = 32; // 每个 LineEdit 固定高度

    // 第1行: 地址 (stretch=0 -> 占自身高度, 避免与右列同区域其他控件争空间产生边框重叠)
    m_verAddrEdit = new QLineEdit;
    m_verAddrEdit->setAlignment(Qt::AlignCenter);
    m_verAddrEdit->setMinimumWidth(kVerAddrMinW);
    m_verAddrEdit->setMinimumHeight(kVerLineH);
    m_verAddrEdit->setMaximumHeight(kVerLineH);
    m_verAddrEdit->setPlaceholderText("0x--");
    m_verAddrEdit->setObjectName("verAddrEdit");
    verLayout->addWidget(m_verAddrEdit, 0);

    // 第2行: 软件版本
    m_verSwEdit = new QLineEdit;
    m_verSwEdit->setAlignment(Qt::AlignCenter);
    m_verSwEdit->setReadOnly(true);
    m_verSwEdit->setMinimumWidth(kVerStrMinW);
    m_verSwEdit->setMinimumHeight(kVerLineH);
    m_verSwEdit->setMaximumHeight(kVerLineH);
    m_verSwEdit->setPlaceholderText("SW:--");
    m_verSwEdit->setObjectName("verSwEdit");
    verLayout->addWidget(m_verSwEdit, 0);

    // 第3行: 硬件版本
    m_verHwEdit = new QLineEdit;
    m_verHwEdit->setAlignment(Qt::AlignCenter);
    m_verHwEdit->setReadOnly(true);
    m_verHwEdit->setMinimumWidth(kVerStrMinW);
    m_verHwEdit->setMinimumHeight(kVerLineH);
    m_verHwEdit->setMaximumHeight(kVerLineH);
    m_verHwEdit->setPlaceholderText("HW:--");
    m_verHwEdit->setObjectName("verHwEdit");
    verLayout->addWidget(m_verHwEdit, 0);

    rightCol->addWidget(verBox);
    rightCol->addStretch();
    // 右列 stretch=0, 不与左列争空间, 但 LineEdit 已有 MinimumWidth 保证不被压缩
    funcMainLayout->addLayout(rightCol, 0);

    funcGroup->setMinimumHeight(180);   // 拉高版本信息区域后, Fun 区最低高度需同步
    // SizePolicy: 竖向 Minimum 优先, 避免 Sccd 展开时被压缩至不可见
    funcGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    // 串口未打开时禁用握手和地址框
    m_funcHandshakeBtn->setEnabled(false);
    m_verAddrEdit->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_debugBtn->setEnabled(false);
    m_sccdBtn->setEnabled(false);
    if (m_zlrBtn) m_zlrBtn->setEnabled(false);
    // App Protocol 3 个 FC 按钮默认禁用, 握手成功后由 enableFuncButtons(true) + m_funcUnlocked 启用
    if (m_protoEnterBootBtn) m_protoEnterBootBtn->setEnabled(false);
    if (m_protoResetBtn)     m_protoResetBtn->setEnabled(false);
    // 射频控制按钮握手成功后启用
    if (m_rfOpenBtn)  m_rfOpenBtn->setEnabled(false);
    if (m_rfCloseBtn) m_rfCloseBtn->setEnabled(false);
    if (m_rfResetBtn) m_rfResetBtn->setEnabled(false);
    // Sccd 内卡操作按钮默认禁用, 射频开启后由 enableRfFuncButtons(true) 启用
    enableRfFuncButtons(false);

    mainLayout->addWidget(funcGroup);

    // ===== Sccd Area (Function Area 下方独立 QGroupBox, 默认隐藏, Sccd 按钮切换) =====
    m_sccdBox = new QGroupBox(tr("Sccd Area"));
    m_sccdBox->setObjectName("sccdBox");
    m_sccdBox->setVisible(false); // 默认隐藏, 由 Sccd 按钮控制展开/隐藏
    auto *sccdOuterLayout = new QVBoxLayout(m_sccdBox);
    sccdOuterLayout->setContentsMargins(4, 4, 4, 4);
    sccdOuterLayout->setSpacing(6);

    // 上行: 三个协议按钮 (15693 / 14443A / 14443B) - 切换 Tab
    auto *sccdBtnRow = new QHBoxLayout;
    sccdBtnRow->setSpacing(6);
    auto *sccdLabel = new QLabel(tr("协议:"));
    sccdLabel->setStyleSheet("font-size: 0.85em; color: #8870a8;");
    sccdBtnRow->addWidget(sccdLabel);

    m_sccdBtn15693 = new QPushButton(tr("15693"));
    m_sccdBtn15693->setObjectName("sccdBtn15693");
    m_sccdBtn15693->setCheckable(true);
    sccdBtnRow->addWidget(m_sccdBtn15693);

    m_sccdBtn14443A = new QPushButton(tr("14443A"));
    m_sccdBtn14443A->setObjectName("sccdBtn14443A");
    m_sccdBtn14443A->setCheckable(true);
    m_sccdBtn14443A->setChecked(true);   // 默认选中 14443A
    sccdBtnRow->addWidget(m_sccdBtn14443A);

    m_sccdBtn14443B = new QPushButton(tr("14443B"));
    m_sccdBtn14443B->setObjectName("sccdBtn14443B");
    m_sccdBtn14443B->setCheckable(true);
    sccdBtnRow->addWidget(m_sccdBtn14443B);

    sccdBtnRow->addStretch();

    // 射频控制按钮 (协议标签右侧): 开启射频 / 关闭射频 / 复位射频, 默认紫色
    m_rfOpenBtn = new QPushButton(tr("开启射频"));
    m_rfOpenBtn->setObjectName("rfOpenBtn");
    connect(m_rfOpenBtn, &QPushButton::clicked, this, &MainWindow::onRfOpen);
    sccdBtnRow->addWidget(m_rfOpenBtn);

    m_rfCloseBtn = new QPushButton(tr("关闭射频"));
    m_rfCloseBtn->setObjectName("rfCloseBtn");
    connect(m_rfCloseBtn, &QPushButton::clicked, this, &MainWindow::onRfClose);
    sccdBtnRow->addWidget(m_rfCloseBtn);

    m_rfResetBtn = new QPushButton(tr("复位射频"));
    m_rfResetBtn->setObjectName("rfResetBtn");
    connect(m_rfResetBtn, &QPushButton::clicked, this, &MainWindow::onRfReset);
    sccdBtnRow->addWidget(m_rfResetBtn);

    sccdOuterLayout->addLayout(sccdBtnRow);

    // 自动互斥 + 切 Tab: 用 toggle 信号
    auto selectSccdTab = [this](int idx) {
        m_sccdBtn15693->setChecked(idx == 0);
        m_sccdBtn14443A->setChecked(idx == 1);
        m_sccdBtn14443B->setChecked(idx == 2);
        m_sccdTabs->setCurrentIndex(idx);
    };
    connect(m_sccdBtn15693, &QPushButton::toggled, this, [selectSccdTab](bool checked) { if (checked) selectSccdTab(0); });
    connect(m_sccdBtn14443A, &QPushButton::toggled, this, [selectSccdTab](bool checked) { if (checked) selectSccdTab(1); });
    connect(m_sccdBtn14443B, &QPushButton::toggled, this, [selectSccdTab](bool checked) { if (checked) selectSccdTab(2); });

    // 下行: TabWidget (15693 / 14443A / 14443B) - 自定义按钮行已切换, 隐藏自带 TabBar 避免重复
    m_sccdTabs = new QTabWidget;
    m_sccdTabs->setObjectName("sccdTabs");
    m_sccdTabs->setTabPosition(QTabWidget::North);
    m_sccdTabs->setDocumentMode(true);
    m_sccdTabs->tabBar()->setVisible(false);  // 隐藏 TabBar, 切换由上方自定义按钮行负责

    // ----- 15693 Tab (参照 14443A 模式风格) -----
    auto *w15693 = new QWidget;
    auto *l15693 = new QVBoxLayout(w15693);
    l15693->setContentsMargins(8, 8, 8, 8);
    l15693->setSpacing(6);

    // 十六进制校验器 (Addr/Count 共用)
    static QRegularExpression hexRe15693("[0-9a-fA-F]*");
    auto *hexValidator15693 = new QRegularExpressionValidator(hexRe15693, this);

    // 第一行: UID(只读) + Inventory 按钮 — 整组居中
    auto *uid15693Row = new QHBoxLayout;
    uid15693Row->setSpacing(6);
    uid15693Row->addStretch();
    m_15693UidEdit = new QLineEdit;
    m_15693UidEdit->setReadOnly(true);
    m_15693UidEdit->setPlaceholderText("Uid");
    m_15693UidEdit->setAlignment(Qt::AlignCenter);
    m_15693UidEdit->setMaximumWidth(160);  // 15693 UID 8B 比 14443A 长, 放宽
    m_15693UidEdit->setObjectName("iso15693UidEdit");
    uid15693Row->addWidget(m_15693UidEdit);
    m_15693InventoryBtn = new QPushButton(tr("Inventory"));
    m_15693InventoryBtn->setObjectName("15693InventoryBtn");
    uid15693Row->addWidget(m_15693InventoryBtn);
    uid15693Row->addStretch();
    l15693->addLayout(uid15693Row);

    // 第二行: Addr + Count 输入 (居中独立一行, 与 14443A 第二行风格对齐)
    auto *addr15693Row = new QHBoxLayout;
    addr15693Row->setSpacing(6);
    addr15693Row->addStretch();
    auto *addr15693Label = new QLabel(tr("Addr:"));
    addr15693Row->addWidget(addr15693Label);
    m_15693AddrEdit = new QLineEdit;
    m_15693AddrEdit->setText("00");
    m_15693AddrEdit->setMaxLength(2);
    m_15693AddrEdit->setMaximumWidth(40);
    m_15693AddrEdit->setAlignment(Qt::AlignCenter);
    m_15693AddrEdit->setValidator(hexValidator15693);
    m_15693AddrEdit->setObjectName("15693AddrEdit");
    addr15693Row->addWidget(m_15693AddrEdit);
    addr15693Row->addSpacing(12);
    auto *count15693Label = new QLabel(tr("Count:"));
    addr15693Row->addWidget(count15693Label);
    m_15693CountEdit = new QLineEdit;
    m_15693CountEdit->setText("01");
    m_15693CountEdit->setMaxLength(2);
    m_15693CountEdit->setMaximumWidth(40);
    m_15693CountEdit->setAlignment(Qt::AlignCenter);
    m_15693CountEdit->setValidator(hexValidator15693);
    m_15693CountEdit->setObjectName("15693CountEdit");
    addr15693Row->addWidget(m_15693CountEdit);
    addr15693Row->addStretch();
    l15693->addLayout(addr15693Row);

    // 第三行: 8 个功能按钮 (Read/Write/LockBlock/StayQuiet + DSFID/EAS/WriteAFI/LockAFI)
    auto *btn15693Row = new QHBoxLayout;
    btn15693Row->setSpacing(6);
    m_15693ReadBtn      = new QPushButton(tr("Read"));
    m_15693ReadBtn->setObjectName("15693ReadBtn");
    m_15693WriteBtn     = new QPushButton(tr("Write"));
    m_15693WriteBtn->setObjectName("15693WriteBtn");
    m_15693LockBtn      = new QPushButton(tr("LockBlock"));
    m_15693LockBtn->setObjectName("15693LockBtn");
    m_15693StayQuietBtn = new QPushButton(tr("StayQuiet"));
    m_15693StayQuietBtn->setObjectName("15693StayQuietBtn");
    m_15693DsfidBtn     = new QPushButton(tr("DSFID"));
    m_15693DsfidBtn->setObjectName("15693DsfidBtn");
    m_15693EasBtn       = new QPushButton(tr("EAS"));
    m_15693EasBtn->setObjectName("15693EasBtn");
    m_15693WriteAfiBtn  = new QPushButton(tr("WriteAFI"));
    m_15693WriteAfiBtn->setObjectName("15693WriteAfiBtn");
    m_15693LockAfiBtn   = new QPushButton(tr("LockAFI"));
    m_15693LockAfiBtn->setObjectName("15693LockAfiBtn");
    btn15693Row->addWidget(m_15693ReadBtn);
    btn15693Row->addWidget(m_15693WriteBtn);
    btn15693Row->addWidget(m_15693LockBtn);
    btn15693Row->addWidget(m_15693StayQuietBtn);
    btn15693Row->addWidget(m_15693DsfidBtn);
    btn15693Row->addWidget(m_15693EasBtn);
    btn15693Row->addWidget(m_15693WriteAfiBtn);
    btn15693Row->addWidget(m_15693LockAfiBtn);
    btn15693Row->addStretch();
    l15693->addLayout(btn15693Row);

    // 第四行: 输入区(左50%) + 输出区(右50%) — 与 14443A IO 区风格一致
    auto *io15693Row = new QHBoxLayout;
    io15693Row->setSpacing(8);
    auto *input15693Box = new QGroupBox(tr("输入区"));
    input15693Box->setObjectName("15693InputBox");
    auto *input15693Layout = new QVBoxLayout(input15693Box);
    input15693Layout->setContentsMargins(4, 4, 4, 4);
    m_15693InputArea = new QTextEdit;
    m_15693InputArea->setObjectName("15693InputArea");
    m_15693InputArea->setFont(QFont("Menlo", 10));
    input15693Layout->addWidget(m_15693InputArea);
    io15693Row->addWidget(input15693Box, 1);
    auto *output15693Box = new QGroupBox(tr("输出区"));
    output15693Box->setObjectName("15693OutputBox");
    auto *output15693Layout = new QVBoxLayout(output15693Box);
    output15693Layout->setContentsMargins(4, 4, 4, 4);
    m_15693OutputArea = new QTextEdit;
    m_15693OutputArea->setObjectName("15693OutputArea");
    m_15693OutputArea->setReadOnly(true);
    m_15693OutputArea->setFont(QFont("Menlo", 10));
    output15693Layout->addWidget(m_15693OutputArea);
    io15693Row->addWidget(output15693Box, 1);
    l15693->addLayout(io15693Row, 1);

    m_sccdTabs->addTab(w15693, QString());

    // 15693 按钮信号
    connect(m_15693InventoryBtn, &QPushButton::clicked, this, &MainWindow::on15693Inventory);
    connect(m_15693ReadBtn,      &QPushButton::clicked, this, &MainWindow::on15693ReadBlock);
    connect(m_15693WriteBtn,     &QPushButton::clicked, this, &MainWindow::on15693WriteBlock);
    connect(m_15693LockBtn,      &QPushButton::clicked, this, &MainWindow::on15693LockBlock);
    connect(m_15693StayQuietBtn, &QPushButton::clicked, this, &MainWindow::on15693StayQuiet);
    connect(m_15693DsfidBtn,     &QPushButton::clicked, this, &MainWindow::on15693Dsfid);
    connect(m_15693EasBtn,       &QPushButton::clicked, this, &MainWindow::on15693Eas);
    connect(m_15693WriteAfiBtn,  &QPushButton::clicked, this, &MainWindow::on15693WriteAfi);
    connect(m_15693LockAfiBtn,   &QPushButton::clicked, this, &MainWindow::on15693LockAfi);

    // ----- 14443A Tab (无协议描述) -----
    auto *w14443A = new QWidget;
    auto *l14443A = new QVBoxLayout(w14443A);
    l14443A->setContentsMargins(8, 8, 8, 8);
    l14443A->setSpacing(6);

    // 第一行: [卡类型只读框(靠 UID 左侧)] [UID 输入框(只读)] [盘点按钮] — 整组居中
    auto *uid14443ARow = new QHBoxLayout;
    uid14443ARow->setSpacing(6);
    uid14443ARow->addStretch();
    // 卡类型 (只读, 盘点时按 uidType 填入卡型名)
    m_14443ACardTypeEdit = new QLineEdit;
    m_14443ACardTypeEdit->setReadOnly(true);
    m_14443ACardTypeEdit->setPlaceholderText(tr("CardType"));
    m_14443ACardTypeEdit->setAlignment(Qt::AlignCenter);
    m_14443ACardTypeEdit->setMaximumWidth(110);
    m_14443ACardTypeEdit->setObjectName("iso14443ACardTypeEdit");
    uid14443ARow->addWidget(m_14443ACardTypeEdit);
    m_14443AUidEdit = new QLineEdit;
    m_14443AUidEdit->setReadOnly(true);
    m_14443AUidEdit->setPlaceholderText("Uid");
    m_14443AUidEdit->setAlignment(Qt::AlignCenter);
    m_14443AUidEdit->setMaximumWidth(120);  // 约 20% 宽
    m_14443AUidEdit->setObjectName("iso14443AUidEdit");
    uid14443ARow->addWidget(m_14443AUidEdit);
    m_14443AInventoryBtn = new QPushButton(tr("Inventory"));
    m_14443AInventoryBtn->setObjectName("14443AInventoryBtn");
    uid14443ARow->addWidget(m_14443AInventoryBtn);
    uid14443ARow->addStretch();
    l14443A->addLayout(uid14443ARow);

    // 第二行: 左 KeyA 选中按钮 + KeyA 输入框(12 hex); 中 块地址(2 hex, 居中); 右 KeyB 选中按钮 + KeyB 输入框(12 hex)
    //   A/B 选中按钮互斥, 默认均显示 000000000000; 块地址默认 0x00; 三者均为十六进制输入
    auto *key14443ARow = new QHBoxLayout;
    key14443ARow->setSpacing(6);

    // 十六进制输入校验器 (KeyA/KeyB/块地址共用)
    static QRegularExpression hexRe("[0-9a-fA-F]*");
    auto *hexValidator = new QRegularExpressionValidator(hexRe, this);

    // KeyA (左)
    m_14443AKeyABtn = new QPushButton(tr("KeyA"));
    m_14443AKeyABtn->setObjectName("14443AKeyABtn");
    m_14443AKeyABtn->setCheckable(true);
    m_14443AKeyABtn->setChecked(true);  // 默认选中 KeyA
    connect(m_14443AKeyABtn, &QPushButton::clicked, this, &MainWindow::on14443AAuthKeyA);
    key14443ARow->addWidget(m_14443AKeyABtn);
    m_14443AKeyAEdit = new QLineEdit;
    m_14443AKeyAEdit->setText("FFFFFFFFFFFF");  // M1 默认 KeyA = FFFFFFFFFFFF (12 hex 字符 = 6B)
    m_14443AKeyAEdit->setMaxLength(12);
    m_14443AKeyAEdit->setMaximumWidth(110);
    m_14443AKeyAEdit->setAlignment(Qt::AlignCenter);
    m_14443AKeyAEdit->setValidator(hexValidator);  // 仅允许十六进制
    m_14443AKeyAEdit->setObjectName("14443AKeyAEdit");
    key14443ARow->addWidget(m_14443AKeyAEdit);

    // 中: 块地址 (居中于 KeyA / KeyB 之间, 默认 0x00, 2 hex 字符)
    key14443ARow->addStretch();
    auto *addrLabel = new QLabel(tr("块地址:"));
    addrLabel->setAlignment(Qt::AlignCenter);
    key14443ARow->addWidget(addrLabel);
    m_14443AAddrEdit = new QLineEdit;
    m_14443AAddrEdit->setText("00");          // 默认 0x00
    m_14443AAddrEdit->setMaxLength(2);       // 两个字符长度
    m_14443AAddrEdit->setMaximumWidth(40);
    m_14443AAddrEdit->setAlignment(Qt::AlignCenter);
    m_14443AAddrEdit->setValidator(hexValidator);  // 仅允许十六进制
    m_14443AAddrEdit->setObjectName("14443AAddrEdit");
    key14443ARow->addWidget(m_14443AAddrEdit);
    key14443ARow->addStretch();

    // KeyB (右)
    m_14443AKeyBBtn = new QPushButton(tr("KeyB"));
    m_14443AKeyBBtn->setObjectName("14443AKeyBBtn");
    m_14443AKeyBBtn->setCheckable(true);
    connect(m_14443AKeyBBtn, &QPushButton::clicked, this, &MainWindow::on14443AAuthKeyB);
    key14443ARow->addWidget(m_14443AKeyBBtn);
    m_14443AKeyBEdit = new QLineEdit;
    m_14443AKeyBEdit->setText("000000000000");
    m_14443AKeyBEdit->setMaxLength(12);
    m_14443AKeyBEdit->setMaximumWidth(110);
    m_14443AKeyBEdit->setAlignment(Qt::AlignCenter);
    m_14443AKeyBEdit->setValidator(hexValidator);  // 仅允许十六进制
    m_14443AKeyBEdit->setObjectName("14443AKeyBEdit");
    key14443ARow->addWidget(m_14443AKeyBEdit);

    l14443A->addLayout(key14443ARow);

    // 第三行: Read / Write / Halt / RATS / TransAPDU / TopazRead / TopazWrite (左中) ... 钱包组(右)
    auto *btn14443ARow = new QHBoxLayout;
    btn14443ARow->setSpacing(6);
    m_14443AReadBtn   = new QPushButton(tr("Read"));
    m_14443AReadBtn->setObjectName("14443AReadBtn");
    m_14443AWriteBtn  = new QPushButton(tr("Write"));
    m_14443AWriteBtn->setObjectName("14443AWriteBtn");
    m_14443AHaltBtn   = new QPushButton(tr("Halt"));
    m_14443AHaltBtn->setObjectName("14443AHaltBtn");
    m_14443ARatsBtn   = new QPushButton(tr("RATS"));
    m_14443ARatsBtn->setObjectName("14443ARatsBtn");
    m_14443ATransApduBtn = new QPushButton(tr("TransAPDU"));
    m_14443ATransApduBtn->setObjectName("14443ATransApduBtn");
    m_14443ATopazReadBtn  = new QPushButton(tr("TopazRead"));
    m_14443ATopazReadBtn->setObjectName("14443ATopazReadBtn");
    m_14443ATopazWriteBtn = new QPushButton(tr("TopazWrite"));
    m_14443ATopazWriteBtn->setObjectName("14443ATopazWriteBtn");
    btn14443ARow->addWidget(m_14443AReadBtn);
    btn14443ARow->addWidget(m_14443AWriteBtn);
    btn14443ARow->addWidget(m_14443AHaltBtn);
    btn14443ARow->addWidget(m_14443ARatsBtn);
    btn14443ARow->addWidget(m_14443ATransApduBtn);
    btn14443ARow->addWidget(m_14443ATopazReadBtn);
    btn14443ARow->addWidget(m_14443ATopazWriteBtn);
    btn14443ARow->addStretch();

    // 钱包组 (带边框 GroupBox, M1 值块操作): 余额只读框 + 初始化/获取/增值/减值
    auto *walletBox = new QGroupBox(tr("钱包"));
    walletBox->setObjectName("14443AWalletBox");
    auto *walletLayout = new QHBoxLayout(walletBox);
    walletLayout->setContentsMargins(6, 4, 6, 4);
    walletLayout->setSpacing(4);
    m_14443AWalletBalEdit = new QLineEdit;
    m_14443AWalletBalEdit->setReadOnly(true);
    m_14443AWalletBalEdit->setText("0");
    m_14443AWalletBalEdit->setMaxLength(6);
    m_14443AWalletBalEdit->setAlignment(Qt::AlignCenter);
    m_14443AWalletBalEdit->setMaximumWidth(60);
    m_14443AWalletBalEdit->setObjectName("14443AWalletBalEdit");
    walletLayout->addWidget(m_14443AWalletBalEdit);
    m_14443AWalletInitBtn = new QPushButton(tr("Init"));
    m_14443AWalletInitBtn->setObjectName("14443AWalletInitBtn");
    m_14443AWalletGetBtn  = new QPushButton(tr("Get"));
    m_14443AWalletGetBtn->setObjectName("14443AWalletGetBtn");
    m_14443AWalletIncBtn  = new QPushButton(tr("Inc"));
    m_14443AWalletIncBtn->setObjectName("14443AWalletIncBtn");
    m_14443AWalletDecBtn  = new QPushButton(tr("Dec"));
    m_14443AWalletDecBtn->setObjectName("14443AWalletDecBtn");
    walletLayout->addWidget(m_14443AWalletInitBtn);
    walletLayout->addWidget(m_14443AWalletGetBtn);
    walletLayout->addWidget(m_14443AWalletIncBtn);
    walletLayout->addWidget(m_14443AWalletDecBtn);
    btn14443ARow->addWidget(walletBox);

    l14443A->addLayout(btn14443ARow);

    // 第四行: 输入区 (左 50%, 中间 40% 带边框) + 输出区 (右 50%, 中间 40% 带边框)
    auto *io14443ARow = new QHBoxLayout;
    io14443ARow->setSpacing(8);

    // 输入区 (左 50%)
    auto *inputBox = new QGroupBox(tr("输入区"));
    inputBox->setObjectName("14443AInputBox");
    auto *inputLayout = new QVBoxLayout(inputBox);
    inputLayout->setContentsMargins(4, 4, 4, 4);
    m_14443AInputArea = new QTextEdit;
    m_14443AInputArea->setObjectName("14443AInputArea");
    m_14443AInputArea->setFont(QFont("Menlo", 10));
    inputLayout->addWidget(m_14443AInputArea);
    io14443ARow->addWidget(inputBox, 1);  // 50% (stretch=1, 与输出区均分)

    // 输出区 (右 50%)
    auto *outputBox = new QGroupBox(tr("输出区"));
    outputBox->setObjectName("14443AOutputBox");
    auto *outputLayout = new QVBoxLayout(outputBox);
    outputLayout->setContentsMargins(4, 4, 4, 4);
    m_14443AOutputArea = new QTextEdit;
    m_14443AOutputArea->setObjectName("14443AOutputArea");
    m_14443AOutputArea->setReadOnly(true);
    m_14443AOutputArea->setPlaceholderText(tr("读块结果 / 解析数据"));
    m_14443AOutputArea->setFont(QFont("Menlo", 10));
    outputLayout->addWidget(m_14443AOutputArea);
    io14443ARow->addWidget(outputBox, 1);  // 50%

    l14443A->addLayout(io14443ARow, 1);  // 输入/输出区拉伸占剩余空间

    m_sccdTabs->addTab(w14443A, QString());

    // 14443A 按钮信号
    connect(m_14443AInventoryBtn, &QPushButton::clicked, this, &MainWindow::on14443AInventory);
    connect(m_14443AReadBtn,   &QPushButton::clicked, this, &MainWindow::on14443AReadBlock);
    connect(m_14443AWriteBtn,  &QPushButton::clicked, this, &MainWindow::on14443AWriteBlock);
    connect(m_14443AHaltBtn,   &QPushButton::clicked, this, &MainWindow::on14443AHalt);
    connect(m_14443ARatsBtn,   &QPushButton::clicked, this, &MainWindow::on14443ARats);
    connect(m_14443ATransApduBtn, &QPushButton::clicked, this, &MainWindow::on14443ATransApdu);
    connect(m_14443ATopazReadBtn,  &QPushButton::clicked, this, &MainWindow::on14443ATopazRead);
    connect(m_14443ATopazWriteBtn, &QPushButton::clicked, this, &MainWindow::on14443ATopazWrite);
    connect(m_14443AWalletInitBtn, &QPushButton::clicked, this, &MainWindow::on14443AWalletInit);
    connect(m_14443AWalletGetBtn,  &QPushButton::clicked, this, &MainWindow::on14443AWalletGet);
    connect(m_14443AWalletIncBtn,  &QPushButton::clicked, this, &MainWindow::on14443AWalletInc);
    connect(m_14443AWalletDecBtn,  &QPushButton::clicked, this, &MainWindow::on14443AWalletDec);

    // ----- 14443B Tab (协议层未实现, 简要提示 + 占位) -----
    auto *w14443B = new QWidget;
    auto *l14443B = new QVBoxLayout(w14443B);
    l14443B->setContentsMargins(8, 8, 8, 8);
    l14443B->setSpacing(6);
    auto *bHint = new QLabel(tr("协议层尚未实现, 后续补齐"));
    bHint->setStyleSheet("color: #8870a8; font-size: 0.82em; font-style: italic;");
    bHint->setAlignment(Qt::AlignCenter);
    l14443B->addWidget(bHint);
    l14443B->addStretch();
    m_sccdTabs->addTab(w14443B, QString());

    // 默认选中 14443A (index=1, 与 m_sccdBtn14443A 默认选中一致)
    m_sccdTabs->setCurrentIndex(1);

    sccdOuterLayout->addWidget(m_sccdTabs);
    // Sccd 区用 Preferred 大小策略, 不抢 FUN / 其他区域高度
    m_sccdBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_sccdTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // 同步 Tab 切换 → 按钮 check (用户点 Tab 标题时也要同步)
    connect(m_sccdTabs, &QTabWidget::currentChanged, this, [selectSccdTab](int idx) { selectSccdTab(idx); });

    // Round029 v2: 取消 QScrollArea 包裹 (实测紧凑后无需滑动)
    m_sccdScroll = nullptr;   // 保留成员定义 (toggle 处直接操作 m_sccdBox)
    mainLayout->addWidget(m_sccdBox);

    // ===== ZLR5401 Area (Sccd 下方独立 QGroupBox, 默认隐藏, ZLR5401 按钮切换) =====
    m_zlrBox = new QGroupBox(tr("ZLR5401 Area"));
    m_zlrBox->setObjectName("zlrBox");
    m_zlrBox->setVisible(false); // 默认隐藏
    // Round029 优化建议①: ZLR 区右键菜单, 可恢复被精简区域 (调试/临时启用)
    m_zlrBox->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_zlrBox, &QGroupBox::customContextMenuRequested, this,
            [this](const QPoint &){ showZlrTrimRestoreMenu(); });
    auto *zlrOuterLayout = new QVBoxLayout(m_zlrBox);
    zlrOuterLayout->setContentsMargins(4, 4, 4, 4);   // Round029 v2 紧凑
    zlrOuterLayout->setSpacing(4);

    // ZLR5401 TabWidget (电机 / UHF / AM / 开锁器)
    m_zlrTabs = new QTabWidget;
    m_zlrTabs->setObjectName("zlrTabs");
    m_zlrTabs->setDocumentMode(true);
    // 紧凑 tab 栏 (避免与下方内容比例失调)
    m_zlrTabs->setStyleSheet("QTabBar::tab { padding: 4px 10px; }");

    // ---------- 电机 Tab (FC=0x20) - Round029 v4 重构: 主操作 2 行紧凑 + 子标签容纳溢出 ----
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(4);

        // ===== 行 1: 速度 + 转矩 一行紧凑 (单行 GroupBox 横向) =====
        {
            auto *spdBox = new QGroupBox(tr("速度"));
            spdBox->setObjectName("zlrMotorGroup");
            auto *spdL = new QHBoxLayout(spdBox);
            spdL->setContentsMargins(4, 4, 4, 4);
            spdL->setSpacing(4);
            spdL->addWidget(new QLabel(tr("速度:")));
            m_zlrMotorSpeed = new QSpinBox;
            m_zlrMotorSpeed->setRange(1, 2000); m_zlrMotorSpeed->setValue(1000);
            m_zlrMotorSpeed->setMaximumWidth(70); m_zlrMotorSpeed->setAlignment(Qt::AlignCenter);
            spdL->addWidget(m_zlrMotorSpeed);
            spdL->addWidget(new QLabel(tr("微步/转:")));
            m_zlrMotorSteps = new QSpinBox;
            m_zlrMotorSteps->setRange(1, 100000); m_zlrMotorSteps->setValue(400);
            m_zlrMotorSteps->setMaximumWidth(80); m_zlrMotorSteps->setAlignment(Qt::AlignCenter);
            m_zlrMotorSteps->setToolTip(tr("电机转一圈的微步数 (200步 x 细分数)"));
            spdL->addWidget(m_zlrMotorSteps);
            m_zlrMotorSpeedBtn = new QPushButton(tr("设速度"));
            m_zlrMotorSpeedBtn->setObjectName("zlrMotorBtn");
            m_zlrMotorSpeedBtn->setToolTip(tr("单独下发 SPEED 命令 (FC=0x20/0x03)"));
            connect(m_zlrMotorSpeedBtn, &QPushButton::clicked, this, &MainWindow::onMotorSpeed);
            spdL->addWidget(m_zlrMotorSpeedBtn);
            l->addWidget(spdBox);
        }

        // ===== 行 2: 转矩 + 控制 一行 (正反转 + 停止 + 转矩) =====
        {
            auto *ctlBox = new QGroupBox(tr("控制"));
            ctlBox->setObjectName("zlrMotorGroup");
            auto *ctlL = new QHBoxLayout(ctlBox);
            ctlL->setContentsMargins(4, 4, 4, 4);
            ctlL->setSpacing(4);
            ctlL->addWidget(new QLabel(tr("角度°:")));
            m_zlrMotorAngle = new QSpinBox;
            m_zlrMotorAngle->setRange(0, 360000); m_zlrMotorAngle->setValue(90);
            m_zlrMotorAngle->setMaximumWidth(80); m_zlrMotorAngle->setAlignment(Qt::AlignCenter);
            m_zlrMotorAngle->setToolTip(tr("电机旋转角度(0~360000°); 0 = 持续运行"));
            ctlL->addWidget(m_zlrMotorAngle);
            ctlL->addWidget(new QLabel(tr("转矩%:")));
            m_zlrMotorTorque = new QSpinBox;
            m_zlrMotorTorque->setRange(6, 100); m_zlrMotorTorque->setValue(50);
            m_zlrMotorTorque->setMaximumWidth(60); m_zlrMotorTorque->setAlignment(Qt::AlignCenter);
            ctlL->addWidget(m_zlrMotorTorque);
            m_zlrMotorTorqueBtn = new QPushButton(tr("设转矩"));
            m_zlrMotorTorqueBtn->setObjectName("zlrMotorBtn");
            m_zlrMotorTorqueBtn->setToolTip(tr("单独下发 TORQUE 命令 (FC=0x20/0x04)"));
            connect(m_zlrMotorTorqueBtn, &QPushButton::clicked, this, &MainWindow::onMotorTorque);
            ctlL->addWidget(m_zlrMotorTorqueBtn);
            m_zlrMotorCwBtn = new QPushButton(tr("正转"));
            m_zlrMotorCwBtn->setObjectName("zlrMotorMain");
            connect(m_zlrMotorCwBtn, &QPushButton::clicked, this, &MainWindow::onMotorMoveCw);
            ctlL->addWidget(m_zlrMotorCwBtn);
            m_zlrMotorStopBtn = new QPushButton(tr("停止"));
            m_zlrMotorStopBtn->setObjectName("zlrMotorStop");
            connect(m_zlrMotorStopBtn, &QPushButton::clicked, this, &MainWindow::onMotorStop);
            ctlL->addWidget(m_zlrMotorStopBtn);
            m_zlrMotorCcwBtn = new QPushButton(tr("反转"));
            m_zlrMotorCcwBtn->setObjectName("zlrMotorMain");
            connect(m_zlrMotorCcwBtn, &QPushButton::clicked, this, &MainWindow::onMotorMoveCcw);
            ctlL->addWidget(m_zlrMotorCcwBtn);
            l->addWidget(ctlBox);
        }

        // ===== 行 3: 输出区 =====
        {
            auto *outBox = new QGroupBox(tr("输出区"));
            outBox->setObjectName("zlrOutBox");
            auto *outL = new QVBoxLayout(outBox);
            outL->setContentsMargins(4, 4, 4, 4);
            m_zlrMotorOut = new QTextEdit;
            m_zlrMotorOut->setObjectName("zlrOut");
            m_zlrMotorOut->setReadOnly(true);
            m_zlrMotorOut->setFont(QFont("Menlo", 10));
            outL->addWidget(m_zlrMotorOut);
            l->addWidget(outBox, 1);
        }

        // ===== 子标签: 容纳溢出 (其他 = 获取/健康/统计/清除故障 / 测试 = 行程测试) =====
        {
            auto *sub = new QTabWidget;
            sub->setDocumentMode(true);
            sub->setStyleSheet("QTabBar::tab { padding: 3px 10px; }");
            // 子页 1: 其他 (获取/健康/统计/清除故障)
            auto *o = new QWidget;
            auto *oL = new QHBoxLayout(o);
            oL->setContentsMargins(4, 4, 4, 4); oL->setSpacing(4);
            m_zlrMotorStateBtn = new QPushButton(tr("获取"));
            m_zlrMotorStateBtn->setObjectName("zlrMotorBtn");
            m_zlrMotorStateBtn->setToolTip(tr("查询电机状态/故障/已走微步数 (QUERY)"));
            connect(m_zlrMotorStateBtn, &QPushButton::clicked, this, &MainWindow::onMotorQuery);
            oL->addWidget(m_zlrMotorStateBtn);
            m_zlrMotorHealthBtn = new QPushButton(tr("健康"));
            m_zlrMotorHealthBtn->setObjectName("zlrMotorBtn");
            m_zlrMotorHealthBtn->setToolTip(tr("读健康/堵转监测 (HEALTH)"));
            connect(m_zlrMotorHealthBtn, &QPushButton::clicked, this, &MainWindow::onMotorHealth);
            oL->addWidget(m_zlrMotorHealthBtn);
            m_zlrMotorStatsBtn = new QPushButton(tr("统计"));
            m_zlrMotorStatsBtn->setObjectName("zlrMotorBtn");
            m_zlrMotorStatsBtn->setToolTip(tr("读运行统计 (STATS)"));
            connect(m_zlrMotorStatsBtn, &QPushButton::clicked, this, &MainWindow::onMotorStats);
            oL->addWidget(m_zlrMotorStatsBtn);
            m_zlrMotorClearBtn = new QPushButton(tr("清除故障"));
            m_zlrMotorClearBtn->setObjectName("zlrMotorMain");
            connect(m_zlrMotorClearBtn, &QPushButton::clicked, this, &MainWindow::onMotorClear);
            oL->addWidget(m_zlrMotorClearBtn);
            oL->addStretch(1);
            sub->addTab(o, tr("其他"));
            // 子页 2: 测试 (行程测试)
            auto *t = new QWidget;
            auto *tL = new QHBoxLayout(t);
            tL->setContentsMargins(4, 4, 4, 4); tL->setSpacing(4);
            tL->addWidget(new QLabel(tr("次数:")));
            m_zlrMotorTestPasses = new QSpinBox;
            m_zlrMotorTestPasses->setRange(1, 100); m_zlrMotorTestPasses->setValue(1);
            m_zlrMotorTestPasses->setMaximumWidth(60);
            m_zlrMotorTestPasses->setToolTip(tr("行程测试往返次数"));
            tL->addWidget(m_zlrMotorTestPasses);
            m_zlrMotorTestBtn = new QPushButton(tr("行程测试"));
            m_zlrMotorTestBtn->setObjectName("zlrMotorMain");
            m_zlrMotorTestBtn->setToolTip(tr("最高速正转→触碰行程→反转→完成1次往返"));
            connect(m_zlrMotorTestBtn, &QPushButton::clicked, this, &MainWindow::onMotorTest);
            tL->addWidget(m_zlrMotorTestBtn);
            tL->addStretch(1);
            sub->addTab(t, tr("测试"));
            l->addWidget(sub);
        }

        m_zlrTabs->addTab(w, tr("电机"));
    }

    // ---------- RGB Tab (FC=0x24) - Round029 v4: 单行紧凑 (颜色+控制) + 输出 ----
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(4);

        // ===== 行 1: 颜色 + 控制 (横向紧凑单行) =====
        {
            auto *colorBox = new QGroupBox(tr("颜色"));
            colorBox->setObjectName("zlrMotorGroup");
            auto *colorL = new QHBoxLayout(colorBox);
            colorL->setContentsMargins(4, 4, 4, 4); colorL->setSpacing(6);
            m_zlrRgbG = new QCheckBox(tr("绿")); m_zlrRgbG->setObjectName("zlrRgbG");
            m_zlrRgbR = new QCheckBox(tr("红")); m_zlrRgbR->setObjectName("zlrRgbR");
            m_zlrRgbB = new QCheckBox(tr("蓝")); m_zlrRgbB->setObjectName("zlrRgbB");
            colorL->addWidget(m_zlrRgbG); colorL->addWidget(m_zlrRgbR); colorL->addWidget(m_zlrRgbB);
            auto *ctlBox = new QGroupBox(tr("控制"));
            ctlBox->setObjectName("zlrMotorGroup");
            auto *ctlL = new QHBoxLayout(ctlBox);
            ctlL->setContentsMargins(4, 4, 4, 4); ctlL->setSpacing(6);
            m_zlrRgbSetBtn = new QPushButton(tr("下发 RGB")); m_zlrRgbSetBtn->setObjectName("zlrAmMain");
            m_zlrRgbSetBtn->setToolTip(tr("按当前勾选组合下发 FC=0x24 SET"));
            connect(m_zlrRgbSetBtn, &QPushButton::clicked, this, &MainWindow::onRgbSet);
            ctlL->addWidget(m_zlrRgbSetBtn);
            m_zlrRgbClearBtn = new QPushButton(tr("全灭")); m_zlrRgbClearBtn->setObjectName("zlrUhfBtn");
            m_zlrRgbClearBtn->setToolTip(tr("下发 mask=0x00 全灭, 并清除勾选"));
            connect(m_zlrRgbClearBtn, &QPushButton::clicked, this, &MainWindow::onRgbClear);
            ctlL->addWidget(m_zlrRgbClearBtn);
            // 单行容器, 颜色 + 控制 横向并排
            auto *row = new QHBoxLayout;
            row->setSpacing(4);
            row->addWidget(colorBox, 1);
            row->addWidget(ctlBox, 1);
            l->addLayout(row);
        }

        // ===== 行 2: 上次 mask =====
        {
            auto *outBox = new QGroupBox(tr("状态"));
            outBox->setObjectName("zlrOutBox");
            auto *outL = new QHBoxLayout(outBox);
            outL->setContentsMargins(4, 4, 4, 4);
            m_zlrRgbOutLabel = new QLabel(tr("上次 mask: --"));
            m_zlrRgbOutLabel->setAlignment(Qt::AlignCenter);
            outL->addWidget(m_zlrRgbOutLabel);
            l->addWidget(outBox);
        }

        l->addStretch(1);
        m_zlrTabs->addTab(w, tr("RGB"));
    }

    // ---------- 自检 Tab (FC=0x25, 设备级自检/锁存错误位) ----------
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(6, 6, 6, 6);   // Round029 v2 紧凑
        l->setSpacing(6);

        // ===== 第一行: 错误位区 / 操作区 二等分 =====
        auto *row1 = new QHBoxLayout;
        row1->setSpacing(6);

        // ---- 错误位区 (边框"错误位"): 锁存位 + 实时诊断 ----
        auto *errBox = new QGroupBox(tr("错误位"));
        errBox->setObjectName("zlrMotorGroup");
        auto *errL = new QVBoxLayout(errBox);
        errL->setContentsMargins(4, 4, 4, 4);
        errL->setSpacing(4);
        // Round028: 自检状态表格 (Round027 原本用两个 QLabel, 现替换为 QTableWidget 2 列, 直接渲染错误位/诊断)
        m_zlrSelfTable = new QTableWidget(0, 3, this);
        m_zlrSelfTable->setObjectName("zlrSelfTable");
        m_zlrSelfTable->setHorizontalHeaderLabels({tr("字段"), tr("值"), tr("状态")});
        m_zlrSelfTable->verticalHeader()->setVisible(false);
        m_zlrSelfTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_zlrSelfTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_zlrSelfTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_zlrSelfTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_zlrSelfTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_zlrSelfTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_zlrSelfTable->setFont(QFont("Menlo", 10));
        errL->addWidget(m_zlrSelfTable);
        // 保留两个 QLabel 占位 (兼容老成员, 但不再使用)
        m_zlrSelfErrBitsLabel = new QLabel("");
        m_zlrSelfErrBitsLabel->setObjectName("zlrSelfErrBitsLabel");
        m_zlrSelfErrBitsLabel->setVisible(false);
        m_zlrSelfDiagLabel = new QLabel("");
        m_zlrSelfDiagLabel->setObjectName("zlrSelfDiagLabel");
        m_zlrSelfDiagLabel->setVisible(false);
        row1->addWidget(errBox, 2);

        // ---- 操作区 (边框"操作"): 查询/重探 + 清错误(掩码) ----
        auto *opBox = new QGroupBox(tr("操作"));
        opBox->setObjectName("zlrMotorGroup");
        auto *opL = new QVBoxLayout(opBox);
        opL->setContentsMargins(4, 4, 4, 4);
        opL->setSpacing(6);
        auto *qRow = new QHBoxLayout; qRow->setSpacing(6);
        m_zlrSelfQueryBtn = new QPushButton(tr("查询"));
        m_zlrSelfQueryBtn->setObjectName("zlrUhfBtn");
        m_zlrSelfQueryBtn->setToolTip(tr("QUERY: 读锁存错误位 + 实时诊断快照 (无阻塞)"));
        connect(m_zlrSelfQueryBtn, &QPushButton::clicked, this, &MainWindow::onSelfTestQuery);
        qRow->addWidget(m_zlrSelfQueryBtn, 1);
        m_zlrSelfRerunBtn = new QPushButton(tr("重探"));
        m_zlrSelfRerunBtn->setObjectName("zlrAmMain");
        m_zlrSelfRerunBtn->setToolTip(tr("RERUN: 重探外设并刷新锁存位 (阻塞~3s)"));
        connect(m_zlrSelfRerunBtn, &QPushButton::clicked, this, &MainWindow::onSelfTestRerun);
        qRow->addWidget(m_zlrSelfRerunBtn, 1);
        opL->addLayout(qRow);
        auto *cRow = new QHBoxLayout; cRow->setSpacing(6);
        cRow->addWidget(new QLabel(tr("掩码:")));
        m_zlrSelfClearMask = new QSpinBox;
        m_zlrSelfClearMask->setRange(0, 65535);
        m_zlrSelfClearMask->setValue(0xFFFF);   // 默认全清
        m_zlrSelfClearMask->setDisplayIntegerBase(16);
        m_zlrSelfClearMask->setToolTip(tr("CLEAR 掩码 16bit (bit0~bit5, 与错误位定义一致)"));
        cRow->addWidget(m_zlrSelfClearMask, 1);
        m_zlrSelfClearBtn = new QPushButton(tr("清错误"));
        m_zlrSelfClearBtn->setObjectName("zlrUhfBtn");
        m_zlrSelfClearBtn->setToolTip(tr("CLEAR: 按掩码清指定位, 响应回剩余位图"));
        connect(m_zlrSelfClearBtn, &QPushButton::clicked, this, &MainWindow::onSelfTestClear);
        cRow->addWidget(m_zlrSelfClearBtn, 1);
        opL->addLayout(cRow);
        row1->addWidget(opBox, 1);

        l->addLayout(row1);

        // Round028: 移除输出区, 全部 ERR 状态走表格直接渲染 (错红正绿)

        m_zlrTabs->addTab(w, tr("自检"));
    }

    // ---------- UHF Tab (FC=0x21) - Round029 v5: 配置/读写 隐藏, 仅盘点/读状态/天线检测 + 标签列表 + 输出区 ----
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(4);

        // ===== 行 1: 控制 (单行: 周期 + 盘点/读状态/天线检测) =====
        {
            auto *ctlBox = new QGroupBox(tr("控制"));
            ctlBox->setObjectName("zlrMotorGroup");
            auto *ctlL = new QHBoxLayout(ctlBox);
            ctlL->setContentsMargins(4, 4, 4, 4); ctlL->setSpacing(4);
            ctlL->addWidget(new QLabel(tr("周期ms:")));
            m_zlrUhfCycle = new QSpinBox;
            m_zlrUhfCycle->setRange(200, 60000); m_zlrUhfCycle->setValue(1000);
            m_zlrUhfCycle->setMaximumWidth(70);
            m_zlrUhfCycle->setToolTip(tr("SCAN_START/盘点 每轮盘存超时"));
            ctlL->addWidget(m_zlrUhfCycle);
            auto mkOpBtn = [](const QString &txt, const char *obj) { auto *b = new QPushButton(txt); b->setObjectName(obj); return b; };
            m_zlrUhfInvBtn = mkOpBtn(tr("盘点"), "zlrUhfMain");
            connect(m_zlrUhfInvBtn, &QPushButton::clicked, this, &MainWindow::onUhfInventory);
            // 其它按钮仍构造 (代码与连接全保留); 隐藏
            m_zlrUhfOpenBtn = mkOpBtn(tr("开启"), "zlrUhfMain");
            connect(m_zlrUhfOpenBtn, &QPushButton::clicked, this, &MainWindow::onUhfOpen);
            m_zlrUhfOpenBtn->setVisible(false);   // 设备 POST 自动上电
            m_zlrUhfCloseBtn = mkOpBtn(tr("关闭"), "zlrUhfBtn");
            connect(m_zlrUhfCloseBtn, &QPushButton::clicked, this, &MainWindow::onUhfClose);
            m_zlrUhfCloseBtn->setVisible(false);
            m_zlrUhfQueryBtn = mkOpBtn(tr("查询"), "zlrUhfBtn");
            connect(m_zlrUhfQueryBtn, &QPushButton::clicked, this, &MainWindow::onUhfQuery);
            m_zlrUhfQueryBtn->setVisible(false);
            m_zlrUhfGetTagsBtn = mkOpBtn(tr("取标签"), "zlrUhfBtn");
            connect(m_zlrUhfGetTagsBtn, &QPushButton::clicked, this, &MainWindow::onUhfGetTags);
            m_zlrUhfGetTagsBtn->setVisible(false);
            m_zlrUhfStatusBtn = mkOpBtn(tr("读状态"), "zlrUhfBtn");
            connect(m_zlrUhfStatusBtn, &QPushButton::clicked, this, &MainWindow::onUhfGetStatus);
            m_zlrUhfAntBtn = mkOpBtn(tr("天线检测"), "zlrUhfBtn");
            connect(m_zlrUhfAntBtn, &QPushButton::clicked, this, &MainWindow::onUhfCheckAnt);
            ctlL->addWidget(m_zlrUhfInvBtn);
            ctlL->addWidget(m_zlrUhfStatusBtn);
            ctlL->addWidget(m_zlrUhfAntBtn);
            ctlL->addStretch(1);
            l->addWidget(ctlBox);
        }

        // ===== 行 2: 标签列表 + 输出区 =====
        {
            auto *row = new QHBoxLayout;
            row->setSpacing(4);
            auto *tagBox = new QGroupBox(tr("标签列表"));
            tagBox->setObjectName("zlrTagBox");
            auto *tagL = new QVBoxLayout(tagBox);
            tagL->setContentsMargins(4, 4, 4, 4);
            m_zlrUhfTagTable = new QTableWidget(0, 3, this);
            m_zlrUhfTagTable->setObjectName("zlrTagTable");
            m_zlrUhfTagTable->setHorizontalHeaderLabels({tr("EPC"), tr("RSSI"), tr("对象")});
            m_zlrUhfTagTable->verticalHeader()->setVisible(false);
            m_zlrUhfTagTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_zlrUhfTagTable->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_zlrUhfTagTable->setSelectionMode(QAbstractItemView::SingleSelection);
            m_zlrUhfTagTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
            m_zlrUhfTagTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            m_zlrUhfTagTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
            m_zlrUhfTagTable->setFont(QFont("Menlo", 10));
            tagL->addWidget(m_zlrUhfTagTable);
            row->addWidget(tagBox, 3);
            auto *outBox = new QGroupBox(tr("输出区"));
            outBox->setObjectName("zlrOutBox");
            auto *outL = new QVBoxLayout(outBox);
            outL->setContentsMargins(4, 4, 4, 4);
            m_zlrUhfOut = new QTextEdit;
            m_zlrUhfOut->setObjectName("zlrOut");
            m_zlrUhfOut->setReadOnly(true);
            m_zlrUhfOut->setFont(QFont("Menlo", 10));
            outL->addWidget(m_zlrUhfOut);
            row->addWidget(outBox, 2);
            l->addLayout(row, 1);
        }

        // ===== 隐藏区代码保留 (右键恢复菜单使用) =====
        // 配置区 (功率/频段/EPC + 读写配置) - 全部隐藏
        {
            auto *cfgBox = new QGroupBox(tr("配置"));
            m_zlrUhfCfgBox = cfgBox;   // 右键恢复菜单用
            cfgBox->setObjectName("zlrMotorGroup");
            auto *cfgL = new QHBoxLayout(cfgBox);
            cfgL->setContentsMargins(4, 4, 4, 4); cfgL->setSpacing(4);
            cfgL->addWidget(new QLabel(tr("功率:")));
            m_zlrUhfPower = new QSpinBox;
            m_zlrUhfPower->setRange(5, 30); m_zlrUhfPower->setValue(20);
            m_zlrUhfPower->setMaximumWidth(60); m_zlrUhfPower->setAlignment(Qt::AlignCenter);
            cfgL->addWidget(m_zlrUhfPower);
            cfgL->addWidget(new QLabel(tr("频段:")));
            m_zlrUhfBand = new QComboBox;
            m_zlrUhfBand->addItem("北美", 0x01); m_zlrUhfBand->addItem("中国1", 0x06);
            m_zlrUhfBand->addItem("CE_LOW", 0x08); m_zlrUhfBand->addItem("全频段", 0xFF);
            m_zlrUhfBand->setMaximumWidth(90);
            cfgL->addWidget(m_zlrUhfBand);
            cfgL->addWidget(new QLabel(tr("EPC:")));
            m_zlrUhfEpc = new QLineEdit; m_zlrUhfEpc->setMaxLength(24);
            m_zlrUhfEpc->setPlaceholderText("hex"); m_zlrUhfEpc->setFont(QFont("Menlo", 10));
            m_zlrUhfEpc->setMaximumWidth(140);
            cfgL->addWidget(m_zlrUhfEpc);
            cfgBox->setVisible(false);
            // 配置区 Bank/Addr/Cnt/Data 控件已不在 cfgBox 内 (v4 重构移除了), 此处不构造
        }
        // 读写区 (读标签/写标签 - 已封禁)
        {
            auto *rwBox = new QGroupBox(tr("读写 - 已封禁"));
            m_zlrUhfRwBox = rwBox;
            rwBox->setObjectName("zlrMotorGroup");
            auto *rwL = new QHBoxLayout(rwBox);
            rwL->setContentsMargins(4, 4, 4, 4); rwL->setSpacing(6);
            m_zlrUhfReadBtn = new QPushButton(tr("读标签")); m_zlrUhfReadBtn->setObjectName("zlrUhfMain");
            connect(m_zlrUhfReadBtn, &QPushButton::clicked, this, &MainWindow::onUhfReadTag);
            m_zlrUhfWriteBtn = new QPushButton(tr("写标签")); m_zlrUhfWriteBtn->setObjectName("zlrUhfMain");
            connect(m_zlrUhfWriteBtn, &QPushButton::clicked, this, &MainWindow::onUhfWriteTag);
            rwL->addWidget(m_zlrUhfReadBtn);
            rwL->addWidget(m_zlrUhfWriteBtn);
            rwL->addStretch(1);
            rwBox->setVisible(false);
        }
        // 扫描区 (自动扫描/停扫描/诊断) - 保留但隐藏
        {
            auto *scanBox = new QGroupBox(tr("扫描"));
            m_zlrUhfScanBox = scanBox;
            scanBox->setObjectName("zlrMotorGroup");
            auto *scanL = new QHBoxLayout(scanBox);
            scanL->setContentsMargins(4, 4, 4, 4); scanL->setSpacing(4);
            m_zlrUhfScanBtn = new QPushButton(tr("自动扫描")); m_zlrUhfScanBtn->setObjectName("zlrUhfMain");
            m_zlrUhfScanBtn->setToolTip(tr("SCAN_START 连续盘点入缓冲"));
            connect(m_zlrUhfScanBtn, &QPushButton::clicked, this, &MainWindow::onUhfSetScan);
            m_zlrUhfScanStopBtn = new QPushButton(tr("停扫描")); m_zlrUhfScanStopBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrUhfScanStopBtn, &QPushButton::clicked, this, &MainWindow::onUhfScanStop);
            m_zlrUhfDumpBtn = new QPushButton(tr("诊断")); m_zlrUhfDumpBtn->setObjectName("zlrUhfBtn");
            m_zlrUhfDumpBtn->setToolTip(tr("GET_DUMP 原始字节诊断"));
            connect(m_zlrUhfDumpBtn, &QPushButton::clicked, this, &MainWindow::onUhfGetDump);
            scanL->addWidget(m_zlrUhfScanBtn);
            scanL->addWidget(m_zlrUhfScanStopBtn);
            scanL->addWidget(m_zlrUhfDumpBtn);
            scanL->addStretch(1);
            scanBox->setVisible(false);
        }

        m_zlrTabs->addTab(w, tr("UHF"));
    }

    // ---------- AM Tab (FC=0x22) - Round029 v4 重构: 主操作 2 行紧凑 + 子标签容纳溢出 ----
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(4);

        // 配置项 (10 项, 2 行 x 5 列对称网格)
        auto mkLine = [](const QString &label, const QString &def) {
            auto *box = new QWidget;
            auto *hb = new QHBoxLayout(box);
            hb->setContentsMargins(0, 0, 0, 0);
            hb->addWidget(new QLabel(label));
            auto *le = new QLineEdit(def);
            le->setMaximumWidth(44);
            le->setAlignment(Qt::AlignCenter);
            le->setFont(QFont("Menlo", 10));
            hb->addWidget(le);
            return QPair<QWidget*, QLineEdit*>{box, le};
        };
        auto t0 = mkLine("thr", "10");   m_zlrAmThr = t0.second;
        auto t1 = mkLine("hit", "5");    m_zlrAmHit = t1.second;
        auto t2 = mkLine("freq", "0");   m_zlrAmFreq = t2.second;
        auto t3 = mkLine("delay", "0");  m_zlrAmDelay = t3.second;
        auto t4 = mkLine("len", "0");    m_zlrAmLen = t4.second;
        auto t5 = mkLine("inv", "0");    m_zlrAmInvert = t5.second;
        auto t6 = mkLine("sync", "0");   m_zlrAmSync = t6.second;
        auto t7 = mkLine("volt", "1");   m_zlrAmVolt = t7.second;
        auto t8 = mkLine("mode", "0");   m_zlrAmMode = t8.second;
        auto t9 = mkLine("mains", "0");  m_zlrAmMains = t9.second;

        // ===== 行 1: 配置 (10 参数网格 + 读/写配置) =====
        {
            auto *cfgBox = new QGroupBox(tr("配置"));
            cfgBox->setObjectName("zlrMotorGroup");
            auto *cfgL = new QVBoxLayout(cfgBox);
            cfgL->setContentsMargins(4, 4, 4, 4);
            auto *cfgGrid = new QGridLayout;
            cfgGrid->setHorizontalSpacing(10);
            cfgGrid->setVerticalSpacing(4);
            cfgGrid->setContentsMargins(0, 0, 0, 0);
            cfgGrid->addWidget(t0.first, 0, 0); cfgGrid->addWidget(t1.first, 0, 1);
            cfgGrid->addWidget(t2.first, 0, 2); cfgGrid->addWidget(t3.first, 0, 3);
            cfgGrid->addWidget(t4.first, 0, 4);
            cfgGrid->addWidget(t5.first, 1, 0); cfgGrid->addWidget(t6.first, 1, 1);
            cfgGrid->addWidget(t7.first, 1, 2); cfgGrid->addWidget(t8.first, 1, 3);
            cfgGrid->addWidget(t9.first, 1, 4);
            cfgL->addLayout(cfgGrid);
            auto *cfgBtnRow = new QHBoxLayout; cfgBtnRow->setSpacing(6);
            m_zlrAmGetBtn = new QPushButton(tr("读配置")); m_zlrAmGetBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrAmGetBtn, &QPushButton::clicked, this, &MainWindow::onAmGetConfig);
            cfgBtnRow->addWidget(m_zlrAmGetBtn, 1);
            m_zlrAmSetBtn = new QPushButton(tr("写配置")); m_zlrAmSetBtn->setObjectName("zlrAmMain");
            connect(m_zlrAmSetBtn, &QPushButton::clicked, this, &MainWindow::onAmSetConfig);
            cfgBtnRow->addWidget(m_zlrAmSetBtn, 1);
            cfgL->addLayout(cfgBtnRow);
            l->addWidget(cfgBox);
        }

        // ===== 行 2: 输出区 =====
        {
            auto *outBox = new QGroupBox(tr("输出区"));
            m_zlrAmOutBox = outBox;
            outBox->setObjectName("zlrOutBox");
            auto *outL = new QVBoxLayout(outBox);
            outL->setContentsMargins(4, 4, 4, 4);
            m_zlrAmOut = new QTextEdit;
            m_zlrAmOut->setObjectName("zlrOut");
            m_zlrAmOut->setReadOnly(true);
            m_zlrAmOut->setFont(QFont("Menlo", 10));
            m_zlrAmOut->setFixedHeight(96);   // Round030: 输出区减半 (原 QTextEdit 默认高 ~192)
            outL->addWidget(m_zlrAmOut);
            l->addWidget(outBox, 1);
        }

        // ===== 子标签: 容纳溢出 (监控 / 波形) - Round030: 默认隐藏, 右键菜单恢复 =====
        {
            auto *sub = new QTabWidget;
            sub->setDocumentMode(true);
            sub->setStyleSheet("QTabBar::tab { padding: 3px 10px; }");
            // 子页: 监控 (查询/状态/切模式)
            auto *monPage = new QWidget;
            auto *monPageL = new QVBoxLayout(monPage);
            monPageL->setContentsMargins(4, 4, 4, 4); monPageL->setSpacing(4);
            auto *monBox = new QGroupBox(tr("监控"));
            monBox->setObjectName("zlrMotorGroup");
            auto *monL = new QVBoxLayout(monBox);
            monL->setContentsMargins(4, 4, 4, 4); monL->setSpacing(4);
            auto *m1 = new QHBoxLayout; m1->setSpacing(6);
            m_zlrAmQueryBtn = new QPushButton(tr("查询")); m_zlrAmQueryBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrAmQueryBtn, &QPushButton::clicked, this, &MainWindow::onAmQuery);
            m1->addWidget(m_zlrAmQueryBtn, 1);
            m_zlrAmStatusBtn = new QPushButton(tr("监控状态")); m_zlrAmStatusBtn->setObjectName("zlrUhfBtn");
            m_zlrAmStatusBtn->setToolTip(tr("AM 监控: 链路 + 事件累计 + 最近事件ms"));
            connect(m_zlrAmStatusBtn, &QPushButton::clicked, this, &MainWindow::onAmGetStatus);
            m1->addWidget(m_zlrAmStatusBtn, 1);
            m_zlrAmSetModeBtn = new QPushButton(tr("切模式")); m_zlrAmSetModeBtn->setObjectName("zlrUhfBtn");
            m_zlrAmSetModeBtn->setToolTip(tr("仅切工作模式 (mode 输入)"));
            connect(m_zlrAmSetModeBtn, &QPushButton::clicked, this, &MainWindow::onAmSetMode);
            m1->addWidget(m_zlrAmSetModeBtn, 1);
            monL->addLayout(m1);
            monPageL->addWidget(monBox);
            sub->addTab(monPage, tr("监控"));
            // 子页: 波形 (采集 / 取波形页 + 输出)
            auto *wavePage = new QWidget;
            auto *wavePageL = new QVBoxLayout(wavePage);
            wavePageL->setContentsMargins(4, 4, 4, 4); wavePageL->setSpacing(4);
            auto *waveCtrlBox = new QGroupBox(tr("控制"));
            waveCtrlBox->setObjectName("zlrMotorGroup");
            auto *wcL = new QHBoxLayout(waveCtrlBox);
            wcL->setContentsMargins(4, 4, 4, 4); wcL->setSpacing(6);
            m_zlrAmWaveBtn = new QPushButton(tr("波形采集")); m_zlrAmWaveBtn->setObjectName("zlrAmMain");
            m_zlrAmWaveBtn->setToolTip(tr("触发一次同步波形采集 (阻塞~1s), 400点/周期"));
            connect(m_zlrAmWaveBtn, &QPushButton::clicked, this, &MainWindow::onAmGetWave);
            wcL->addWidget(m_zlrAmWaveBtn, 1);
            wcL->addWidget(new QLabel(tr("页:")));
            m_zlrAmWavePage = new QSpinBox; m_zlrAmWavePage->setRange(0, 9); m_zlrAmWavePage->setValue(0);
            m_zlrAmWavePage->setMaximumWidth(60);
            m_zlrAmWavePage->setToolTip(tr("波形页 (48点/页, 400点≈9页)"));
            wcL->addWidget(m_zlrAmWavePage);
            m_zlrAmWavePageBtn = new QPushButton(tr("取波形页")); m_zlrAmWavePageBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrAmWavePageBtn, &QPushButton::clicked, this, &MainWindow::onAmGetWavePage);
            wcL->addWidget(m_zlrAmWavePageBtn);
            wavePageL->addWidget(waveCtrlBox);
            auto *waveBox = new QGroupBox(tr("波形输出"));
            waveBox->setObjectName("zlrOutBox");
            auto *waveL = new QVBoxLayout(waveBox);
            waveL->setContentsMargins(4, 4, 4, 4);
            m_zlrAmWaveOut = new QTextEdit;
            m_zlrAmWaveOut->setObjectName("zlrOut");
            m_zlrAmWaveOut->setReadOnly(true);
            m_zlrAmWaveOut->setFont(QFont("Menlo", 10));
            waveL->addWidget(m_zlrAmWaveOut);
            wavePageL->addWidget(waveBox, 1);
            sub->addTab(wavePage, tr("波形"));
            l->addWidget(sub);
            m_zlrAmSubTabs = sub;
            sub->setVisible(false);   // Round030: 默认隐藏 监控/波形 子标签, 缩短 AM Tab
        }

        m_zlrTabs->addTab(w, tr("AM"));
    }

    // ---------- 开锁器 Tab (FC=0x23) - Round029 v5: 清单+操作 隐藏, 仅 解锁 + 输出区 ----
    {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(4);

        // ===== 行 1: 解锁 (EPC + 两个时间窗口 + 软标数 + 按钮) - Round033: 窗口可修正, 0=协议缺省 =====
        {
            auto *unlockBox = new QGroupBox(tr("解锁"));
            unlockBox->setObjectName("zlrMotorGroup");
            auto *uBoxL = new QVBoxLayout(unlockBox);
            uBoxL->setContentsMargins(4, 4, 4, 4); uBoxL->setSpacing(4);
            // 行 1a: EPC + 解锁按钮
            auto *uL1 = new QHBoxLayout; uL1->setSpacing(4);
            uL1->addWidget(new QLabel(tr("EPC:")));
            m_zlrLockerUnlockEpc = new QLineEdit;
            m_zlrLockerUnlockEpc->setMaxLength(119);
            m_zlrLockerUnlockEpc->setPlaceholderText("hex 1~12B; 多标签(2~4张)以空格/逗号分隔");
            m_zlrLockerUnlockEpc->setFont(QFont("Menlo", 10));
            m_zlrLockerUnlockEpc->setToolTip(tr("期望标签 1~4 张; 1 张=单标, 多张=多标, 统一 UNLOCK_MULTI"));
            uL1->addWidget(m_zlrLockerUnlockEpc, 1);
            m_zlrLockerUnlockBtn = new QPushButton(tr("解锁"));
            m_zlrLockerUnlockBtn->setObjectName("zlrAmMain");
            m_zlrLockerUnlockBtn->setToolTip(tr("FC=0x23/0x0A: UNLOCK_MULTI 唯一开锁通道"));
            connect(m_zlrLockerUnlockBtn, &QPushButton::clicked, this, &MainWindow::onLockerOneShot);
            uL1->addWidget(m_zlrLockerUnlockBtn);
            uBoxL->addLayout(uL1);
            // 行 1b: EPC窗(tmo) + 软标数 — Round035: hold(解锁总窗)隐藏固定公式下发, tmo=EPC单次盘点时间 默认500ms
            auto *uL2 = new QHBoxLayout; uL2->setSpacing(4);
            uL2->addWidget(new QLabel(tr("EPC窗:")));
            m_zlrLockerTmo = new QSpinBox;
            m_zlrLockerTmo->setRange(0, 10000); m_zlrLockerTmo->setValue(500);
            m_zlrLockerTmo->setMaximumWidth(80); m_zlrLockerTmo->setAlignment(Qt::AlignCenter);
            m_zlrLockerTmo->setSuffix(" ms");
            m_zlrLockerTmo->setToolTip(tr("tmoMs EPC 单次盘点时限: 默认 500ms, 上限 10000ms"));
            uL2->addWidget(m_zlrLockerTmo);
            uL2->addWidget(new QLabel(tr("软标:")));
            m_zlrLockerDemagCnt = new QSpinBox;
            m_zlrLockerDemagCnt->setRange(0, 255); m_zlrLockerDemagCnt->setValue(0);
            m_zlrLockerDemagCnt->setMaximumWidth(60);
            m_zlrLockerDemagCnt->setToolTip(tr("软标消磁数 softCnt (0=跳过软标段; 每消一个推 0x0E 事件)"));
            uL2->addWidget(m_zlrLockerDemagCnt);
            uL2->addStretch(1);
            uBoxL->addLayout(uL2);
            l->addWidget(unlockBox);
        }

        // ===== 行 2: 输出区 =====
        {
            auto *outBox = new QGroupBox(tr("输出区"));
            outBox->setObjectName("zlrOutBox");
            auto *outL = new QVBoxLayout(outBox);
            outL->setContentsMargins(4, 4, 4, 4);
            m_zlrLockerOut = new QTextEdit;
            m_zlrLockerOut->setObjectName("zlrOut");
            m_zlrLockerOut->setReadOnly(true);
            m_zlrLockerOut->setFont(QFont("Menlo", 10));
            outL->addWidget(m_zlrLockerOut);
            l->addWidget(outBox, 1);
        }

        // ===== 隐藏区代码保留 (右键恢复菜单使用) =====
        // 操作区 (开始/取消/查询/取事件)
        {
            auto *opBox = new QGroupBox(tr("操作"));
            m_zlrLockerOpBox = opBox;   // 右键恢复菜单用
            opBox->setObjectName("zlrMotorGroup");
            auto *opL = new QHBoxLayout(opBox);
            opL->setContentsMargins(4, 4, 4, 4); opL->setSpacing(4);
            m_zlrLockerStartBtn = new QPushButton(tr("开始"));
            m_zlrLockerStartBtn->setObjectName("zlrAmMain");
            connect(m_zlrLockerStartBtn, &QPushButton::clicked, this, &MainWindow::onLockerStart);
            m_zlrLockerStartBtn->setEnabled(false);   // 清单区隐藏后失效
            m_zlrLockerStartBtn->setToolTip(tr("编排模式入口已隐藏 (CONFIGURE/ADD 在清单区), 解锁请走 UNLOCK_MULTI"));
            opL->addWidget(m_zlrLockerStartBtn);
            m_zlrLockerCancelBtn = new QPushButton(tr("取消"));
            m_zlrLockerCancelBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrLockerCancelBtn, &QPushButton::clicked, this, &MainWindow::onLockerCancel);
            opL->addWidget(m_zlrLockerCancelBtn);
            m_zlrLockerQueryBtn = new QPushButton(tr("查询"));
            m_zlrLockerQueryBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrLockerQueryBtn, &QPushButton::clicked, this, &MainWindow::onLockerQuery);
            opL->addWidget(m_zlrLockerQueryBtn);
            m_zlrLockerEvtBtn = new QPushButton(tr("取事件"));
            m_zlrLockerEvtBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrLockerEvtBtn, &QPushButton::clicked, this, &MainWindow::onLockerGetEvent);
            opL->addWidget(m_zlrLockerEvtBtn);
            opBox->setVisible(false);
        }
        // 清单区 (软标总数/硬标签EPC/配置/追加)
        {
            auto *lstBox = new QGroupBox(tr("清单"));
            m_zlrLockerLstBox = lstBox;   // 右键恢复菜单用
            lstBox->setObjectName("zlrMotorGroup");
            auto *lstL = new QVBoxLayout(lstBox);
            lstL->setContentsMargins(4, 4, 4, 4); lstL->setSpacing(4);
            auto *scRow = new QHBoxLayout; scRow->setSpacing(6);
            scRow->addWidget(new QLabel(tr("软标总数:")));
            m_zlrLockerSoftCnt = new QSpinBox; m_zlrLockerSoftCnt->setRange(0, 65535); m_zlrLockerSoftCnt->setValue(1);
            m_zlrLockerSoftCnt->setMaximumWidth(80);
            scRow->addWidget(m_zlrLockerSoftCnt);
            lstL->addLayout(scRow);
            auto *epcRow = new QHBoxLayout; epcRow->setSpacing(6);
            epcRow->addWidget(new QLabel(tr("硬标签EPC:")));
            m_zlrLockerHardEpc = new QLineEdit; m_zlrLockerHardEpc->setMaxLength(24);
            m_zlrLockerHardEpc->setPlaceholderText("hex"); m_zlrLockerHardEpc->setFont(QFont("Menlo", 10));
            epcRow->addWidget(m_zlrLockerHardEpc, 1);
            lstL->addLayout(epcRow);
            auto *lstBtnRow = new QHBoxLayout; lstBtnRow->setSpacing(6);
            m_zlrLockerCfgBtn = new QPushButton(tr("配置清单")); m_zlrLockerCfgBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrLockerCfgBtn, &QPushButton::clicked, this, &MainWindow::onLockerConfigure);
            lstBtnRow->addWidget(m_zlrLockerCfgBtn);
            m_zlrLockerAddBtn = new QPushButton(tr("追加硬标")); m_zlrLockerAddBtn->setObjectName("zlrUhfBtn");
            connect(m_zlrLockerAddBtn, &QPushButton::clicked, this, &MainWindow::onLockerAdd);
            lstBtnRow->addWidget(m_zlrLockerAddBtn);
            lstL->addLayout(lstBtnRow);
            lstBox->setVisible(false);
        }

        m_zlrTabs->addTab(w, tr("开锁器"));
    }

    // Round029 D5: 用 QScrollArea 包裹 (面板内容超高时内部滚动, 不再挤压 LOG 区)
    m_zlrScroll = new QScrollArea;
    m_zlrScroll = nullptr;   // Round029 v2: 不再 scroll 包裹 (实测紧凑渲染后整面板无滑动)
    // m_zlrScroll 声明仅满足成员定义 (为空指针), toggle 处保留兼容调用; 直接 add m_zlrBox
    zlrOuterLayout->addWidget(m_zlrTabs);
    m_zlrBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_zlrTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    mainLayout->addWidget(m_zlrBox);

    // 注: 自动扫描(SCAN_START)不主动上报, 标签持续入缓冲由 GET_TAGS 拉取, 无需异步推送轮询.

    // ===== 通信调试面板 (功能栏下方独立区域，默认隐藏) =====
    m_debugPanel = new QFrame;
    m_debugPanel->setObjectName("debugPanel");
    m_debugPanel->setVisible(false);
    auto *debugPanelLayout = new QVBoxLayout(m_debugPanel);
    debugPanelLayout->setContentsMargins(8, 4, 8, 4);
    debugPanelLayout->setSpacing(4);

    // 面板标题行
    auto *debugTitleRow = new QHBoxLayout;
    auto *debugTitle = new QLabel(tr("通信调试面板"));
    debugTitle->setStyleSheet("font-weight: bold; color: #50a050; font-size: 0.9em;");
    debugTitleRow->addWidget(debugTitle);
    debugTitleRow->addStretch();
    m_debugCloseBtn = new QPushButton(tr("✕ 关闭"));
    m_debugCloseBtn->setObjectName("debugCloseBtn");
    m_debugCloseBtn->setStyleSheet("font-size: 0.75em; padding: 2px 8px;");
    connect(m_debugCloseBtn, &QPushButton::clicked, this, [this]() { m_debugPanel->setVisible(false); });
    debugTitleRow->addWidget(m_debugCloseBtn);
    debugPanelLayout->addLayout(debugTitleRow);

    // 上行：地址、功能码、超时、打包按钮+帧展示
    auto *debugRow1 = new QHBoxLayout;
    debugRow1->setSpacing(6);
    debugRow1->addWidget(new QLabel(tr("地址:")));
    m_debugAddrEdit = new QLineEdit;
    m_debugAddrEdit->setPlaceholderText("0x01");
    m_debugAddrEdit->setMaximumWidth(60);
    m_debugAddrEdit->setAlignment(Qt::AlignCenter);
    debugRow1->addWidget(m_debugAddrEdit);
    debugRow1->addWidget(new QLabel(tr("功能码:")));
    m_debugFcEdit = new QLineEdit;
    m_debugFcEdit->setPlaceholderText("0x01");
    m_debugFcEdit->setMaximumWidth(60);
    m_debugFcEdit->setAlignment(Qt::AlignCenter);
    debugRow1->addWidget(m_debugFcEdit);
    debugRow1->addWidget(new QLabel(tr("超时:")));
    m_debugTimeoutSpin = new QSpinBox;
    m_debugTimeoutSpin->setRange(100, 60000);
    m_debugTimeoutSpin->setValue(3000);
    m_debugTimeoutSpin->setSuffix(" ms");
    m_debugTimeoutSpin->setMaximumWidth(100);
    debugRow1->addWidget(m_debugTimeoutSpin);
    m_debugPackBtn = new QPushButton(tr("打包"));
    m_debugPackBtn->setObjectName("debugPackBtn");
    connect(m_debugPackBtn, &QPushButton::clicked, this, &MainWindow::onDebugPack);
    debugRow1->addWidget(m_debugPackBtn);
    m_debugFrameLabel = new QLabel;
    m_debugFrameLabel->setStyleSheet("color: #50a050; font-family: Menlo, monospace; font-size: 0.8em;");
    debugRow1->addWidget(m_debugFrameLabel, 1);
    debugPanelLayout->addLayout(debugRow1);

    // 下行：参数输入
    auto *debugRow2 = new QHBoxLayout;
    debugRow2->setSpacing(6);
    debugRow2->addWidget(new QLabel(tr("参数 (Hex, 空格分隔):")));
    m_debugParamEdit = new QLineEdit;
    m_debugParamEdit->setPlaceholderText("可为空，如: 00 02 9B F0");
    debugRow2->addWidget(m_debugParamEdit, 1);
    debugPanelLayout->addLayout(debugRow2);

    mainLayout->addWidget(m_debugPanel);

    // 中间区域不再放升级控件: 进度条移入 LOG 区(已存在 m_logProgressBar),
    // 校验级别硬编码 FULL(见 startUpgrade), 升级过程详情统一显示在左下日志.
    // LOG 区进度条 + 状态已足够.

    // ===== 日志栏 =====
    auto *logSplitter = new QSplitter(Qt::Vertical);

    // ===== Exporting Area (发送区) — 上面, 独立 QGroupBox =====
    auto *sendGroupBox = new QGroupBox(tr("Exporting Area"));
    sendGroupBox->setObjectName("sendGroupBox");
    auto *sendGroupLayout = new QVBoxLayout(sendGroupBox);
    sendGroupLayout->setContentsMargins(8, 4, 8, 4);
    sendGroupLayout->setSpacing(4);

    m_timerSendCheck = new QCheckBox(tr("定时发送"));
    auto *sendHeader = new QHBoxLayout;
    sendHeader->setSpacing(10);  // 增大元素间距, 避免文字与输入框重叠
    sendHeader->addWidget(m_timerSendCheck);
    sendHeader->addSpacing(4);   // 定时发送文字与输入框之间留白
    m_timerSendSpin = new QSpinBox;
    m_timerSendSpin->setRange(100, 60000);
    m_timerSendSpin->setValue(1000);
    m_timerSendSpin->setSingleStep(100);
    m_timerSendSpin->setMinimumWidth(70);
    sendHeader->addWidget(m_timerSendSpin);
    auto *msLabel = new QLabel(tr("ms"));
    msLabel->setStyleSheet("font-size: 0.85em;");
    sendHeader->addWidget(msLabel);
    sendHeader->addStretch();
    m_sendHexCheck = new QCheckBox(tr("Hex发送"));
    m_sendHexCheck->setChecked(true);
    sendHeader->addWidget(m_sendHexCheck);
    sendHeader->addSpacing(10);  // Hex发送 与 发送 之间留白
    m_sendBtn = new QPushButton(tr("发送"));
    m_sendBtn->setObjectName("sendBtn");
    m_sendBtn->setDefault(true);
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::sendData);
    sendHeader->addWidget(m_sendBtn);
    sendGroupLayout->addLayout(sendHeader);

    m_sendEdit = new QTextEdit;
    m_sendEdit->setMaximumHeight(80);
    m_sendEdit->setFont(QFont("Menlo", 10));
    sendGroupLayout->addWidget(m_sendEdit);

    // ===== Log Area (日志区) — 下面, 独立 QGroupBox =====
    auto *logGroupBox = new QGroupBox(tr("Log Area"));
    logGroupBox->setObjectName("logGroupBox");
    auto *logGroupLayout = new QVBoxLayout(logGroupBox);
    logGroupLayout->setContentsMargins(8, 4, 8, 4);
    logGroupLayout->setSpacing(4);

    auto *logHeader = new QHBoxLayout;
    logHeader->setSpacing(8);  // 增大元素间距
    auto *logTitle = new QLabel(tr("Log Area"));
    logTitle->setStyleSheet("font-weight: bold;");
    logHeader->addStretch();
    auto *frameGapLabel = new QLabel(tr("帧间隔:"));
    frameGapLabel->setStyleSheet("font-size: 0.82em; color: #8870a8;");
    logHeader->addWidget(frameGapLabel);
    m_frameGapSpin = new QSpinBox;
    m_frameGapSpin->setRange(10, 500);
    m_frameGapSpin->setValue(50);
    m_frameGapSpin->setSuffix(" ms");
    m_frameGapSpin->setMaximumWidth(90);
    m_frameGapSpin->setToolTip(tr("串口数据帧间间隔\n数据停止超过此时间视为一帧结束"));
    logHeader->addWidget(m_frameGapSpin);
    logHeader->addSpacing(12);
    m_logHexCheck = new QCheckBox(tr("Hex显示"));
    m_logHexCheck->setChecked(true);
    logHeader->addWidget(m_logHexCheck);
    logHeader->addSpacing(10);  // Hex显示 与 清空 之间留白
    m_logDisplay = new QTextEdit;
    m_logDisplay->setObjectName("logDisplay");
    m_logDisplay->setReadOnly(true);
    m_logDisplay->setFont(QFont("Menlo", 10));
    m_logDisplay->setMinimumHeight(120);   // Round029 D5: LOG 区保底 5 行, 最大化时不被上方挤出
    m_clearLogBtn = new QPushButton(tr("清空"));
    connect(m_clearLogBtn, &QPushButton::clicked, m_logDisplay, &QTextEdit::clear);
    connect(m_clearLogBtn, &QPushButton::clicked, this, [this](){ m_saveLogBtn->setEnabled(false); });
    logHeader->addWidget(m_clearLogBtn);
    m_saveLogBtn = new QPushButton(tr("保存"));
    m_saveLogBtn->setObjectName("saveLogBtn");
    m_saveLogBtn->setEnabled(false);
    connect(m_saveLogBtn, &QPushButton::clicked, this, &MainWindow::onSaveLog);
    logHeader->addWidget(m_saveLogBtn);
    logGroupLayout->addLayout(logHeader);

    // 日志容器：叠加日志文本框和底部进度条
    auto *logContainer = new QFrame;
    logContainer->setObjectName("logContainer");
    auto *containerLayout = new QVBoxLayout(logContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);
    containerLayout->addWidget(m_logDisplay, 1);

    // 日志区底部发光进度条
    m_logProgressBar = new QProgressBar;
    m_logProgressBar->setObjectName("logProgressBar");
    m_logProgressBar->setFixedHeight(6);
    m_logProgressBar->setTextVisible(false);
    m_logProgressBar->setRange(0, 100);
    m_logProgressBar->setValue(0);
    m_logProgressBar->setVisible(false);
    containerLayout->addWidget(m_logProgressBar);

    logGroupLayout->addWidget(logContainer);

    // Exporting Area 在上(索引 0, stretchFactor 小), Log Area 在下(索引 1, stretchFactor 大)
    logSplitter->addWidget(sendGroupBox);
    logSplitter->addWidget(logGroupBox);
    logSplitter->setStretchFactor(0, 1); // 发送区比例小
    logSplitter->setStretchFactor(1, 3); // 日志区比例大
    mainLayout->addWidget(logSplitter, 1);

    // 升级状态整合进左下日志: 不再单独显示"步骤X/6"弹窗式状态条, 由 appendSystemLog 输出到日志区

    setCentralWidget(central);

    // 状态栏: 左=系统消息(扫描USB/连接状态等), 右=永久版本号
    auto *sbLeft = new QLabel(tr("就绪"));
    sbLeft->setObjectName("statusBarLeft");
    auto *sbRight = new QLabel(tr("SwComForTool %1 (Qt %2)")
        .arg(COMFORTOOL_VERSION).arg(QString::fromLatin1(qVersion())));
    sbRight->setObjectName("statusBarRight");
    sbRight->setStyleSheet("color: #a080c0; font-size: 0.8em;");
    statusBar()->addWidget(sbLeft, 1);

    applyTransportVisibility();
}

// ============================ Stylesheet ============================

void MainWindow::applyStylesheet()
{
    setStyleSheet(R"(
        QMainWindow { background: #f0eaf8; }
        QWidget { background: #f0eaf8; color: #503070; }
        QGroupBox {
            border: 1px solid #c8b8e0;
            border-radius: 6px;
            margin-top: 8px;
            padding-top: 10px;
            font-weight: bold;
            color: #7830b0;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }
        QComboBox {
            background: #e8e0f4; border: 1px solid #c8b8e0; border-radius: 4px;
            padding: 3px 8px; color: #503070; min-width: 50px;
        }
        QComboBox::drop-down { border: none; }
        QComboBox QAbstractItemView { background: #e8e0f4; color: #503070; selection-background-color: #d0c0e8; }
        QPushButton {
            background: #e0d4f0; border: 1px solid #c8b8e0; border-radius: 4px;
            padding: 4px 12px; color: #503070;
        }
        QPushButton:hover { background: #d0c0e8; }
        QPushButton:pressed { background: #c0b0d8; }
        QPushButton:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#openBtn { background: #7830b0; color: #f0eaf8; font-weight: bold; padding: 4px 16px; }
        QPushButton#openBtn:hover { background: #8a40c0; }
        QPushButton#openBtn[connected="true"] { background: #50a050; color: #f0fff0; }
        QPushButton#openBtn[connected="true"]:hover { background: #60c060; }
        QPushButton#funcHandshakeBtn { background: #c05050; color: #fff; font-weight: bold; padding: 8px 12px; }
        QPushButton#funcHandshakeBtn:hover { background: #d06060; }
        QPushButton#funcHandshakeBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#funcHandshakeBtn[handshaked="true"] { background: #50a050; color: #fff; }
        QPushButton#funcHandshakeBtn[handshaked="true"]:hover { background: #60b060; }
        QPushButton#sendBtn { background: #7830b0; color: #f0eaf8; font-weight: bold; }
        QPushButton#sendBtn:hover { background: #8a40c0; }
        QPushButton#sendBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#updateBtn { background: #c050a0; color: #fff; font-weight: bold; padding: 6px 14px; }
        QPushButton#updateBtn:hover { background: #d060b0; }
        QPushButton#updateBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#debugBtn { background: #50a050; color: #fff; font-weight: bold; padding: 6px 14px; }
        QPushButton#debugBtn:hover { background: #60b060; }
        QPushButton#debugBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        /* Supported Devices 边框 (Sccd 容器) */
        QGroupBox#supportedDevicesBox { border: 1px solid #b090d0; border-radius: 6px; margin-top: 10px; padding-top: 12px; font-weight: bold; color: #503070; }
        QGroupBox#supportedDevicesBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QPushButton#sccdBtn { background: #a060c0; color: #fff; font-weight: bold; padding: 8px 12px; }
        QPushButton#sccdBtn:hover { background: #b070d0; }
        QPushButton#sccdBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#sccdBtn[active="true"] { background: #50a050; color: #fff; }
        QPushButton#sccdBtn[active="true"]:hover { background: #60b060; }
        QPushButton#sccdBtn15693, QPushButton#sccdBtn14443A, QPushButton#sccdBtn14443B { background: #e0d4f0; color: #503070; padding: 4px 12px; }
        QPushButton#sccdBtn15693:hover, QPushButton#sccdBtn14443A:hover, QPushButton#sccdBtn14443B:hover { background: #d0c0e8; }
        QPushButton#sccdBtn15693:checked, QPushButton#sccdBtn14443A:checked, QPushButton#sccdBtn14443B:checked { background: #7830b0; color: #f0eaf8; }
        /* ZLR5401 区域 */
        QGroupBox#zlrBox { border: 1px solid #b090d0; border-radius: 6px; margin-top: 10px; padding-top: 12px; }
        QPushButton#zlrBtn { background: #3080b0; color: #fff; font-weight: bold; padding: 8px 12px; }
        QPushButton#zlrBtn:hover { background: #4090c0; }
        QPushButton#zlrBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#zlrBtn[active="true"] { background: #50a050; color: #fff; }
        QPushButton#zlrBtn[active="true"]:hover { background: #60b060; }
        QPushButton#zlrMotorBtn, QPushButton#zlrUhfBtn { background: #a080c0; color: #fff; padding: 4px 10px; font-size: 0.85em; }
        QPushButton#zlrMotorBtn:hover, QPushButton#zlrUhfBtn:hover { background: #b090d0; }
        QPushButton#zlrMotorBtn:disabled, QPushButton#zlrUhfBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#zlrMotorMain { background: #c05050; color: #fff; font-weight: bold; padding: 5px 12px; }
        QPushButton#zlrMotorMain:hover { background: #d06060; }
        QPushButton#zlrMotorMain:disabled { background: #e8e0f4; color: #b0a0c0; }
        /* 电机停止: 警示橙色 (区别于正转/反转的红/绿, 危险动作强调) */
        QPushButton#zlrMotorStop { background: #e07800; color: #fff; font-weight: bold; padding: 5px 14px; }
        QPushButton#zlrMotorStop:hover { background: #f08810; }
        QPushButton#zlrMotorStop:disabled { background: #e8e0f4; color: #b0a0c0; }
        /* 电机分区 GroupBox */
        QGroupBox#zlrMotorGroup { border: 1px solid #b090d0; border-radius: 6px; margin-top: 8px; padding-top: 8px; }
        QGroupBox#zlrMotorGroup::title { subcontrol-origin: margin; left: 10px; font-size: 0.95em; }
        /* 自检错误位/诊断标签 */
        QLabel#zlrSelfErrBitsLabel { background: #ffffff; border: 1px solid #c8b8e0; border-radius: 4px; padding: 6px; }
        QLabel#zlrSelfDiagLabel { background: #f8f4ff; border: 1px solid #d8c8f0; border-radius: 4px; padding: 4px; }
        QPushButton#zlrUhfMain { background: #c08030; color: #fff; font-weight: bold; padding: 5px 12px; }
        QPushButton#zlrUhfMain:hover { background: #d09040; }
        QPushButton#zlrUhfMain:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#zlrAmMain { background: #508050; color: #fff; font-weight: bold; padding: 5px 12px; }
        QPushButton#zlrAmMain:hover { background: #609060; }
        QPushButton#zlrAmMain:disabled { background: #e8e0f4; color: #b0a0c0; }
        QTableWidget#zlrTagTable {
            background: #ffffff; color: #2a2438; border: 1px solid #c8b8e0;
            gridline-color: #d8c8f0; selection-background-color: #d0c0e8; selection-color: #2a2438;
            alternate-background-color: #f6f2fb;
        }
        QTableWidget#zlrTagTable QHeaderView::section {
            background: #d8c8f0; color: #4a2a7a; border: none; padding: 3px 6px; font-weight: bold;
        }
        QPushButton#15693InventoryBtn, QPushButton#15693ReadBtn, QPushButton#15693WriteBtn,
        QPushButton#15693LockBtn, QPushButton#15693StayQuietBtn,
        QPushButton#15693DsfidBtn, QPushButton#15693EasBtn,
        QPushButton#15693WriteAfiBtn, QPushButton#15693LockAfiBtn,
        QPushButton#14443AInventoryBtn, QPushButton#14443AReadBtn, QPushButton#14443AWriteBtn,
        QPushButton#14443AHaltBtn, QPushButton#14443ARatsBtn, QPushButton#14443ATransApduBtn,
        QPushButton#14443ATopazReadBtn, QPushButton#14443ATopazWriteBtn,
        QPushButton#14443AWalletInitBtn, QPushButton#14443AWalletGetBtn, QPushButton#14443AWalletIncBtn, QPushButton#14443AWalletDecBtn
            { background: #a080c0; color: #fff; padding: 4px 10px; font-size: 0.85em; }
        QPushButton#14443AInventoryBtn { background: #50a050; }   /* 盘点: 主操作绿色 */
        QPushButton#14443AInventoryBtn:hover { background: #60b060; }
        QPushButton#14443AInventoryBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#14443AReadBtn:hover, QPushButton#14443AWriteBtn:hover,
        QPushButton#14443AHaltBtn:hover, QPushButton#14443ARatsBtn:hover, QPushButton#14443ATransApduBtn:hover,
        QPushButton#14443ATopazReadBtn:hover, QPushButton#14443ATopazWriteBtn:hover,
        QPushButton#14443AWalletInitBtn:hover, QPushButton#14443AWalletGetBtn:hover,
        QPushButton#14443AWalletIncBtn:hover, QPushButton#14443AWalletDecBtn:hover { background: #b090d0; }
        QPushButton#14443AReadBtn:disabled, QPushButton#14443AWriteBtn:disabled,
        QPushButton#14443AHaltBtn:disabled, QPushButton#14443ARatsBtn:disabled, QPushButton#14443ATransApduBtn:disabled,
        QPushButton#14443ATopazReadBtn:disabled, QPushButton#14443ATopazWriteBtn:disabled,
        QPushButton#14443AWalletInitBtn:disabled, QPushButton#14443AWalletGetBtn:disabled,
        QPushButton#14443AWalletIncBtn:disabled, QPushButton#14443AWalletDecBtn:disabled,
        QPushButton#15693InventoryBtn:disabled, QPushButton#15693ReadBtn:disabled, QPushButton#15693WriteBtn:disabled,
        QPushButton#15693LockBtn:disabled, QPushButton#15693StayQuietBtn:disabled,
        QPushButton#15693DsfidBtn:disabled, QPushButton#15693EasBtn:disabled,
        QPushButton#15693WriteAfiBtn:disabled, QPushButton#15693LockAfiBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        /* KeyA / KeyB 互斥选中按钮 */
        QPushButton#14443AKeyABtn, QPushButton#14443AKeyBBtn { background: #e0d4f0; color: #503070; padding: 4px 10px; }
        QPushButton#14443AKeyABtn:hover, QPushButton#14443AKeyBBtn:hover { background: #d0c0e8; }
        QPushButton#14443AKeyABtn:checked { background: #5080c0; color: #fff; }   /* KeyA 选中: 蓝 */
        QPushButton#14443AKeyBBtn:checked { background: #c050a0; color: #fff; }   /* KeyB 选中: 粉 */
        /* 射频控制按钮 (默认紫色) */
        QPushButton#rfOpenBtn  { background: #7830b0; color: #fff; font-weight: bold; padding: 4px 10px; }
        QPushButton#rfCloseBtn { background: #a060c0; color: #fff; font-weight: bold; padding: 4px 10px; }
        QPushButton#rfResetBtn { background: #a060c0; color: #fff; font-weight: bold; padding: 4px 10px; }
        QPushButton#rfOpenBtn:hover  { background: #8a40c0; }
        QPushButton#rfCloseBtn:hover, QPushButton#rfResetBtn:hover { background: #b070d0; }
        QPushButton#rfOpenBtn:disabled, QPushButton#rfCloseBtn:disabled, QPushButton#rfResetBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        /* 14443A 输入/输出区 GroupBox */
        QGroupBox#14443AInputBox { border: 1px solid #c8b8e0; border-radius: 4px; margin-top: 8px; padding-top: 8px; font-weight: bold; color: #7830b0; }
        QGroupBox#14443AInputBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QGroupBox#14443AOutputBox { border: 1px solid #c8b8e0; border-radius: 4px; margin-top: 8px; padding-top: 8px; font-weight: bold; color: #7830b0; }
        QGroupBox#14443AOutputBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        /* 15693 输入/输出区 GroupBox (与 14443A 同款边框 + 统一复用原 iso14443AUidEdit 同色系) */
        QGroupBox#15693InputBox { border: 1px solid #c8b8e0; border-radius: 4px; margin-top: 8px; padding-top: 8px; font-weight: bold; color: #305888; }
        QGroupBox#15693InputBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QGroupBox#15693OutputBox { border: 1px solid #c8b8e0; border-radius: 4px; margin-top: 8px; padding-top: 8px; font-weight: bold; color: #305888; }
        QGroupBox#15693OutputBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QLineEdit#iso15693UidEdit { background: #f0f4ff; border: 1px solid #b0c0d8; border-radius: 3px; padding: 2px 4px; color: #305888; font-family: "Menlo", monospace; }
        /* 钱包组: 带边框 GroupBox + 余额只读框 */
        QGroupBox#14443AWalletBox { border: 1px solid #b090d0; border-radius: 4px; margin-top: 8px; padding-top: 10px; font-weight: bold; color: #503070; }
        QGroupBox#14443AWalletBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QLineEdit#14443AWalletBalEdit { background: #fffaf0; border: 1px solid #d8c890; border-radius: 3px; padding: 2px 4px; color: #705030; font-weight: bold; }
        QLineEdit#14443AWalletBalEdit:disabled { background: #f8f4e8; color: #b0a080; }
        QTextEdit#14443AInputArea { background: #f8f4ff; border: 1px solid #d8d0e8; border-radius: 3px; }
        QTextEdit#14443AOutputArea { background: #f0ecf8; border: 1px solid #d8d0e8; border-radius: 3px; color: #503070; }
        /* Key 输入框 */
        QLineEdit#14443AKeyAEdit, QLineEdit#14443AKeyBEdit { background: #fffaf0; border: 1px solid #d8c890; border-radius: 3px; padding: 2px 4px; color: #705030; font-family: "Menlo", monospace; }
        QPushButton#saveLogBtn { background: #a080c0; color: #fff; font-weight: bold; padding: 4px 10px; }
        QPushButton#saveLogBtn:hover { background: #b090d0; }
        QPushButton#saveLogBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#debugPackBtn { background: #50a050; color: #fff; font-weight: bold; padding: 4px 12px; }
        QPushButton#debugPackBtn:hover { background: #60b060; }
        QTextEdit#logDisplay { background: #f8f4ff; border: 1px solid #d8d0e8; border-radius: 4px; color: #503070; }
        QTextEdit { background: #f4f0fa; border: 1px solid #d8d0e8; border-radius: 4px; color: #503070; }
        QLineEdit#verAddrEdit { background: #f8f4ff; border: 1px solid #c8b8e0; border-radius: 3px; padding: 2px 4px; color: #503070; font-family: "Menlo", monospace; }
        QLineEdit#verSwEdit { background: #f0ecf8; border: 1px solid #c8b8e0; border-radius: 3px; padding: 2px 4px; color: #8870a8; font-family: "Menlo", monospace; }
        QLineEdit#verHwEdit { background: #f0ecf8; border: 1px solid #c8b8e0; border-radius: 3px; padding: 2px 4px; color: #8870a8; font-family: "Menlo", monospace; }
        QLineEdit#iso14443AUidEdit, QLineEdit#iso15693UidEdit { background: #fffaf0; border: 1px solid #d8c890; border-radius: 3px; padding: 2px 4px; color: #705030; font-family: "Menlo", monospace; }
        QGroupBox#sccdBox { border: 1px solid #c0a0e0; border-radius: 6px; margin-top: 8px; padding-top: 10px; font-weight: bold; color: #7830b0; background: #faf0ff; }
        QGroupBox#sccdBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }
        QPushButton#protoEnterBootBtn { background: #d08020; color: #fff; font-weight: bold; padding: 6px 14px; }
        QPushButton#protoEnterBootBtn:hover { background: #e09030; }
        QPushButton#protoEnterBootBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QPushButton#protoResetBtn { background: #c05050; color: #fff; font-weight: bold; padding: 6px 14px; }
        QPushButton#protoResetBtn:hover { background: #d06060; }
        QPushButton#protoResetBtn:disabled { background: #e8e0f4; color: #b0a0c0; }
        QTabWidget#sccdTabs::pane { border: 1px solid #d0c0e0; background: #f8f4ff; border-radius: 4px; }
        QTabBar::tab { background: #e0d4f0; color: #503070; padding: 4px 14px; border: 1px solid #c8b8e0; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; }
        QTabBar::tab:selected { background: #7830b0; color: #f0eaf8; }
        QTabBar::tab:hover:!selected { background: #d0c0e8; }
        QCheckBox { color: #7850a0; spacing: 6px; }
        QCheckBox::indicator { width: 14px; height: 14px; }
        QCheckBox:disabled { color: #b0a0c8; }
        QLabel { color: #503070; background: transparent; }
        QSplitter::handle { background: #d8d0e8; height: 3px; }
        QProgressBar { border: 1px solid #c8b8e0; border-radius: 4px; text-align: center; background: #e8e0f4; }
        QProgressBar::chunk { background: #7830b0; border-radius: 3px; }
        QProgressBar#logProgressBar { border: none; border-radius: 3px; background: #e0d8f0; }
        QProgressBar#logProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #a040d0, stop:0.5 #d050c0, stop:1 #e060a0); border-radius: 3px; }
        QFrame#logContainer { border: none; }
        QFrame#debugPanel { border: 1px solid #a0d8a0; border-radius: 6px; background: #f0faf0; }
        QSpinBox { background: #e8e0f4; border: 1px solid #c8b8e0; border-radius: 4px; padding: 2px 6px; color: #503070; }
        QSpinBox:disabled { background: #e8e0f4; color: #b0a0c0; }
    )");
}

// ============================ Port ============================

void MainWindow::refreshPorts()
{
    m_hidDevices.clear();
    if (m_transportType == TransportType::Usb) {
#ifdef COMFORTOOL_ENABLE_USB
        // 全量枚举所有 HID 设备(不过滤 VID:PID), 便于定位真实设备.
        // 纯 IOHIDManager, GUI 进程内独立线程跑 CFRunLoop (主线程 Qt 循环时不派发 matching).
        // 枚举结果不播报(需求: 不提示扫描到 SCCD 设备), 由 onSearchBtn 决定是否自动连接.
        m_hidDevices = HidManager::enumerate(0x0000, 0x0000);
        // 刷新路径 Label (保持选中索引或显示默认)
        if (m_selectedHudIdx >= m_hidDevices.size()) m_selectedHudIdx = 0;
        updatePathLabel();
        return;
#endif
    }
    statusBar()->showMessage(tr("USB 传输未编译"), 5000);
}

void MainWindow::onSearchBtn()
{
    // Search: 弹模态 Loading 遮罩 3s(居中, 其他区域不可操作), 期间刷新 HID 列表 + 自动连接 VID 0x5377
    QProgressDialog loadingDlg(tr("正在搜索 USB 设备..."), QString(), 0, 0, this);
    loadingDlg.setWindowModality(Qt::ApplicationModal);
    loadingDlg.setWindowFlags(loadingDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    loadingDlg.setCancelButton(nullptr); // 不允许取消, 固定 3s 后关闭
    loadingDlg.setMinimumDuration(0);
    loadingDlg.show();
    QCoreApplication::processEvents();

    refreshPorts();
    int swIdx = -1;
    for (int i = 0; i < m_hidDevices.size(); ++i) {
        if (m_hidDevices[i].vendorId == 0x5377) { swIdx = i; break; }
    }
    bool found = (swIdx >= 0);
    bool alreadyConnected = (m_transport && m_transport->isOpen());

    // 找到 PID 0x5378 设备且未连接则自动连接握手(VID 0x5377 仅是 Sw01 系列前缀, 还需核对 PID 0x5378)
    bool willConnect = false;
    if (!alreadyConnected && swIdx >= 0 && m_hidDevices[swIdx].productId == 0x5378) {
        willConnect = true;
    }

    // 提交实际动作到 main loop 执行, 确保 dialog 先显示
    QTimer::singleShot(50, this, [this, willConnect, found, alreadyConnected]() {
        if (!alreadyConnected && willConnect) {
            onOpen(); // 自动连接 VID 0x5377:0x5378 并握手
        } else if (!found) {
            appendSystemLog("未发现 PID 0x5378 设备, 不主动连接", QColor("#d08020"));
        } else if (alreadyConnected) {
            appendSystemLog("已连接", QColor("#50a050"));
        }
    });
    // 3s 后强制关闭 dialog
    QTimer::singleShot(3000, &loadingDlg, [&loadingDlg]() {
        loadingDlg.close();
    });
    // 保持 dialog 模态直到 3s 计时器/连接/超时结束. processEvents 在此阻塞(因 dialog modal).
    // 用 modal dialog.exec() 取代 singleShot 关闭以更可靠:
    loadingDlg.exec(); // 阻塞至 close(). singleShot 会在 3s 调用 close().
}

void MainWindow::onOpen()
{
    // 当前激活传输已打开 -> 关闭
    if (m_transport && m_transport->isOpen()) {
        { QFile _f(QDir::tempPath() + "/comfor_onopen.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
          QTextStream _s(&_f); _s << "[onOpen] close branch: was open\n"; _s.flush(); }
        m_transport->close();
        m_transport = nullptr;
        // 按钮状态: 连接
        m_openBtn->setText(tr("Connect"));
        m_openBtn->setProperty("connected", false);
        m_openBtn->style()->unpolish(m_openBtn);
        m_openBtn->style()->polish(m_openBtn);
        statusBar()->showMessage(tr("已断开"), 3000);
        return;
    }

    // 自动选择设备: 优先 VID 0x5377, 否则默认索引 0
    if (m_transportType == TransportType::Usb) {
#ifdef COMFORTOOL_ENABLE_USB
        int targetIdx = -1;
        for (int i = 0; i < m_hidDevices.size(); ++i) {
            if (m_hidDevices[i].vendorId == 0x5377 && m_hidDevices[i].productId == 0x5378) {
                targetIdx = i; break;
            }
        }
        if (targetIdx < 0 && !m_hidDevices.isEmpty()) targetIdx = 0;
        if (targetIdx < 0) {
            statusBar()->showMessage(tr("未发现 HID 设备, 请点击 Search"), 5000);
            return;
        }
        m_selectedHudIdx = targetIdx;
        updatePathLabel();

        const auto &d = m_hidDevices[targetIdx];
        m_transport = m_hidManager;
        QVariantMap params;
        params["path"] = d.path;
        params["vid"]  = d.vendorId;
        params["pid"]  = d.productId;
        {
            QFile _f(QDir::tempPath() + "/comfor_onopen.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
            QTextStream _s(&_f); _s << "[onOpen] before open err='" << m_transport->errorString() << "'\n"; _s.flush();
        }
        bool ok = m_transport->open(params);
        {
            QFile _f(QDir::tempPath() + "/comfor_onopen.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
            QTextStream _s(&_f); _s << "[onOpen] open ret=" << ok << " err='" << m_transport->errorString() << "' isOpen=" << m_transport->isOpen() << "\n"; _s.flush();
        }
        if (!ok) {
            statusBar()->showMessage(tr("HID打开失败: %1").arg(m_transport->errorString()), 5000);
            m_transport = nullptr;
            return;
        }
        // 按钮状态: 已连接
        m_openBtn->setText(tr("DisConnect"));
        m_openBtn->setProperty("connected", true);
        m_openBtn->style()->unpolish(m_openBtn);
        m_openBtn->style()->polish(m_openBtn);
        statusBar()->showMessage(tr("已连接: %1").arg(m_transport->name()), 5000);
#else
        statusBar()->showMessage(tr("USB 传输未编译"), 5000);
#endif
        return;
    }

    // COM 暂不启用
    statusBar()->showMessage(tr("COM 串口暂不启用, 请使用 USB"), 5000);
}

void MainWindow::onPathLabelClicked()
{
    // 弹出 QMenu 列出当前 HID 设备 (仅手动选择, 不再刷新; 刷新由 Search 按钮负责)
    QMenu menu(this);
    if (m_hidDevices.isEmpty()) {
        QAction *none = menu.addAction(tr("(无 HID 设备)"));
        none->setEnabled(false);
    } else {
        for (int i = 0; i < m_hidDevices.size(); ++i) {
            const auto &d = m_hidDevices[i];
            QString vidPid = QString("VID %1:PID %2")
                .arg(d.vendorId, 4, 16, QChar('0')).toUpper()
                .arg(d.productId, 4, 16, QChar('0')).toUpper();
            QString text = QString("%1 | %2")
                .arg(vidPid)
                .arg(d.product.isEmpty() ? tr("(无产品名)") : d.product);
            QAction *act = menu.addAction(text);
            act->setData(i);
            // 悬停显示完整 HID 路径 + 厂商 + 序列号, 便于在多设备时定位
            act->setToolTip(QString("路径: %1\n厂商: %2\n序列号: %3")
                                .arg(d.path).arg(d.manufacturer).arg(d.serial));
            if (i == m_selectedHudIdx) {
                QFont f = act->font(); f.setBold(true); act->setFont(f);
            }
        }
    }
    QAction *sel = menu.exec(m_pathLabel->mapToGlobal(QPoint(0, m_pathLabel->height())));
    if (sel && sel->data().isValid()) {
        m_selectedHudIdx = sel->data().toInt();
        updatePathLabel();
    }
}

void MainWindow::updatePathLabel()
{
    if (m_selectedHudIdx >= 0 && m_selectedHudIdx < m_hidDevices.size()) {
        const auto &d = m_hidDevices[m_selectedHudIdx];
        m_pathLabel->setText(QString("VID %1:PID %2")
            .arg(d.vendorId, 4, 16, QChar('0')).toUpper()
            .arg(d.productId, 4, 16, QChar('0')).toUpper());
    } else {
        m_pathLabel->setText(tr("(未选择)"));
    }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_pathLabel && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            onPathLabelClicked();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ============================ Handshake ============================

void MainWindow::onFuncHandshake()
{
    { QFile _f(QDir::tempPath() + "/comfor_onopen.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[handshake] m_transport=" << (qulonglong)m_transport << " isOpen=" << (m_transport? m_transport->isOpen(): -1) << "\n"; _s.flush(); }
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("请先打开通信接口");
        return;
    }

    QString addrText = m_verAddrEdit->text().trimmed();
    // 地址框为空时用 0xFF 广播探测: 设备真实地址不固定(实测为 0x00, 非 0x01),
    // 用 0x01 会被静默丢弃; 广播必响应且回告真实 devAddr, 握手成功后由响应回填.
    quint8 devAddr = 0xFF;
    if (!addrText.isEmpty()) {
        bool ok;
        devAddr = static_cast<quint8>(addrText.toInt(&ok, 16));
        if (!ok) devAddr = 0xFF;
    }
    m_deviceAddr = devAddr;

    QByteArray req = m_parser->makeHandshakeFrame(devAddr);

    // 单次发送 + 等响应, 解析失败也完整显示原始字节
    m_rxBuffer.clear();
    m_transport->writeData(req);
    m_transport->waitForBytesWritten(5000);
    appendLog("[SYS][TX]", req, QColor("#c050a0"), tr("握手请求"));

    if (waitForResponse(3000)) {
        onHandshakeResponse();
    } else {
        appendSystemLog(QString("[握手] 超时 3s 无响应 (rxBuf=%1字节)").arg(m_rxBuffer.size()));
        m_funcStatusLabel->setText("● 握手超时");
        m_funcStatusLabel->setStyleSheet("color: #d08020; font-size: 0.82em;");
    }
}

void MainWindow::onHandshakeResponse()
{
    // 系统级: 单行 [SYS][RX] 显示完整原始字节 + 解析描述
    if (m_rxBuffer.isEmpty()) return;

    FrameData frame = m_parser->parseFrame(m_rxBuffer);
    QString note;
    bool handshakeOk = false;
    // Round 019 HANDSHAKE 28B (App/Boot 一致, 移除冗余 version 字段):
    //   [0]result [1]protoVer [2]status [3-14]UID(12B) [15-18]uidHash(LE)
    //   [19]layer [20-23]upgradeCount(LE) [24-27]baudRate(LE)
    //   只要 result/proto/status/layer 关键字段存在即判定 OK, baudRate 高字节缺失也容错.
    if (frame.valid && frame.fc == 0xFE
        && frame.data.size() >= 20 && static_cast<quint8>(frame.data[0]) == 0x00) {
        quint8 protoVersion = static_cast<quint8>(frame.data[1]);
        quint8 layer = static_cast<quint8>(frame.data[19]);
        quint8 deviceStatus = static_cast<quint8>(frame.data[2]);
        quint32 baudRate = 0;
        if (frame.data.size() >= 28) {
            baudRate = static_cast<quint8>(frame.data[24])
                     | (static_cast<quint8>(frame.data[25]) << 8)
                     | (static_cast<quint8>(frame.data[26]) << 16)
                     | (static_cast<quint8>(frame.data[27]) << 24);
        } else if (frame.data.size() >= 27) {
            baudRate = static_cast<quint8>(frame.data[24])
                     | (static_cast<quint8>(frame.data[25]) << 8)
                     | (static_cast<quint8>(frame.data[26]) << 16);
        }
        m_deviceAddr = frame.devAddr;
        m_verAddrEdit->setText(QString("0x%1").arg(hex2(m_deviceAddr)).toUpper());
        QStringList layerNames = {"BOOT", "APP"};
        QString layerStr = layer < layerNames.size() ? layerNames[layer] : "?";
        QStringList statusNames = {"IDLE", "RUN", "UPG", "FAULT"};
        QString statusStr = deviceStatus < statusNames.size() ? statusNames[deviceStatus] : "?";
        note = QString("OK DevAddr=0x%1 Proto=%2 Layer=%3(%4) Status=%5 Baud=%6")
                    .arg(hex2(m_deviceAddr)).arg(protoVersion)
                    .arg(layer).arg(layerStr).arg(statusStr).arg(baudRate);
        m_funcStatusLabel->setText("● 设备已连接");
        m_funcStatusLabel->setStyleSheet("color: #50a050; font-size: 0.82em;");
        m_saveLogBtn->setEnabled(true);
        handshakeOk = true;
    } else if (frame.valid && frame.fc == 0xFE) {
        quint8 r = frame.data.isEmpty() ? 0xFF : static_cast<quint8>(frame.data[0]);
        note = QString("握手 result=0x%1 (期望 0)").arg(hex2(r));
    } else if (!frame.valid) {
        note = QString("帧解析失败: %1").arg(frame.error);
    } else {
        note = QString("FC=0x%1 data(%2B)=%3")
                    .arg(hex2(frame.fc)).arg(frame.data.size())
                    .arg(QString::fromLatin1(frame.data.toHex(' ')).toUpper());
    }
    appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"), note);
m_rxBuffer.clear();

    // 握手成功 → 立即解锁 FUN 区其他按钮 (需求: 握手成功后再开放)
    if (handshakeOk) {
        m_funcUnlocked = true;
        enableFuncButtons(true);
        // 握手按钮变绿色
        m_funcHandshakeBtn->setProperty("handshaked", true);
        m_funcHandshakeBtn->style()->unpolish(m_funcHandshakeBtn);
        m_funcHandshakeBtn->style()->polish(m_funcHandshakeBtn);
        m_funcStatusLabel->setText("● 握手成功, FUN 区已开放");
        m_funcStatusLabel->setStyleSheet("color: #50a050; font-size: 0.82em;");

        // 握手成功后自动发 DEVICE_INFO 拿 hwVersion/version, 渲染到版本信息 UI (三行)
        QByteArray reqInfo = m_parser->makeDeviceInfoFrame(m_deviceAddr);
        m_rxBuffer.clear();
        m_transport->writeData(reqInfo);
        m_transport->waitForBytesWritten(5000);
        appendLog("[SYS][TX]", reqInfo, QColor("#c050a0"), tr("设备信息请求"));

        if (waitForResponse(3000)) {
            FrameData infoFrame = m_parser->parseFrame(m_rxBuffer);
            // DEVICE_INFO 响应 34B: [0]result [1]addr [2-17]hwVersion [18-33]version
            if (infoFrame.valid && infoFrame.fc == (0x07 ^ 0xFF)
                && infoFrame.data.size() >= 34
                && static_cast<quint8>(infoFrame.data[0]) == 0x00) {
                quint8 addr = static_cast<quint8>(infoFrame.data[1]);
                QString hw = QString::fromLatin1(infoFrame.data.mid(2, 16).constData()).trimmed();
                QString sw = QString::fromLatin1(infoFrame.data.mid(18, 16).constData()).trimmed();
                m_verAddrEdit->setText(QString("0x%1").arg(hex2(addr)).toUpper());
                m_verSwEdit->setText(QString("SW:%1").arg(sw));
                m_verHwEdit->setText(QString("HW:%1").arg(hw));
                // Round028: 握手后按 SW 启用 Supported Devices 区按钮 (ZLR5401 / SCCD 二选一, 不符则全禁用)
                enableSuppBySw(sw);
                // 设备型号 → 标记到 ZLR5401 区域 (SW 以 "ZLR" 开头判定为 ZLR5401 设备)
                // Round029: ZLR5401 Area 标题仅 "ZLR5401 Area", 不附加 SW 版本号
                if (sw.toUpper().startsWith("ZLR")) {
                    if (m_zlrBox) m_zlrBox->setTitle(tr("ZLR5401 Area"));
                    if (m_zlrBtn) m_zlrBtn->setToolTip(
                        tr("检测到 ZLR5401 (%1): 电机 / UHF / AM / 开锁器 / RGB").arg(sw));
                }
                appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
                          QString("OK Addr=0x%1 HW=%2 SW=%3").arg(hex2(addr)).arg(hw, sw));
            } else {
                QString why = infoFrame.valid
                    ? QString("FC=0x%1 data=%2B result=%3")
                          .arg(hex2(infoFrame.fc)).arg(infoFrame.data.size())
                          .arg(infoFrame.data.isEmpty() ? "?" : hex2(static_cast<int>(infoFrame.data[0])))
                    : QString("解析失败: %1").arg(infoFrame.error);
                appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
                          QString("设备信息失败: %1").arg(why));
            }
        } else {
            appendSystemLog(QString("[设备信息] 超时 3s 无响应 (rxBuf=%1字节)").arg(m_rxBuffer.size()),
                            QColor("#d08020"));
        }
        m_rxBuffer.clear();
    }
}

// ============================ Send ============================

void MainWindow::sendData()
{
    if (!m_transport || !m_transport->isOpen()) return;
    QString text = m_sendEdit->toPlainText();
    if (text.isEmpty()) return;

    QByteArray data;
    if (m_sendHexCheck->isChecked()) {
        QString hex = text;
        hex.remove(' ');
        data = QByteArray::fromHex(hex.toUtf8());
    } else {
        data = text.toUtf8();
    }

    m_transport->writeData(data);
    appendLog("[TX]", data, QColor("#c050a0"));  // 自定义帧: 红紫色

    // 一问一答: USB 模式等响应 2s, 串口模式不等
    if (m_transportType != TransportType::Usb) return;

    QElapsedTimer t; t.start();
    while (t.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        int prev = m_rxBuffer.size();
        if (m_transport) {
            QByteArray polled = m_transport->readAll();
            if (!polled.isEmpty())
                m_rxBuffer.append(polled);
        }
        if (t.elapsed() > 200 && m_rxBuffer.size() == prev)
            break;
    }
    // 显示完整接收字节(不截断第一帧, 设备可能附带额外 data)
    if (!m_rxBuffer.isEmpty())
        appendLog("[RX]", m_rxBuffer, QColor("#30a050"));
    else
        appendSystemLog("[发送后] 2s 内无响应");
    m_rxBuffer.clear();
}

// ============================ Receive ============================

void MainWindow::onReadyRead()
{
    if (!m_transport) return;
    QByteArray data = m_transport->readAll();
    m_rxBuffer.append(data);
    appendLog("[RX]", data, QColor("#30b078"));
}

void MainWindow::onPortError()
{
    if (!m_transport) return;
    // 升级流程中 setReport 失败是预期(EXEC 后设备软复位/旧 handle 失效), 静默不弹错误;
    // 升级结束由握手结果判定成功与否, 此处噪音会误导.
    if (m_upgrading) return;
    statusBar()->showMessage(tr("通信错误: %1").arg(m_transport->errorString()), 5000);
}

// ============================ Sccd Area ============================

void MainWindow::onSccdToggle(bool checked)
{
    // 切换 Sccd Area 显示/隐藏 + 切换 Sccd 按钮底色
    if (m_sccdScroll) m_sccdScroll->setVisible(checked);   // v2 兼容: scroll 已取消
    m_sccdBox->setVisible(checked);
    // 按钮底色: 关闭 = 默认紫; 展开 = 高亮绿
    m_sccdBtn->setProperty("active", checked);
    m_sccdBtn->style()->unpolish(m_sccdBtn);
    m_sccdBtn->style()->polish(m_sccdBtn);
    if (checked)
        statusBar()->showMessage(tr("Sccd Area 已展开"), 2000);
    else
        statusBar()->showMessage(tr("Sccd Area 已隐藏"), 2000);
    // 优化建议③: 窗口标题栏显示当前展开设备区
    refreshWindowTitleArea();
}

void MainWindow::onZlrToggle(bool checked)
{
    // 切换 ZLR5401 Area 显示/隐藏 + 切换 ZLR5401 按钮底色 (参照 onSccdToggle)
    if (m_zlrScroll) m_zlrScroll->setVisible(checked);   // v2 兼容
    m_zlrBox->setVisible(checked);
    m_zlrBtn->setProperty("active", checked);
    m_zlrBtn->style()->unpolish(m_zlrBtn);
    m_zlrBtn->style()->polish(m_zlrBtn);
    if (checked)
        statusBar()->showMessage(tr("ZLR5401 Area 已展开"), 2000);
    else
        statusBar()->showMessage(tr("ZLR5401 Area 已隐藏"), 2000);
    refreshWindowTitleArea();
}

// Round029 优化建议③: 窗口标题栏追加当前展开的设备区, 收缩/展开即时刷新
void MainWindow::refreshWindowTitleArea()
{
    QStringList areas;
    if (m_sccdBtn && m_sccdBtn->isChecked()) areas << "Sccd";
    if (m_zlrBtn && m_zlrBtn->isChecked()) areas << "ZLR5401";
    setWindowTitle(tr("SwComForTool %1%2")
                       .arg(COMFORTOOL_VERSION)
                       .arg(areas.isEmpty() ? QString() : tr(" (%1)").arg(areas.join(" + "))));
}

// Round029 优化建议①: ZLR Area 右键菜单, 恢复被精简区域 (默认隐藏, 仅调试/临时启用)
//   复位: 二次右键 + 勾选/取消勾选, 立即生效. 配置 + 控制 3 按钮 + 标签列表 + 输出区始终保留.
void MainWindow::showZlrTrimRestoreMenu()
{
    QMenu menu(this);
    auto addToggle = [&](const QString &label, QWidget *w) {
        QAction *a = menu.addAction(label);
        a->setCheckable(true);
        if (w) {
            a->setChecked(w->isVisible());
            connect(a, &QAction::toggled, this, [this, a, w](bool on){
                w->setVisible(on);
                // 读写区需恢复按钮/配置联动
                QGroupBox *box = qobject_cast<QGroupBox *>(w);
                if (box == m_zlrUhfRwBox) {
                    if (m_zlrUhfReadBtn)  m_zlrUhfReadBtn->setEnabled(on);
                    if (m_zlrUhfWriteBtn) m_zlrUhfWriteBtn->setEnabled(on);
                    if (m_zlrUhfBank) m_zlrUhfBank->setEnabled(on);
                    if (m_zlrUhfAddr) m_zlrUhfAddr->setEnabled(on);
                    if (m_zlrUhfCnt)  m_zlrUhfCnt->setEnabled(on);
                    if (m_zlrUhfData) m_zlrUhfData->setEnabled(on);
                    if (m_zlrUhfSetCfgBtn) m_zlrUhfSetCfgBtn->setEnabled(on);
                    if (m_zlrUhfGetCfgBtn) m_zlrUhfGetCfgBtn->setEnabled(on);
                    box->setTitle(on ? tr("读写") : tr("读写 - 已封禁"));
                }
                // 锁定操作: 清单区隐藏后 "开始" 失效, 恢复时联动启用
                if (box == m_zlrLockerLstBox && m_zlrLockerStartBtn) {
                    m_zlrLockerStartBtn->setEnabled(on);
                    m_zlrLockerStartBtn->setToolTip(
                        on ? QString() : tr("编排模式入口已隐藏 (CONFIGURE/ADD 在清单区), 解锁请走 UNLOCK_MULTI"));
                }
                a->setChecked(on);
            });
        }
    };
    addToggle("UHF 配置区", m_zlrUhfCfgBox);   // Round029 v5: 隐藏配置区
    addToggle("UHF 扫描区", m_zlrUhfScanBox);
    addToggle("UHF 读写区", m_zlrUhfRwBox);
    addToggle("AM 监控/波形区", m_zlrAmSubTabs);   // Round030: 子标签整体切换
    addToggle("AM 输出区", m_zlrAmOutBox);
    addToggle("开锁器 操作区", m_zlrLockerOpBox);   // Round029 v5: 隐藏操作区
    addToggle("开锁器 清单区", m_zlrLockerLstBox);
    menu.exec(QCursor::pos());
}

// ===== ZLR5401 通用子命令收发 =====
// 打包 [cmd][argData] → makeFrame(fc, subCmd+argData) → 发 → 等响应 → 校验 func==fc^0xFF
//   → data[0]==subCmd → 打印 data[1] err (0=OK) → outPayload=data.mid(2)
bool MainWindow::sendZlrSubCmd(quint8 fc, quint8 subCmd, const QByteArray &argData,
                               QByteArray *outPayload, QString *outErrNote, int timeoutMs,
                               bool clearRxBuffer, bool keepWaitingOnNonMatch, quint8 *outErrCode,
                               const std::function<void(const QByteArray &)> &onEventFrame)
{
    if (!m_transport || !m_transport->isOpen()) {
        if (outErrNote) *outErrNote = "请先打开通信接口";
        return false;
    }
    QByteArray data;
    data.append(static_cast<char>(subCmd));
    data.append(argData);
    QByteArray req = m_parser->makeFrame(m_deviceAddr, fc, data);

    if (clearRxBuffer) m_rxBuffer.clear();
    m_transport->writeData(req);
    m_transport->waitForBytesWritten(5000);
    // Round031: 日志只显示全部线上字节 — USB HID 传输层在帧前自动加报告 ID 0x02,
    // TX 与 RX 一并显示 (RX 的 02 前缀来自 64B 报告缓冲首字节), 帧本体从 53 77 起
    QByteArray txWire = req;
    if (m_transportType == TransportType::Usb) txWire.prepend(static_cast<char>(0x02));
    appendLog("[SYS][TX]", txWire, QColor("#c050a0"));

    // Round029: 等响应后扫 buffer 找匹配 subCmd, 未匹配帧保留 buffer 留给上层
    //   keepWaitingOnNonMatch=true (UNLOCK_MULTI 长等待): 头帧非目标(如迟到的 0x09 进度响应/推送事件帧)
    //     → 丢弃该帧继续等剩余超时, 避免被无关帧"假唤醒"后立即失败
    //   keepWaitingOnNonMatch=false (普通调用): 头帧不匹配 → 保留 buffer, 返回 false
    int waitMs = timeoutMs > 0 ? timeoutMs : 3000;
    QElapsedTimer deadline;
    deadline.start();
    int total = 0;   // 匹配帧总长 (含 0x02 报告 ID 前缀), break 后解析时用
    bool haveNewData = false;   // keepWaiting 丢弃非目标帧后 buffer 可能已含目标帧, 立即重扫不空等
    while (true) {
        if (!haveNewData) {
            int remain = waitMs - static_cast<int>(deadline.elapsed());
            if (remain <= 0 || !waitForResponse(remain)) {
                if (outErrNote) *outErrNote = QString("超时 %1ms 无响应 (rxBuf=%2字节)").arg(waitMs).arg(m_rxBuffer.size());
                return false;
            }
        }
        haveNewData = false;
        // 扫 buffer 头帧: fc==fc^0xFF 且 subCmd 匹配
        int scanOff = 0;
        if (!m_rxBuffer.isEmpty() && static_cast<quint8>(m_rxBuffer[0]) == 0x02) scanOff = 1;
        if (m_rxBuffer.size() - scanOff < 11) {
            if (keepWaitingOnNonMatch) continue;          // 数据不足, 继续等后续字节
            if (outErrNote) *outErrNote = QString("帧长度不足: %1B").arg(m_rxBuffer.size());
            return false;
        }
        if (static_cast<quint8>(m_rxBuffer[scanOff]) != 0x53
            || static_cast<quint8>(m_rxBuffer[scanOff + 1]) != 0x77) {
            if (keepWaitingOnNonMatch) { m_rxBuffer.remove(0, 1); haveNewData = true; continue; }  // 帧头坏, 逐字节 resync
            if (outErrNote) *outErrNote = QString("帧头错误: 0x%1 0x%2")
                .arg(hex2(static_cast<int>(m_rxBuffer[scanOff]))).arg(hex2(static_cast<int>(m_rxBuffer[scanOff + 1])));
            return false;
        }
        quint16 length = static_cast<quint8>(m_rxBuffer[scanOff + 4])
                      | (static_cast<quint8>(m_rxBuffer[scanOff + 5]) << 8);
        quint8 gotFc = static_cast<quint8>(m_rxBuffer[scanOff + 6]);
        int dataLen = length - 1 - 4;
        if (dataLen < 0 || dataLen > 1024) {
            if (keepWaitingOnNonMatch) { m_rxBuffer.remove(0, 1); haveNewData = true; continue; }  // length 异常, resync
            if (outErrNote) *outErrNote = QString("length 异常: %1").arg(length);
            return false;
        }
        total = 7 + dataLen + 4 + scanOff;
        if (m_rxBuffer.size() < total) {
            if (keepWaitingOnNonMatch) continue;          // 帧不完整, 继续等后续字节
            if (outErrNote) *outErrNote = QString("帧长度不足: 期望%1 实际%2")
                .arg(total).arg(m_rxBuffer.size());
            return false;
        }
        if (gotFc != static_cast<quint8>(fc ^ 0xFF)) {
            if (keepWaitingOnNonMatch) {
                if (onEventFrame) onEventFrame(m_rxBuffer.left(total));   // Round105: 完整帧交回调 (UNLOCK_MULTI 推送事件)
                m_rxBuffer.remove(0, total); haveNewData = true; continue; }  // 非目标帧, 丢弃重扫
            // 不消费 buffer, 返回错; 上层可自行扫 buffer
            QByteArray nonMatchFrame = m_rxBuffer.left(total);
            m_rxBuffer.remove(0, total);
            m_rxBuffer.prepend(nonMatchFrame);  // 保留
            if (outErrNote) *outErrNote = QString("FC=0x%1 (期望 0x%2) — 帧已保留 buffer")
                .arg(hex2(gotFc)).arg(hex2(fc ^ 0xFF));
            return false;
        }
        // fc 匹配, subCmd 必须匹配
        quint8 gotSubCmd = static_cast<quint8>(m_rxBuffer[scanOff + 7]);
        if (gotSubCmd != subCmd) {
            if (keepWaitingOnNonMatch) {
                if (onEventFrame) onEventFrame(m_rxBuffer.left(total));   // Round105: 完整帧交回调 (0x0B~0x0F 推送事件)
                m_rxBuffer.remove(0, total); haveNewData = true; continue; }  // 如 0x09 迟到响应/事件帧, 丢弃重扫
            // 非目标 subCmd (如 0x09 GET_PROGRESS 夹在 0x08 流程中), 保留 buffer
            QByteArray nonMatchFrame = m_rxBuffer.left(total);
            m_rxBuffer.remove(0, total);
            m_rxBuffer.prepend(nonMatchFrame);
            if (outErrNote) *outErrNote = QString("响应 subCmd=0x%1 (期望 0x%2) — 帧已保留 buffer")
                .arg(hex2(gotSubCmd)).arg(hex2(subCmd));
            return false;
        }
        break;   // fc + subCmd 全匹配, 走下方解析
    }
    // fc + subCmd 全匹配: 截取 frame 解析
    QByteArray frame = m_rxBuffer.left(total);
    FrameData parsed = m_parser->parseFrame(frame);
    m_rxBuffer.remove(0, total);
    if (!parsed.valid) {
        if (outErrNote) *outErrNote = QString("帧解析失败: %1").arg(parsed.error);
        return false;
    }
    quint8 err = static_cast<quint8>(parsed.data[1]);
    appendLog("[SYS][RX]", frame, QColor("#30b078"));   // Round031: 只显示帧, 不加描述
    if (err != 0) {
        if (outErrCode) *outErrCode = err;   // Round_101: err 码透传给上层做友好显示 (如 UNLOCK_MULTI err=11 NO_IR)
        if (outErrNote) *outErrNote = QString("失败 err=0x%1").arg(hex2(err));
        return false;
    }
    if (outPayload) *outPayload = parsed.data.mid(2);
    return true;
}

// 电机 MOVE 公共流程: 先设速度/转矩(可选) → 发 MOVE
//   dir: 0=正转(CW) 1=反转(CCW). 角度 0=持续运行(steps=0)
void MainWindow::onMotorMoveCw() { onMotorMove(0); }
void MainWindow::onMotorMoveCcw() { onMotorMove(1); }
void MainWindow::onMotorMove(int dir)
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("电机: 请先连接", QColor("#c050a0"));
        return;
    }
    // 1) 先下发 SPEED (速度微步/s, 16bit LE)
    if (m_zlrMotorSpeed) {
        quint16 hz = static_cast<quint16>(m_zlrMotorSpeed->value());
        QByteArray arg;
        arg.append(static_cast<char>(hz & 0xFF));
        arg.append(static_cast<char>((hz >> 8) & 0xFF));
        QString err;
        if (!sendZlrSubCmd(0x20, 0x03, arg, nullptr, &err))
            appendSystemLog(QString("电机 设速度失败: %1").arg(err), QColor("#d08020"));
    }
    // 2) 角度 → 微步 (0=持续运行)
    quint32 steps = 0;
    if (m_zlrMotorAngle && m_zlrMotorAngle->value() > 0) {
        quint32 stepsPerRev = m_zlrMotorSteps ? static_cast<quint32>(m_zlrMotorSteps->value()) : 3200;
        double frac = m_zlrMotorAngle->value() / 360.0;
        steps = static_cast<quint32>(frac * static_cast<double>(stepsPerRev) + 0.5);
        if (steps == 0) steps = 1; // 极小角度至少走 1 微步
        if (steps > 0xFFFFFF) {    // MOVE steps 为 24bit, 超限封顶并提示
            steps = 0xFFFFFF;
            appendSystemLog("电机: 微步数超过 24bit 上限, 已封顶为 0xFFFFFF", QColor("#d08020"));
        }
    }
    // 3) MOVE [dir, steps 24bit LE]
    QByteArray arg;
    arg.append(static_cast<char>(dir));
    arg.append(static_cast<char>(steps & 0xFF));
    arg.append(static_cast<char>((steps >> 8) & 0xFF));
    arg.append(static_cast<char>((steps >> 16) & 0xFF));
    QString err;
    if (sendZlrSubCmd(0x20, 0x01, arg, nullptr, &err)) {
        // steps=0 表示持续运行, 否则按步/转 换算实际圈数与角度闭环
        int spr = m_zlrMotorSteps ? m_zlrMotorSteps->value() : 400;
        QString info = (steps == 0)
            ? QString("电机: %1 开始 (持续运行)").arg(dir == 0 ? "正转" : "反转")
            : QString("电机: %1 开始, %2微步 (设定 %3° → 实际 %4°)")
                  .arg(dir == 0 ? "正转" : "反转")
                  .arg(steps)
                  .arg(m_zlrMotorAngle->value())
                  .arg(qRound(steps * 360.0 / static_cast<double>(spr)));
        appendSystemLog(info, QColor("#50a050"));
    } else {
        appendSystemLog(QString("电机 启动失败: %1").arg(err), QColor("#c050a0"));
    }
}

void MainWindow::onMotorStop()
{
    QString err;
    if (sendZlrSubCmd(0x20, 0x02, QByteArray(), nullptr, &err))
        appendSystemLog("电机: 已停止", QColor("#50a050"));
    else
        appendSystemLog(QString("电机 停止失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onMotorSpeed()
{
    if (!m_zlrMotorSpeed) return;
    quint16 hz = static_cast<quint16>(m_zlrMotorSpeed->value());
    QByteArray arg;
    arg.append(static_cast<char>(hz & 0xFF));
    arg.append(static_cast<char>((hz >> 8) & 0xFF));
    QString err;
    if (sendZlrSubCmd(0x20, 0x03, arg, nullptr, &err))
        appendSystemLog(QString("电机: 速度已设 %1 微步/s").arg(hz), QColor("#50a050"));
    else
        appendSystemLog(QString("电机 设速度失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onMotorTorque()
{
    if (!m_zlrMotorTorque) return;
    QByteArray arg;
    arg.append(static_cast<char>(m_zlrMotorTorque->value()));
    QString err;
    if (sendZlrSubCmd(0x20, 0x04, arg, nullptr, &err))
        appendSystemLog(QString("电机: 转矩已设 %1%%").arg(m_zlrMotorTorque->value()), QColor("#50a050"));
    else
        appendSystemLog(QString("电机 设转矩失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onMotorQuery()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x20, 0x05, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("电机 查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [state, fault, diag1, diag2, stepsDoneL, stepsDoneH]
    QStringList info;
    if (payload.size() >= 6) {
        quint8 st = static_cast<quint8>(payload[0]);
        quint8 flt = static_cast<quint8>(payload[1]);
        quint8 d1 = static_cast<quint8>(payload[2]);
        quint8 d2 = static_cast<quint8>(payload[3]);
        quint16 done = static_cast<quint8>(payload[4]) | (static_cast<quint8>(payload[5]) << 8);
        // 协议 §10.2 语义由 testState 决定, 上位机只显示状态码值, 详细解读由日志与协议共同负责
        info << QString("state=%1(%2)").arg(st).arg(st == 0 ? "IDLE" : st == 1 ? "RUN" : "FAULT");
        info << QString("fault=0x%1").arg(flt, 2, 16, QChar('0'));
        info << QString("diag1=0x%1 diag2=0x%2").arg(d1, 2, 16, QChar('0')).arg(d2, 2, 16, QChar('0'));
        info << QString("stepsDone=%1").arg(done);
    } else {
        info << QString("payload=%1B").arg(payload.size());
    }
    // 状态显示已移除 (Round026追加), 结果渲染到输出区
    if (m_zlrMotorOut) m_zlrMotorOut->append(info.join("  "));
    appendSystemLog(QString("电机 状态: %1").arg(info.join("  ")), QColor("#50a050"));
}

void MainWindow::onMotorClear()
{
    QString err;
    if (sendZlrSubCmd(0x20, 0x06, QByteArray(), nullptr, &err))
        appendSystemLog("电机: 已清除故障", QColor("#50a050"));
    else
        appendSystemLog(QString("电机 清除故障失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onMotorTest()
{
    // TEST: [passes]; 进行中再下发回 MOTOR_ERR_BUSY(3)
    quint8 passes = m_zlrMotorTestPasses ? static_cast<quint8>(m_zlrMotorTestPasses->value()) : 1;
    QByteArray arg;
    arg.append(static_cast<char>(passes));
    QString err;
    if (sendZlrSubCmd(0x20, 0x07, arg, nullptr, &err))
        appendSystemLog(QString("电机: 行程测试已启动 (%1次往返)").arg(passes), QColor("#50a050"));
    else
        appendSystemLog(QString("电机 行程测试失败: %1 (进行中会返回 BUSY)").arg(err), QColor("#c050a0"));
}

// HEALTH: [err, olovState, threshL,threshH(2B LE), trqL,trqH(2B LE), reason]
void MainWindow::onMotorHealth()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x20, 0x08, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("电机 健康查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QStringList v;
    if (payload.size() >= 6) {
        quint8 olov = static_cast<quint8>(payload[0]);
        quint16 thresh = static_cast<quint8>(payload[1]) | (static_cast<quint8>(payload[2]) << 8);
        quint16 trq = static_cast<quint8>(payload[3]) | (static_cast<quint8>(payload[4]) << 8);
        quint8 reason = static_cast<quint8>(payload[5]);
        static const char *olovName[] = {"正常","持续高负载","已自动降速","高负载停机"};
        static const char *reasonName[] = {"无","正常停止","运行时限","高负载停机","DRV故障"};
        v << QString("高负载=%1(%2)").arg(olov).arg(olov <= 3 ? olovName[olov] : "?");
        v << QString("阈值=%1").arg(thresh);
        v << QString("转矩=%1(%2)").arg(trq).arg(trq > 3000 ? "≈无负载" : trq > 1500 ? "正常" : trq > 0 ? "偏重" : "极重/失速");
        v << QString("停机原因=%1").arg(reason <= 4 ? reasonName[reason] : QString("0x%1").arg(hex2(reason)));
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrMotorOut) m_zlrMotorOut->append(v.join("  "));
    appendSystemLog(QString("电机 健康: %1").arg(v.join("  ")), QColor("#50a050"));
}

// STATS: [err, runSeconds(3B LE), startCount(2B LE), lastReason]
void MainWindow::onMotorStats()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x20, 0x09, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("电机 统计查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QStringList v;
    if (payload.size() >= 6) {
        quint32 secs = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8)
                     | (static_cast<quint8>(payload[2]) << 16);
        quint16 starts = static_cast<quint8>(payload[3]) | (static_cast<quint8>(payload[4]) << 8);
        quint8 reason = static_cast<quint8>(payload[5]);
        static const char *reasonName[] = {"无","正常停止","运行时限","高负载停机","DRV故障"};
        v << QString("累计运行=%1s (%2h%3m)").arg(secs).arg(secs / 3600).arg((secs % 3600) / 60);
        v << QString("启动=%1次").arg(starts);
        v << QString("最近停止=%1").arg(reason <= 4 ? reasonName[reason] : QString("0x%1").arg(hex2(reason)));
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrMotorOut) m_zlrMotorOut->append(v.join("  "));
    appendSystemLog(QString("电机 统计: %1").arg(v.join("  ")), QColor("#50a050"));
}

// ===== UHF (FC=0x21) =====
void MainWindow::onUhfOpen()
{
    QString err;
    if (sendZlrSubCmd(0x21, 0x01, QByteArray(), nullptr, &err))
        appendSystemLog("UHF: 已开启 (READY)", QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 开启失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onUhfClose()
{
    QString err;
    if (sendZlrSubCmd(0x21, 0x02, QByteArray(), nullptr, &err))
        appendSystemLog("UHF: 已关闭", QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 关闭失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onUhfInventory()
{
    // INVENTORY 同步阻塞: 请求带 timeout(ms, 缺省1000), 标签内联在响应带回 [err, countL,H, (rssi, epcLen, epc..)...]
    QByteArray arg;
    int timeout = m_zlrUhfCycle ? m_zlrUhfCycle->value() : 1000;
    arg.append(static_cast<char>(timeout & 0xFF));
    arg.append(static_cast<char>((timeout >> 8) & 0xFF));
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x03, arg, &payload, &err)) {
        // err=0x05 (NO_TAG) 属于正常"无标签", 单独友好提示
        if (err.contains("0x05"))
            appendSystemLog("UHF 盘点: 场内无标签", QColor("#d08020"));
        else
            appendSystemLog(QString("UHF 盘点失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [count L, count H, (rssi, epcLen, epc..)...]
    QStringList rows;
    QSet<QString> seen;
    if (payload.size() >= 2) {
        int count = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8);
        int i = 2;
        int idx = 0;
        while (i + 2 <= payload.size() && idx < count) {
            quint8 rssi = static_cast<quint8>(payload[i]);
            int epcLen = static_cast<quint8>(payload[i + 1]);
            i += 2;
            if (i + epcLen > payload.size()) break;
            QByteArray epc = payload.mid(i, epcLen);
            i += epcLen;
            QString e = QString::fromLatin1(epc.toHex()).toUpper();
            if (seen.contains(e)) continue;   // 同 EPC 去重
            seen.insert(e);
            rows << QString("%1|%2|%3").arg(e).arg(rssi).arg(++idx);
        }
    }
    if (m_zlrUhfTagTable) {
        m_zlrUhfTagTable->setRowCount(rows.size());
        for (int r = 0; r < rows.size(); ++r) {
            QStringList c = rows[r].split('|');
            for (int col = 0; col < 3 && col < c.size(); ++col)
                m_zlrUhfTagTable->setItem(r, col, new QTableWidgetItem(c[col]));
        }
        if (rows.isEmpty()) m_zlrUhfTagTable->setRowCount(0);
    }
    // Round031: 盘点结果只进标签表格, 成功描述不再进主日志 (失败/无标签提示保留)
}

void MainWindow::onUhfQuery()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x07, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("UHF 查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [state, link, totalTags]
    QStringList info;
    if (payload.size() >= 3) {
        quint8 st = static_cast<quint8>(payload[0]);
        quint8 link = static_cast<quint8>(payload[1]);
        quint8 total = static_cast<quint8>(payload[2]);
        static const char *stName[] = {"IDLE","READY","INVENTORY","READ","WRITE","ERROR","ANT_CHECK","GETBUF"};
        info << QString("state=%1(%2)").arg(st).arg(st <= 7 ? stName[st] : "?");
        info << QString("link=%1").arg(link == 0 ? "正常" : link == 1 ? "超时" : "CRC错");
        info << QString("totalTags=%1").arg(total);
    } else {
        info << QString("payload=%1B").arg(payload.size());
    }
    if (m_zlrUhfOut) m_zlrUhfOut->append(info.join("  "));
    appendSystemLog(QString("UHF 状态: %1").arg(info.join("  ")), QColor("#50a050"));
}

void MainWindow::onUhfGetTags()
{
    QByteArray payload;
    QString err;
    // GET_TAGS: [count]; count=0 取全部
    if (!sendZlrSubCmd(0x21, 0x0A, QByteArray(1, '\0'), &payload, &err)) {
        appendSystemLog(QString("UHF 取标签失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [totalL, totalH, (epcLen, rssi, epc..)...]
    // 解析到标签表格 (EPC, RSSI, 序号); 相同 EPC 去重只保留一行; 双击 EPC 可填入输入框
    int total = 0;
    QStringList rows;
    QSet<QString> seenEpc;   // 已加入的 EPC (去重)
    int dropped = 0;
    if (payload.size() >= 2) {
        total = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8);
        int i = 2;
        int idx = 0;
        while (i + 2 <= payload.size()) {
            int epcLen = static_cast<quint8>(payload[i]);
            quint8 rssi = static_cast<quint8>(payload[i + 1]);
            i += 2;
            if (i + epcLen > payload.size()) break;
            QByteArray epc = payload.mid(i, epcLen);
            i += epcLen;
            QString epcS = QString::fromLatin1(epc.toHex()).toUpper();
            if (seenEpc.contains(epcS)) { ++dropped; continue; }  // 重复 EPC 跳过
            seenEpc.insert(epcS);
            rows << QString("%1|%2|%3").arg(epcS, 2).arg(rssi).arg(++idx);
        }
    }
    if (m_zlrUhfTagTable) {
        m_zlrUhfTagTable->setRowCount(rows.size());
        for (int r = 0; r < rows.size(); ++r) {
            QStringList c = rows[r].split('|');
            for (int col = 0; col < 3 && col < c.size(); ++col)
                m_zlrUhfTagTable->setItem(r, col, new QTableWidgetItem(c[col]));
        }
        if (rows.isEmpty())
            m_zlrUhfTagTable->setRowCount(0);
    }
    appendSystemLog(QString("UHF 取标签: 设备返回%1条, 去重%2条, 表格%3行")
                        .arg(total).arg(dropped).arg(rows.size()), QColor("#50a050"));
}

void MainWindow::onUhfGetConfig()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x08, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("UHF 读配置失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // [powerDbm, antenna, checksumEn, session, target, q, band]
    QStringList v;
    if (payload.size() >= 6) {
        v << QString("power=%1dBm").arg(static_cast<quint8>(payload[0]));
        v << QString("ant=%1").arg(static_cast<quint8>(payload[1]));
        v << QString("crc=%1").arg(static_cast<quint8>(payload[2]));
        v << QString("session=%1").arg(static_cast<quint8>(payload[3]));
        v << QString("target=%1").arg(static_cast<quint8>(payload[4]));
        v << QString("q=%1").arg(static_cast<quint8>(payload[5]));
        if (payload.size() >= 7) {
            quint8 band = static_cast<quint8>(payload[6]);
            v << QString("band=0x%1(%2)").arg(band, 2, 16, QChar('0'))
                .arg(band == 0x01 ? "北美" : band == 0x06 ? "中国1" : band == 0x08 ? "CE_LOW" : band == 0xFF ? "全频段" : "?");
        }
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrUhfOut) m_zlrUhfOut->append(v.join("  "));
    appendSystemLog(QString("UHF 配置: %1").arg(v.join("  ")), QColor("#50a050"));
}

void MainWindow::onUhfSetConfig()
{
    // SET_CONFIG: [powerDbm, antenna, checksumEn, session, target, q, band]
    quint8 band = m_zlrUhfBand ? static_cast<quint8>(m_zlrUhfBand->currentData().toUInt()) : 0x01;
    QByteArray arg;
    arg.append(static_cast<char>(m_zlrUhfPower->value()));
    arg.append(static_cast<char>(0));            // antenna
    arg.append(static_cast<char>(1));            // checksumEn
    arg.append(static_cast<char>(0));            // session S0
    arg.append(static_cast<char>(0));            // target A
    arg.append(static_cast<char>(0));            // q=动态
    arg.append(static_cast<char>(band));         // Region(新增 band)
    QString err;
    if (sendZlrSubCmd(0x21, 0x09, arg, nullptr, &err))
        appendSystemLog(QString("UHF: 配置已保存 (power=%1dBm band=0x%2)").arg(m_zlrUhfPower->value()).arg(band, 2, 16, QChar('0')), QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 设配置失败: %1").arg(err), QColor("#c050a0"));
}

// UHF GET_STATUS: [state, link, totalTags, powered, antennaOk, lastErr, antRl H, antRl L, antVswr H, antVswr L]
void MainWindow::onUhfGetStatus()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x0B, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("UHF 读状态失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QStringList v;
    if (payload.size() >= 10) {
        quint8 st = static_cast<quint8>(payload[0]);
        quint8 link = static_cast<quint8>(payload[1]);
        quint8 total = static_cast<quint8>(payload[2]);
        quint8 powered = static_cast<quint8>(payload[3]);
        quint8 antOk = static_cast<quint8>(payload[4]);
        quint8 lastErr = static_cast<quint8>(payload[5]);
        quint16 rl = static_cast<quint8>(payload[6]) << 8 | static_cast<quint8>(payload[7]);
        quint16 vswr = static_cast<quint8>(payload[8]) << 8 | static_cast<quint8>(payload[9]);
        static const char *stName[] = {"IDLE","READY","INVENTORY","READ","WRITE","ERROR","ANT_CHECK"};
        v << QString("state=%1(%2)").arg(st).arg(st <= 6 ? stName[st] : "?");
        v << QString("link=%1").arg(link == 0 ? "正常" : link == 1 ? "超时" : "CRC错");
        v << QString("powered=%1").arg(powered ? "是" : "否");
        v << QString("antOk=%1").arg(antOk ? "正常" : "不良");
        v << QString("lastErr=%1").arg(lastErr);
        v << QString("总标签=%1").arg(total);
        v << QString("RL=%1").arg(rl == 0 ? "未测" : QString("%1 dB").arg(rl / 10.0, 0, 'f', 1));
        v << QString("VSWR=%1").arg(vswr == 0 ? "未测" : QString("%1").arg(vswr / 100.0, 0, 'f', 2));
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrUhfOut) m_zlrUhfOut->append(v.join("  "));
    // Round031: 成功描述不再进主日志 (结果在 UHF 输出区), 失败仍保留
}

// UHF CHECK_ANT: [antennaOk, antRl H, antRl L, antVswr H, antVswr L]
void MainWindow::onUhfCheckAnt()
{
    QByteArray payload;
    QString err;
    // Round031: CHECK_ANT 为同步等待上报式 — 一条命令设备内部跑完回波检测全流程后结果内联返回,
    // 无主动上报; 实测流程 ~1.8s, 超时放宽至 5s 覆盖天线不良等慢路径
    if (!sendZlrSubCmd(0x21, 0x0C, QByteArray(), &payload, &err, 5000)) {
        appendSystemLog(QString("UHF 天线检测失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QString out;
    if (payload.size() >= 5) {
        quint8 antOk = static_cast<quint8>(payload[0]);
        quint16 rl = static_cast<quint8>(payload[1]) << 8 | static_cast<quint8>(payload[2]);
        quint16 vswr = static_cast<quint8>(payload[3]) << 8 | static_cast<quint8>(payload[4]);
        out = QString("天线%1  RL=%2  VSWR=%3")
                  .arg(antOk ? "正常" : "不良")
                  .arg(rl == 0 ? "未测" : QString("%1 dB").arg(rl / 10.0, 0, 'f', 1))
                  .arg(vswr == 0 ? "未测" : QString("%1").arg(vswr / 100.0, 0, 'f', 2));
    } else out = "空响应";
    if (m_zlrUhfOut) m_zlrUhfOut->append("天线检测: " + out);
    // Round031: 成功描述不再进主日志 (结果在 UHF 输出区), 失败仍保留
}

// UHF READ_TAG: [epcLen, epc.., bank, addr, cnt]
void MainWindow::onUhfReadTag()
{
    QString hex = m_zlrUhfEpc ? m_zlrUhfEpc->text().trimmed() : QString();
    if (hex.isEmpty()) { appendSystemLog("UHF 读标签: 请先输入 EPC", QColor("#d08020")); return; }
    QByteArray epc = QByteArray::fromHex(hex.toLatin1());
    if (epc.isEmpty() || epc.size() > 12) { appendSystemLog("UHF 读标签: EPC 需为 1~12 字节 hex", QColor("#d08020")); return; }
    QByteArray arg;
    arg.append(static_cast<char>(epc.size()));
    arg.append(epc);
    arg.append(static_cast<char>(m_zlrUhfBank->value()));
    arg.append(static_cast<char>(m_zlrUhfAddr->value()));
    arg.append(static_cast<char>(m_zlrUhfCnt->value()));
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x04, arg, &payload, &err)) {
        appendSystemLog(QString("UHF 读标签失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QString dataHex = QString::fromLatin1(payload.toHex(' ')).toUpper().trimmed();
    if (m_zlrUhfOut) m_zlrUhfOut->append(QString("读标签 bank=%1 addr=%2 cnt=%3 → %4")
                                .arg(m_zlrUhfBank->value()).arg(m_zlrUhfAddr->value()).arg(m_zlrUhfCnt->value())
                                .arg(dataHex.isEmpty() ? "(空)" : dataHex));
    appendSystemLog(QString("UHF 读标签: %1").arg(dataHex.isEmpty() ? "(空)" : dataHex), QColor("#50a050"));
}

// UHF WRITE_TAG: [epcLen, epc.., bank, addr, len, data..]
void MainWindow::onUhfWriteTag()
{
    QString hex = m_zlrUhfEpc ? m_zlrUhfEpc->text().trimmed() : QString();
    if (hex.isEmpty()) { appendSystemLog("UHF 写标签: 请先输入 EPC", QColor("#d08020")); return; }
    QByteArray epc = QByteArray::fromHex(hex.toLatin1());
    if (epc.isEmpty() || epc.size() > 12) { appendSystemLog("UHF 写标签: EPC 需为 1~12 字节 hex", QColor("#d08020")); return; }
    QString dhex = m_zlrUhfData ? m_zlrUhfData->text().trimmed() : QString();
    QByteArray data = QByteArray::fromHex(dhex.toLatin1());
    if (data.isEmpty()) { appendSystemLog("UHF 写标签: 请先输入数据(hex)", QColor("#d08020")); return; }
    QByteArray arg;
    arg.append(static_cast<char>(epc.size()));
    arg.append(epc);
    arg.append(static_cast<char>(m_zlrUhfBank->value()));
    arg.append(static_cast<char>(m_zlrUhfAddr->value()));
    arg.append(static_cast<char>(data.size()));
    arg.append(data);
    QString err;
    if (sendZlrSubCmd(0x21, 0x05, arg, nullptr, &err))
        appendSystemLog(QString("UHF 写标签: bank=%1 addr=%2 len=%3B 完成").arg(m_zlrUhfBank->value()).arg(m_zlrUhfAddr->value()).arg(data.size()), QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 写标签失败: %1").arg(err), QColor("#c050a0"));
}

// UHF SCAN_START: [cycleL, cycleH] 自动扫描(连续盘点入缓冲, GET_TAGS 拉取)
void MainWindow::onUhfSetScan()
{
    QByteArray arg;
    int cycle = m_zlrUhfCycle ? m_zlrUhfCycle->value() : 1000;
    arg.append(static_cast<char>(cycle & 0xFF));
    arg.append(static_cast<char>((cycle >> 8) & 0xFF));
    QString err;
    if (sendZlrSubCmd(0x21, 0x0D, arg, nullptr, &err))
        appendSystemLog(QString("UHF: 自动扫描已启动 (周期%1ms, 用取标签拉取)").arg(cycle), QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 自动扫描失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onUhfScanStop()
{
    QString err;
    if (sendZlrSubCmd(0x21, 0x0E, QByteArray(), nullptr, &err))
        appendSystemLog("UHF: 自动扫描已停止", QColor("#50a050"));
    else
        appendSystemLog(QString("UHF 停止扫描失败: %1").arg(err), QColor("#c050a0"));
}

// UHF GET_DUMP: [cmd] → [cmd, err, data..] 0x22/0x29 原始字节诊断
void MainWindow::onUhfGetDump()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x21, 0x10, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("UHF 诊断失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QString hex = QString::fromLatin1(payload.toHex(' ')).toUpper().trimmed();
    if (m_zlrUhfOut) m_zlrUhfOut->append(QString("UHF DUMP (%1B): %2").arg(payload.size()).arg(hex.isEmpty() ? "(空)" : hex));
    appendSystemLog(QString("UHF DUMP (%1B)").arg(payload.size()), QColor("#50a050"));
}

// ===== AM (FC=0x22) =====
// 16bit 参数值高字节在后: (H<<8)|L
static quint16 zlrAmVal16(const QLineEdit *le, quint16 def = 0)
{
    if (!le) return def;
    bool ok = false;
    quint32 v = le->text().toUInt(&ok, 10);
    return ok ? static_cast<quint16>(v) : def;
}
void MainWindow::onAmGetConfig()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x22, 0x01, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("AM 读配置失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // [thrH,thrL, hitH,hitL, freq, delayH,delayL, len, invert, syncH,syncL, volt, mode]
    QStringList v;
    if (payload.size() >= 13) {
        auto v16 = [&](int hi, int lo){ return static_cast<quint8>(payload[hi])<<8 | static_cast<quint8>(payload[lo]); };
        v << QString("thr=%1").arg(v16(0, 1));
        v << QString("hit=%1").arg(v16(2, 3));
        v << QString("freq=%1").arg(static_cast<quint8>(payload[4]));
        v << QString("delay=%1").arg(v16(5, 6));
        v << QString("len=%1").arg(static_cast<quint8>(payload[7]));
        v << QString("invert=%1").arg(static_cast<quint8>(payload[8]));
        v << QString("sync=%1").arg(v16(9, 10));
        v << QString("volt=%1").arg(static_cast<quint8>(payload[11]));
        v << QString("mode=%1").arg(static_cast<quint8>(payload[12]));
        if (payload.size() >= 14)
            v << QString("mains=%1").arg(static_cast<quint8>(payload[13]) ? "60Hz" : "50Hz");
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrAmOut) m_zlrAmOut->append(v.join("  "));
    appendSystemLog(QString("AM 配置: %1").arg(v.join("  ")), QColor("#50a050"));
}

void MainWindow::onAmSetConfig()
{
    auto v16leH = [](quint16 v, QByteArray &d) {
        d.append(static_cast<char>((v >> 8) & 0xFF));
        d.append(static_cast<char>(v & 0xFF));
    };
    QByteArray d;
    v16leH(zlrAmVal16(m_zlrAmThr, 10), d);
    v16leH(zlrAmVal16(m_zlrAmHit, 5), d);
    d.append(static_cast<char>(m_zlrAmFreq ? m_zlrAmFreq->text().toUInt() : 0));
    v16leH(zlrAmVal16(m_zlrAmDelay, 0), d);
    d.append(static_cast<char>(m_zlrAmLen ? m_zlrAmLen->text().toUInt() : 0));
    d.append(static_cast<char>(m_zlrAmInvert ? m_zlrAmInvert->text().toUInt() : 0));
    v16leH(zlrAmVal16(m_zlrAmSync, 0), d);
    d.append(static_cast<char>(m_zlrAmVolt ? m_zlrAmVolt->text().toUInt() : 1));
    d.append(static_cast<char>(m_zlrAmMode ? m_zlrAmMode->text().toUInt() : 0));
    d.append(static_cast<char>(m_zlrAmMains ? m_zlrAmMains->text().toUInt() : 0));  // mains: 0=50Hz 1=60Hz
    QString err;
    if (sendZlrSubCmd(0x22, 0x02, d, nullptr, &err))
        appendSystemLog("AM: 配置已保存", QColor("#50a050"));
    else
        appendSystemLog(QString("AM 设配置失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onAmQuery()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x22, 0x05, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("AM 查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // [link]
    quint8 link = payload.isEmpty() ? 0xFF : static_cast<quint8>(payload[0]);
    QString s = link == 0 ? "链路正常" : (link == 4 ? "链路错误" : link == 6 ? "超时" : QString("link=0x%1").arg(hex2(link)));
    if (m_zlrAmOut) m_zlrAmOut->append(QString("AM 查询: %1").arg(s));
    appendSystemLog(QString("AM 查询: %1").arg(s), QColor(link == 0 ? "#50a050" : "#d08020"));
}

// AM GET_STATUS: 无 err 字段, 首字节即 link: [link, evt(4B LE), lastEventMs(4B LE)]
void MainWindow::onAmGetStatus()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x22, 0x06, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("AM 监控失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // sendZlrSubCmd 已剥掉 [cmd, link]: GET_STATUS 无独立 err, data[1]=link 被当作 err 校验,
    // link=0(正常) 时校验通过, outPayload=data.mid(2)=[evtL,evtH,evt3,evt4, lastL..last4] (8B)
    QString out;
    if (payload.size() >= 8) {
        quint32 evt = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8)
                    | (static_cast<quint8>(payload[2]) << 16) | (static_cast<quint8>(payload[3]) << 24);
        quint32 last = static_cast<quint8>(payload[4]) | (static_cast<quint8>(payload[5]) << 8)
                     | (static_cast<quint8>(payload[6]) << 16) | (static_cast<quint8>(payload[7]) << 24);
        out = QString("检测事件累计=%1  最近事件=%2ms").arg(evt).arg(last);
    } else out = QString("payload=%1B").arg(payload.size());
    if (m_zlrAmOut) m_zlrAmOut->append("AM 监控: " + out);
    appendSystemLog(QString("AM 监控: %1").arg(out), QColor("#50a050"));
}

// AM SET_MODE: [mode]
void MainWindow::onAmSetMode()
{
    QByteArray arg;
    arg.append(static_cast<char>(m_zlrAmMode ? m_zlrAmMode->text().toUInt() : 0));
    QString err;
    if (sendZlrSubCmd(0x22, 0x07, arg, nullptr, &err))
        appendSystemLog("AM: 工作模式已切换", QColor("#50a050"));
    else
        appendSystemLog(QString("AM 切模式失败: %1").arg(err), QColor("#c050a0"));
}

// AM GET_WAVE: 触发同步波形采集 (0x64, 阻塞~1s) [pointsH, pointsL]
void MainWindow::onAmGetWave()
{
    QByteArray payload;
    QString err;
    appendSystemLog("AM: 正在采集波形 (~1s)...", QColor("#d08020"));
    if (!sendZlrSubCmd(0x22, 0x08, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("AM 波形采集失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [pointsH, pointsL]
    if (payload.size() >= 2) {
        quint16 points = static_cast<quint8>(payload[0]) << 8 | static_cast<quint8>(payload[1]);
        if (m_zlrAmWaveOut) m_zlrAmWaveOut->append(QString("波形已采集, 总点数=%1 (4800 采样率)" ).arg(points));
        appendSystemLog(QString("AM: 波形采集完成, 总点数=%1").arg(points), QColor("#50a050"));
    } else {
        if (m_zlrAmWaveOut) m_zlrAmWaveOut->append(QString("波形采集完成 (points 未回, payload=%1B)").arg(payload.size()));
        appendSystemLog("AM: 波形采集完成", QColor("#50a050"));
    }
}

// AM GET_WAVE_PAGE: [page] → [page, err, pointsH, pointsL, w(≤48)]
void MainWindow::onAmGetWavePage()
{
    QByteArray arg;
    arg.append(static_cast<char>(m_zlrAmWavePage ? m_zlrAmWavePage->value() : 0));
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x22, 0x09, arg, &payload, &err)) {
        appendSystemLog(QString("AM 取波形页失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // payload: [pointsH, pointsL, w(≤48)]
    QString out;
    if (payload.size() >= 2) {
        quint16 points = static_cast<quint8>(payload[0]) << 8 | static_cast<quint8>(payload[1]);
        QByteArray w = (payload.size() > 2) ? payload.mid(2) : QByteArray();
        QString wave;
        for (int i = 0; i < w.size(); ++i) {
            if (!wave.isEmpty()) wave += " ";
            wave += QString("%1").arg(static_cast<quint8>(w[i]));
        }
        out = QString("页%1 总点%2 本页%3点\n").arg(m_zlrAmWavePage ? m_zlrAmWavePage->value() : 0).arg(points).arg(w.size()) + wave;
    } else out = QString("payload=%1B").arg(payload.size());
    if (m_zlrAmWaveOut) m_zlrAmWaveOut->append(out);
    appendSystemLog("AM 波形页: 取回", QColor("#50a050"));
}

// ===== 开锁器 (FC=0x23) =====
void MainWindow::onLockerConfigure()
{
    // CONFIGURE: [hardCntL, hardCntH, softCntL, softCntH] (hardCnt 仅预检, 传0)
    QByteArray d;
    quint32 soft = m_zlrLockerSoftCnt ? static_cast<quint32>(m_zlrLockerSoftCnt->value()) : 1;
    d.append(static_cast<char>(0)); d.append(static_cast<char>(0));
    d.append(static_cast<char>(soft & 0xFF));
    d.append(static_cast<char>((soft >> 8) & 0xFF));
    QString err;
    if (sendZlrSubCmd(0x23, 0x01, d, nullptr, &err))
        appendSystemLog(QString("开锁器: 清单已配置 (软标N=%1)").arg(soft), QColor("#50a050"));
    else
        appendSystemLog(QString("开锁器 配置失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onLockerAdd()
{
    // ADD: [epcLen, epc..]
    QString hex = m_zlrLockerHardEpc ? m_zlrLockerHardEpc->text().trimmed() : QString();
    if (hex.isEmpty()) { appendSystemLog("开锁器: 请先输入硬标签 EPC", QColor("#d08020")); return; }
    QByteArray epc = QByteArray::fromHex(hex.toLatin1());
    if (epc.isEmpty() || epc.size() > 12) {
        appendSystemLog("开锁器: EPC 需为 1~12 字节 hex", QColor("#d08020"));
        return;
    }
    QByteArray d;
    d.append(static_cast<char>(epc.size()));
    d.append(epc);
    QString err;
    if (sendZlrSubCmd(0x23, 0x02, d, nullptr, &err))
        appendSystemLog(QString("开锁器: 已追加硬标 %1").arg(QString::fromLatin1(epc.toHex()).toUpper()), QColor("#50a050"));
    else
        appendSystemLog(QString("开锁器 追加失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onLockerStart()
{
    QString err;
    if (sendZlrSubCmd(0x23, 0x03, QByteArray(), nullptr, &err))
        appendSystemLog("开锁器: 已开始 (上电UHF+开扫)", QColor("#50a050"));
    else
        appendSystemLog(QString("开锁器 开始失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onLockerCancel()
{
    QString err;
    if (sendZlrSubCmd(0x23, 0x04, QByteArray(), nullptr, &err))
        appendSystemLog("开锁器: 已取消", QColor("#50a050"));
    else
        appendSystemLog(QString("开锁器 取消失败: %1").arg(err), QColor("#c050a0"));
}

void MainWindow::onLockerQuery()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x23, 0x05, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("开锁器 查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // [state, hmL,hmH, scL,scH, suL,suH]
    static const char *stName[] = {"IDLE","CONFIGURED","UNLOCK_HOLD","SOFT_DECODE","DONE","FAULT"};
    QStringList v;
    if (payload.size() >= 7) {
        quint8 st = static_cast<quint8>(payload[0]);
        quint16 hm = static_cast<quint8>(payload[1]) | (static_cast<quint8>(payload[2]) << 8);
        quint16 sc = static_cast<quint8>(payload[3]) | (static_cast<quint8>(payload[4]) << 8);
        quint16 su = static_cast<quint8>(payload[5]) | (static_cast<quint8>(payload[6]) << 8);
        v << QString("state=%1(%2)").arg(st).arg(st <= 5 ? stName[st] : "?");
        v << QString("硬标解锁=%1").arg(hm);
        v << QString("软标=%1/%2").arg(su).arg(sc);
    } else v << QString("payload=%1B").arg(payload.size());
    if (m_zlrLockerOut) m_zlrLockerOut->append(v.join("  "));
    appendSystemLog(QString("开锁器 状态: %1").arg(v.join("  ")), QColor("#50a050"));
}

void MainWindow::onLockerGetEvent()
{
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x23, 0x07, QByteArray(), &payload, &err)) {
        appendSystemLog(QString("开锁器 取事件失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // [code, epcLen, epc.., hmL,hmH, suL,suH, scL,scH, hcL,hcH]
    static const char *evName[] = {"MATCH_OK","MISMATCH","HARD_DONE","SOFT_USED","TIMEOUT","DONE","FAULT"};
    QString out;
    if (payload.size() >= 1) {
        quint8 code = static_cast<quint8>(payload[0]);
        out += QString("事件 %1(%2)").arg(code).arg(code <= 6 ? evName[code] : "?");
        int i = 1;
        if (i < payload.size()) {
            int epcLen = static_cast<quint8>(payload[i]); i++;
            if (i + epcLen <= payload.size()) {
                QByteArray epc = payload.mid(i, epcLen); i += epcLen;
                out += QString(" EPC=%1").arg(QString::fromLatin1(epc.toHex()).toUpper(), 2);
            }
        }
    } else out = "空事件";
    if (m_zlrLockerOut) m_zlrLockerOut->append(out);
    appendSystemLog(QString("开锁器 事件: %1").arg(out), QColor("#50a050"));
}

// ===== Round027: 一键解锁 (ONE_SHOT) + 等待对话框 =====

// --- LockerWaitDialog impl ---
// Round032: 协议更新 — 弹窗不再显示进度动画, 也不再 5s 轮询 GET_PROGRESS;
//   进度全靠设备推送事件 (0x0B 确认/0x0C 失配/0x0D 硬标完成/0x0E 软标/0x0F 受理) 展示
LockerWaitDialog::LockerWaitDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("等待解锁中"));
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(360, 200);
    QPushButton *cancelBtn = new QPushButton(tr("停止"));
    cancelBtn->setObjectName("zlrMotorStop");   // 警示橙
    cancelBtn->setToolTip(tr("发 CANCEL (0x04) 流程中安全打断"));
    connect(cancelBtn, &QPushButton::clicked, this, &LockerWaitDialog::cancelRequested);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);
    lay->addStretch(1);
    auto *hint = new QLabel(tr("解锁进行中, 请稍候..."));
    hint->setAlignment(Qt::AlignCenter);
    lay->addWidget(hint);
    // Round105: 推送事件实时行 (UNLOCK_MULTI 0x0B 确认/0x0C 失配/0x0D 硬标完成/0x0E 软标/0x0F 受理)
    auto *eventLabel = new QLabel("");
    eventLabel->setObjectName("lwd_event");
    eventLabel->setAlignment(Qt::AlignCenter);
    eventLabel->setWordWrap(true);
    eventLabel->setStyleSheet("color:#2060a0; font-size:0.88em;");
    lay->addWidget(eventLabel);
    auto *finalLabel = new QLabel("");
    finalLabel->setObjectName("lwd_final");
    finalLabel->setAlignment(Qt::AlignCenter);
    finalLabel->setWordWrap(true);
    finalLabel->setStyleSheet("color:#503070; font-size:0.9em; font-weight:bold;");
    lay->addWidget(finalLabel);
    lay->addStretch(1);
    lay->addWidget(cancelBtn, 0, Qt::AlignCenter);
}

// Round_011: 原单标签 updateFinal (ONE_SHOT 终态) 已随 0x08 废除删除 — 终态统一走 updateFinalMulti
// Round105: UNLOCK_MULTI 终态 — endReason 语义: 1=ALL_OK 2=PARTIAL_TIMEOUT 4=UHF_LOST 6=ABORTED
// Round032: 协议 V2 新增 7=SOFT_TIMEOUT (软标窗 5min 满未校验完成)
void LockerWaitDialog::updateFinalMulti(int endReason, int confirmed, int total, int bitmap, int softDone, int softCnt)
{
    static const char *endNameMulti[] = {"?","ALL_OK","PARTIAL_TIMEOUT","?","UHF_LOST","?","ABORTED","SOFT_TIMEOUT"};
    const char *rTxt = (endReason >= 0 && endReason <= 7) ? endNameMulti[endReason] : "?";
    m_finalTxt = QString("完成: endReason=%1(%2) 确认 %3/%4 (位图 0b%5) 软标 %6/%7")
                     .arg(endReason).arg(rTxt).arg(confirmed).arg(total)
                     .arg(QString::number(bitmap, 2).rightJustified(total > 0 ? total : 1, '0'))
                     .arg(softDone).arg(softCnt);
    if (auto *l = findChild<QLabel *>("lwd_final")) {
        l->setText(m_finalTxt);
        l->setStyleSheet((endReason == 1 && confirmed == total)
            ? "color:#2a8030; font-size:0.9em; font-weight:bold;"
            : "color:#c05050; font-size:0.9em; font-weight:bold;");
    }
}

// Round105: 推送事件实时行 — 只显示最近一条 (完整明细在开锁器输出区)
void LockerWaitDialog::updateEvent(const QString &txt)
{
    if (auto *l = findChild<QLabel *>("lwd_event"))
        l->setText(txt);
}

// --- 槽实现 ---
// Round_011: 原单标签 ONE_SHOT 表 (kLockerEndReason/kLockerPhase 0~7) 已随 0x08 废除删除
// Round032: GET_PROGRESS 5s 轮询已废 (协议更新后弹窗只接收上报事件) — 终态表在 runLockerUnlockMulti 内

void MainWindow::closeLockerWaitDialog()
{
    if (m_zlrLockerCancelGuard)   { m_zlrLockerCancelGuard->stop();   m_zlrLockerCancelGuard->deleteLater();   m_zlrLockerCancelGuard   = nullptr; }
    if (m_zlrLockerWait)          { m_zlrLockerWait->accept();         m_zlrLockerWait->deleteLater();         m_zlrLockerWait          = nullptr; }
    m_zlrLockerWinMs = 0;   // Round_011: 复位解锁窗
}

// Round_011: 0x08 ONE_SHOT 已废除 (固件不解析帧形状, 一律回 err=2 PARAM, App_LockerOneShot 模块移除)
//   解锁统一走 0x0A UNLOCK_MULTI — 单标 epcCnt=1 (W=2min), 多标 ≤4 张; 均阻塞 + 推送事件实时展示
void MainWindow::onLockerOneShot()
{
    if (!m_zlrLockerUnlockEpc) return;
    QString hex = m_zlrLockerUnlockEpc->text().trimmed();
    if (hex.isEmpty()) {
        appendSystemLog("开锁器: 请输入期望EPC", QColor("#d08020"));
        return;
    }
    // 多 EPC 解析: 空格/逗号/分号 (含全角) 分隔
    QString norm = hex;
    norm.replace(',', ' ').replace(';', ' ').replace("，", " ").replace("；", " ");
    const QStringList tokens = norm.split(' ', Qt::SkipEmptyParts);
    QList<QByteArray> epcs;
    for (const QString &t : tokens) {
        QByteArray e = QByteArray::fromHex(t.toLatin1());
        if (e.isEmpty() || e.size() < 1 || e.size() > 12) {
            appendSystemLog(QString("开锁器: EPC \"%1\" 需为 1~12 字节 hex").arg(t), QColor("#d08020"));
            return;
        }
        epcs.append(e);
    }
    if (epcs.size() > 4) {
        appendSystemLog("开锁器: UNLOCK_MULTI 最多 4 张 EPC", QColor("#d08020"));
        return;
    }
    if (epcs.size() >= 2) {
        // 协议 0x0A 布局为单一 epcLen — 各 EPC 字节数必须一致
        for (const QByteArray &e : epcs) {
            if (e.size() != epcs[0].size()) {
                appendSystemLog("开锁器: 多标签各 EPC 字节数须一致 (协议单 epcLen 域)", QColor("#d08020"));
                return;
            }
        }
    }
    runLockerUnlockMulti(epcs);
}

// Round_011: UNLOCK_MULTI (0x0A) 唯一开锁通道 — 单标 epcCnt=1 / 多标 ≤4 张
//   阻塞等终帧, 期间 0x0B~0x0F 推送事件实时展示 (Round032 起不再轮询 GET_PROGRESS)
//   请求: [cmd, tmo(2), hold(2), softCnt, epcCnt, epcLen, epcCnt*epcLen] (协议 §13.1 V2, 固件 App_Dispatch.c)
//   tmoMs = EPC 单次盘点时限 (0→设备缺省500, 上限10000) — 解锁区输入框, 默认 500ms;
//   holdMs = 解锁总窗: Round035 起隐藏固定下发 0 → 设备公式 W = 120000+(m-1)*30000 (绝对上限240000);
//   softCnt>0 软标窗最长 5min
void MainWindow::runLockerUnlockMulti(const QList<QByteArray> &epcs)
{
    const quint16 tmo  = m_zlrLockerTmo ? static_cast<quint16>(m_zlrLockerTmo->value()) : 500;
    const quint16 hold = 0;   // Round035: hold 固定 0 — 设备按公式取解锁总窗 W
    const quint8 softCnt = m_zlrLockerDemagCnt ? static_cast<quint8>(m_zlrLockerDemagCnt->value()) : 0;
    const quint8 epcLen = static_cast<quint8>(epcs[0].size());
    const quint8 epcCnt = static_cast<quint8>(epcs.size());
    QByteArray arg;
    arg.append(static_cast<char>(tmo & 0xFF));
    arg.append(static_cast<char>((tmo >> 8) & 0xFF));
    arg.append(static_cast<char>(hold & 0xFF));
    arg.append(static_cast<char>((hold >> 8) & 0xFF));
    arg.append(static_cast<char>(softCnt));
    arg.append(static_cast<char>(epcCnt));
    arg.append(static_cast<char>(epcLen));
    for (const QByteArray &e : epcs) arg.append(e);

    // 等待超时 = 解锁总窗 W (hold=0 → 公式 120000+(m-1)*30000) + 软标窗(最多 5min, +10s 余量) + 升降/盘点余量 40s
    const int winMs = 120000 + (epcCnt - 1) * 30000;
    const int timeoutMs = winMs + (softCnt > 0 ? 310000 : 0) + 40000;

    // 弹等待框 (Round032: 不轮询, 进度全靠 0x0B~0x0F 上报事件)
    if (m_zlrLockerWait) closeLockerWaitDialog();
    m_zlrLockerWinMs = winMs;
    m_zlrLockerWait = new LockerWaitDialog(this);
    connect(m_zlrLockerWait, &LockerWaitDialog::cancelRequested, this, &MainWindow::onLockerCancelUnlock);
    connect(m_zlrLockerWait, &QDialog::rejected, this, [this]{
        if (m_zlrLockerWait) closeLockerWaitDialog();
    });
    m_zlrLockerWait->show();

    appendSystemLog(QString("开锁器: 下发 UNLOCK_MULTI tmo=%1 W=%2s softCnt=%3 标签数=%4")
                        .arg(tmo).arg(winMs / 1000.0, 0, 'f', 1).arg(softCnt).arg(epcCnt), QColor("#d08020"));
    if (m_zlrLockerOut)
        m_zlrLockerOut->append(QString("— UNLOCK_MULTI 下发: %1 张 EPC (每张 %2B) 软标 %3, 窗 W=%4s —")
                                   .arg(epcCnt).arg(epcLen).arg(softCnt).arg(winMs / 1000.0, 0, 'f', 1));

    // Round032: 不再启 5s GET_PROGRESS 轮询 — 弹窗进度全靠 0x0B~0x0F 上报事件 (handleLockerPushEvent)

    QByteArray payload;
    QString err;
    quint8 rspErr = 0;
    const bool ok = sendZlrSubCmd(0x23, 0x0A, arg, &payload, &err, timeoutMs,
                                   /*clearRxBuffer=*/true, /*keepWaitingOnNonMatch=*/true, &rspErr,
                                   [this](const QByteArray &f){ handleLockerPushEvent(f); });
    if (!ok) {
        static const char *kUnlockErrName[] = {   // UNLK_ERR_* (协议 §13.4)
            "OK","BUSY","PARAM","UHF_OPEN","UHF_LINK","?","?","HOMING","MOTOR_FAULT","MOTOR_TIMEOUT","AM_LINK","NO_IR"
        };
        const char *eName = (rspErr <= 11) ? kUnlockErrName[rspErr] : "?";
        appendSystemLog(QString("开锁器 UNLOCK_MULTI 失败: err=%1(%2) — %3")
                            .arg(rspErr).arg(eName).arg(err), QColor("#c050a0"));
        if (m_zlrLockerOut) m_zlrLockerOut->append(QString("[失败] err=%1(%2) %3").arg(rspErr).arg(eName).arg(err));
        closeLockerWaitDialog();
        return;
    }

    // 终帧 payload (cmd+err 已剥) = [endReason, bitmap, confirmed, total, rise(2), lower(2), softDone, softCnt, elapsed(2)]
    int endReason = 0, bitmap = 0, confirmed = 0, total = 0, softDone = 0, softCntR = 0, elapsedMs = 0;
    quint16 rise = 0, lower = 0;
    if (payload.size() >= 5) {
        endReason = static_cast<quint8>(payload[0]);
        bitmap    = static_cast<quint8>(payload[1]);
        confirmed = static_cast<quint8>(payload[2]);
        total     = static_cast<quint8>(payload[3]);
    }
    if (payload.size() >= 9) {
        rise  = static_cast<quint8>(payload[4]) | (static_cast<quint8>(payload[5]) << 8);
        lower = static_cast<quint8>(payload[6]) | (static_cast<quint8>(payload[7]) << 8);
    }
    if (payload.size() >= 12) {
        softDone = static_cast<quint8>(payload[8]);
        softCntR = static_cast<quint8>(payload[9]);
        elapsedMs = static_cast<quint8>(payload[10]) | (static_cast<quint8>(payload[11]) << 8);
    }
    static const char *kUnlockEnd[] = {"?","ALL_OK","PARTIAL_TIMEOUT","?","UHF_LOST","?","ABORTED","SOFT_TIMEOUT"};
    const char *eTxt = (endReason >= 0 && endReason <= 7) ? kUnlockEnd[endReason] : "?";
    QString bmTxt = QString::number(bitmap, 2).rightJustified(total > 0 ? total : 1, '0');

    QString result = QString("UNLOCK_MULTI 完成: end=%1(%2)  确认 %3/%4 (位图 0b%5)  rise=%6 lower=%7 软标 %8/%9 历时 %10s")
                       .arg(endReason).arg(eTxt).arg(confirmed).arg(total).arg(bmTxt)
                       .arg(rise).arg(lower).arg(softDone).arg(softCntR).arg(elapsedMs / 1000.0, 0, 'f', 1);
    if (m_zlrLockerOut) m_zlrLockerOut->append(result);
    // 全部确认=成功(绿); PARTIAL/UHF_LOST/ABORTED 按结果着色
    appendSystemLog(QString("开锁器 %1").arg(result),
                    QColor((endReason == 1 && confirmed == total) ? "#50a050" : "#d08020"));

    if (m_zlrLockerWait) {
        if (auto *dlg = qobject_cast<LockerWaitDialog *>(m_zlrLockerWait))
            dlg->updateFinalMulti(endReason, confirmed, total, bitmap, softDone, softCntR);
        QTimer::singleShot(800, this, [this]{ closeLockerWaitDialog(); });
    }
}

// Round105: 0x0B~0x0F 推送事件帧解析 + 实时展示 (协议 §13.3 推送表, 原路回 func=0xF2)
//   帧经 sendZlrSubCmd keepWaiting 分支回调而来 (data 含 0x02 报告 ID 前缀时 parseFrame 自动容忍)
void MainWindow::handleLockerPushEvent(const QByteArray &frame)
{
    FrameData parsed = m_parser->parseFrame(frame);
    if (!parsed.valid || parsed.data.isEmpty()) return;
    const QByteArray &d = parsed.data;
    const quint8 ev = static_cast<quint8>(d[0]);
    QString txt;
    switch (ev) {
    case 0x0F: {   // EVT_START 受理: [0F, 0, phase=1, winMs(3 LE)]
        if (d.size() < 6) return;
        m_zlrLockerWinMs = static_cast<quint8>(d[3]) | (static_cast<quint8>(d[4]) << 8)
                         | (static_cast<quint8>(d[5]) << 16);
        txt = QString("✓ 受理: 解锁窗 W=%1s (自放标起算)").arg(m_zlrLockerWinMs / 1000.0, 0, 'f', 1);
        break;
    }
    case 0x0B: {   // EVT_TAG 确认: [0B, seq, epcLen, epc.., confirmed, total, 判据ms(2), 流程ms(2)]
        if (d.size() < 4) return;
        const int seq = static_cast<quint8>(d[1]);
        const int eLen = static_cast<quint8>(d[2]);
        if (d.size() < 3 + eLen + 6) return;
        const QString epcHex = QString::fromLatin1(d.mid(3, eLen).toHex()).toUpper();
        const int confirmed = static_cast<quint8>(d[3 + eLen]);
        const int total = static_cast<quint8>(d[4 + eLen]);
        const int judgeMs = static_cast<quint8>(d[5 + eLen]) | (static_cast<quint8>(d[6 + eLen]) << 8);
        const int flowMs  = static_cast<quint8>(d[7 + eLen]) | (static_cast<quint8>(d[8 + eLen]) << 8);
        txt = QString("✓ 确认 #%1/%2: EPC=%3 (判据 %4s, 流程 %5s)")
                  .arg(seq).arg(total).arg(epcHex).arg(judgeMs / 1000.0, 0, 'f', 1).arg(flowMs / 1000.0, 0, 'f', 1);
        break;
    }
    case 0x0C: {   // EVT_MISMATCH 失配: [0C, epcLen, epc.., hits] (不终止) — 协议V2: 外来标签稳定确认一张一帧(单向掩码不重报)
        if (d.size() < 2) return;
        const int eLen = static_cast<quint8>(d[1]);
        if (d.size() < 2 + eLen + 1) return;
        const QString epcHex = eLen > 0 ? QString::fromLatin1(d.mid(2, eLen).toHex()).toUpper() : "—";
        const int hits = static_cast<quint8>(d[2 + eLen]);
        txt = QString("✗ 外来EPC %1 (稳定确认, 累计读到 %2 次, 不终止)").arg(epcHex).arg(hits);
        break;
    }
    case 0x0D: {   // EVT_HARD_DONE 硬标段完成: [0D, endReason, bitmap, confirmed, total, elapsed(2)]
        // 协议V2: 磁块保持升起 (回降移至整个流程结束), softCnt>0 随即进入软标段
        if (d.size() < 7) return;
        const int endReason = static_cast<quint8>(d[1]);
        const int bitmap = static_cast<quint8>(d[2]);
        const int confirmed = static_cast<quint8>(d[3]);
        const int total = static_cast<quint8>(d[4]);
        const int elapsedMs = static_cast<quint8>(d[5]) | (static_cast<quint8>(d[6]) << 8);
        static const char *eName[] = {"?","ALL_OK","PARTIAL_TIMEOUT","?","UHF_LOST","?","ABORTED"};
        const int softCnt = m_zlrLockerDemagCnt ? m_zlrLockerDemagCnt->value() : 0;
        txt = QString("◆ 硬标段完成: %1 (位图 0b%2, %3/%4) 历时 %5s, %6")
                  .arg((endReason <= 6) ? eName[endReason] : "?")
                  .arg(QString::number(bitmap, 2).rightJustified(total > 0 ? total : 1, '0'))
                  .arg(confirmed).arg(total).arg(elapsedMs / 1000.0, 0, 'f', 1)
                  .arg(softCnt > 0 ? "磁块保持升起, 进入软标段..." : "磁块保持升起, 等待回降...");
        break;
    }
    case 0x0E: {   // EVT_SOFT 软标解码: [0E, done, softCnt]
        if (d.size() < 3) return;
        txt = QString("✓ 软标消磁 %1/%2").arg(static_cast<quint8>(d[1])).arg(static_cast<quint8>(d[2]));
        break;
    }
    default:
        return;   // 非事件帧 (0x09 迟到响应等) — 静默丢弃
    }
    if (m_zlrLockerOut) m_zlrLockerOut->append(QString("[事件] %1").arg(txt));
    if (auto *dlg = qobject_cast<LockerWaitDialog *>(m_zlrLockerWait))
        dlg->updateEvent(txt);
    // 确认/软标=绿; 失配=警示
    appendSystemLog(QString("开锁器 %1").arg(txt),
                    QColor((ev == 0x0B || ev == 0x0E || ev == 0x0F || ev == 0x0D) ? "#50a050" : "#d08020"));
}

// Round032: onLockerProgress (5s GET_PROGRESS 轮询) 已废 — 协议更新后弹窗只接收上报事件, 函数整体删除

void MainWindow::onLockerCancelUnlock()
{
    // 用户点等待对话框"停止" → 发 CANCEL (0x04)
    QString err;
    appendSystemLog("开锁器: 用户停止, 发 CANCEL...", QColor("#d08020"));
    sendZlrSubCmd(0x23, 0x04, QByteArray(), nullptr, &err, 3000);
    // CANCEL 协议 §13: 0x0A 流程中打断请求 — 立即回 OK, 安全回降后以 endReason=6 ABORTED 结账回 0x0A 终帧
    // 为防终帧迟迟不来, 启 1s 兜底定时强关等待框 (终帧仍会由后台阻塞调用解析落盘)
    if (!m_zlrLockerCancelGuard) {
        m_zlrLockerCancelGuard = new QTimer(this);
        m_zlrLockerCancelGuard->setSingleShot(true);
        connect(m_zlrLockerCancelGuard, &QTimer::timeout, this, [this]{
            // 兜底: 即便 0x0A 终帧没回来, 也强制关闭等待框
            if (m_zlrLockerWait) {
                appendSystemLog("开锁器: 停止后兜底超时 1s 强关等待框", QColor("#d08020"));
                closeLockerWaitDialog();
            }
        });
    }
    m_zlrLockerCancelGuard->start(1000);
}

// ===== RGB (FC=0x24) =====
// RGB SET: [cmd=0x01, mask, reserved=0x00]; mask bit0=G 绿 / bit1=R 红 / bit2=B 蓝
void MainWindow::onRgbSet()
{
    quint8 mask = 0;
    if (m_zlrRgbG && m_zlrRgbG->isChecked()) mask |= 0x01;
    if (m_zlrRgbR && m_zlrRgbR->isChecked()) mask |= 0x02;
    if (m_zlrRgbB && m_zlrRgbB->isChecked()) mask |= 0x04;

    QByteArray arg;
    arg.append(static_cast<char>(mask));
    arg.append(static_cast<char>(0x00)); // reserved
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x24, 0x01, arg, &payload, &err)) {
        appendSystemLog(QString("RGB 下发失败: %1 (mask=0x%2)").arg(err).arg(mask, 2, 16, QChar('0')), QColor("#c050a0"));
        return;
    }
    // 响应回显 mask: payload=[mask]
    quint8 echoMask = payload.isEmpty() ? mask : static_cast<quint8>(payload[0]);
    QString colorStr;
    colorStr += (echoMask & 0x01) ? "绿" : "";
    colorStr += (echoMask & 0x02) ? "红" : "";
    colorStr += (echoMask & 0x04) ? "蓝" : "";
    if (colorStr.isEmpty()) colorStr = "全灭";
    if (m_zlrRgbOutLabel)
        m_zlrRgbOutLabel->setText(QString("上次 mask: 0x%1 (%2)").arg(echoMask, 2, 16, QChar('0')).arg(colorStr));
    appendSystemLog(QString("RGB: 已下发 mask=0x%1 (%2)").arg(echoMask, 2, 16, QChar('0')).arg(colorStr), QColor("#50a050"));
}

void MainWindow::onRgbClear()
{
    // 下发 mask=0x00 (全灭)
    QByteArray arg;
    arg.append(static_cast<char>(0x00));
    arg.append(static_cast<char>(0x00));
    QByteArray payload;
    QString err;
    bool ok = sendZlrSubCmd(0x24, 0x01, arg, &payload, &err);
    // 清除勾选
    if (m_zlrRgbG) m_zlrRgbG->setChecked(false);
    if (m_zlrRgbR) m_zlrRgbR->setChecked(false);
    if (m_zlrRgbB) m_zlrRgbB->setChecked(false);
    if (m_zlrRgbOutLabel) m_zlrRgbOutLabel->setText("上次 mask: 0x00 (全灭)");
    appendSystemLog("RGB: 全灭已下发", ok ? QColor("#50a050") : QColor("#d08020"));
}

// ===== 自检 (FC=0x25) =====
// 错误位名称表 (与协议 §15.1 bit 定义一致)
static const char *kSelfErrName[] = {
    "MOTOR_SPI", "MOTOR_FAULT", "UHF_COMM", "AM_COMM",
    "PARAM_CRC", "TRAVEL_SW",
    nullptr
};
static QString formatErrBits(quint16 bits)
{
    QStringList v;
    for (int i = 0; i < 6; ++i) {
        v << QString("%1=%2(%3)").arg(i).arg((bits >> i) & 1).arg(kSelfErrName[i]);
    }
    return v.join("  ");
}

// Round028: 自检状态表格填充 helper (字段/值/状态: 错红正绿) — inline in each slot
void MainWindow::onSelfTestQuery()
{
    QByteArray payload;
    QString err;
    // sendZlrSubCmd 已剥掉 [cmd, err]: QUERY 响应 data[0]=cmd, data[1]=err,
    // 后续: [errBitsL, errBitsH, motorCommOk, drvFault, uhfLink, amLink, paramCrc, switchErr]
    if (!sendZlrSubCmd(0x25, 0x01, QByteArray(), &payload, &err, 3000)) {
        appendSystemLog(QString("自检 查询失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    if (payload.size() < 8) {
        appendSystemLog(QString("自检 响应长度不足: %1B").arg(payload.size()), QColor("#d08020"));
        return;
    }
    quint16 errBits = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8);
    quint8 motorCommOk = static_cast<quint8>(payload[2]);
    quint8 drvFault = static_cast<quint8>(payload[3]);
    quint8 uhfLink = static_cast<quint8>(payload[4]);
    quint8 amLink = static_cast<quint8>(payload[5]);
    quint8 paramCrc = static_cast<quint8>(payload[6]);
    quint8 switchErr = static_cast<quint8>(payload[7]);

    // 填表格 (字段 / 值 / 状态)
    if (m_zlrSelfTable) {
        m_zlrSelfTable->setRowCount(7);
        struct Row { const char *name; QString val; bool ok; };
        Row rows[7] = {
            {"锁存错误位(16bit)", QString("0x%1  %2").arg(errBits, 4, 16, QChar('0')).arg(formatErrBits(errBits)), errBits == 0},
            {"motorCommOk", QString::number(motorCommOk), motorCommOk != 0},
            {"drvFault", QString("0x%1").arg(drvFault, 2, 16, QChar('0')), drvFault == 0},
            {"uhfLink", QString("%1%2").arg(uhfLink).arg(uhfLink == 0 ? "(正常)" : uhfLink == 1 ? "(超时)" : uhfLink == 2 ? "(CRC错)" : ""), uhfLink == 0},
            {"amLink", QString("%1%2").arg(amLink).arg(amLink == 0 ? "(正常)" : "(掉线)"), amLink == 0},
            {"paramCrc", QString("%1%2").arg(paramCrc).arg(paramCrc == 0 ? "(正常)" : "(曾CRC失败)"), paramCrc == 0},
            {"switchErr", QString("0x%1").arg(switchErr, 2, 16, QChar('0')), switchErr == 0},
        };
        for (int i = 0; i < 7; ++i) {
            auto *nameIt = new QTableWidgetItem(QString::fromUtf8(rows[i].name));
            auto *valIt  = new QTableWidgetItem(rows[i].val);
            auto *stIt   = new QTableWidgetItem(rows[i].ok ? QStringLiteral("✓ 正常") : QStringLiteral("✗ 异常"));
            if (!rows[i].ok) {
                QString red = "background:#fdecea; color:#c05050; font-weight:bold;";
                nameIt->setBackground(QColor("#fdecea"));
                valIt->setBackground(QColor("#fdecea"));
                stIt->setBackground(QColor("#fdecea"));
                stIt->setForeground(QColor("#c05050"));
            } else {
                stIt->setForeground(QColor("#2a8030"));
            }
            m_zlrSelfTable->setItem(i, 0, nameIt);
            m_zlrSelfTable->setItem(i, 1, valIt);
            m_zlrSelfTable->setItem(i, 2, stIt);
        }
    }
    appendSystemLog(QString("自检: errBits=0x%1 motorCommOk=%2 drvFault=0x%3 uhfLink=%4 amLink=%5 paramCrc=%6 switchErr=0x%7")
                        .arg(errBits, 4, 16, QChar('0')).arg(motorCommOk)
                        .arg(drvFault, 2, 16, QChar('0')).arg(uhfLink).arg(amLink)
                        .arg(paramCrc).arg(switchErr, 2, 16, QChar('0')),
                    QColor(errBits == 0 ? "#50a050" : "#d08020"));
}

void MainWindow::onSelfTestRerun()
{
    // RERUN 阻塞约 3s, 用 8000ms 超时保护 (避免默认 3000 卡临界)
    QString err;
    appendSystemLog("自检: 正在重探外设 (~3s)...", QColor("#d08020"));
    QByteArray payload;
    if (!sendZlrSubCmd(0x25, 0x02, QByteArray(), &payload, &err, 8000)) {
        appendSystemLog(QString("自检 重探失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    if (payload.size() < 2) {
        appendSystemLog(QString("自检 重探响应长度不足: %1B").arg(payload.size()), QColor("#d08020"));
        return;
    }
    quint16 errBits = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8);
    // RERUN 仅回 errBits 2 字节; QUERY 才有完整诊断. 这里只更新锁存位行, 其它行保留上次值.
    if (m_zlrSelfTable && m_zlrSelfTable->rowCount() >= 1) {
        auto *valIt = new QTableWidgetItem(QString("0x%1  %2%3")
            .arg(errBits, 4, 16, QChar('0')).arg(formatErrBits(errBits))
            .arg(errBits == 0 ? "  ✓ 全清" : ""));
        auto *stIt  = new QTableWidgetItem(errBits == 0 ? "✓ 正常" : "✗ 异常");
        if (errBits != 0) {
            valIt->setBackground(QColor("#fdecea"));
            stIt->setBackground(QColor("#fdecea"));
            stIt->setForeground(QColor("#c05050"));
        } else stIt->setForeground(QColor("#2a8030"));
        m_zlrSelfTable->setItem(0, 1, valIt);
        m_zlrSelfTable->setItem(0, 2, stIt);
    }
    appendSystemLog(QString("自检 重探完成: errBits=0x%1").arg(errBits, 4, 16, QChar('0')),
                    QColor(errBits == 0 ? "#50a050" : "#d08020"));
}

void MainWindow::onSelfTestClear()
{
    quint16 mask = m_zlrSelfClearMask ? static_cast<quint16>(m_zlrSelfClearMask->value()) : 0;
    QByteArray arg;
    arg.append(static_cast<char>(mask & 0xFF));
    arg.append(static_cast<char>((mask >> 8) & 0xFF));
    QByteArray payload;
    QString err;
    if (!sendZlrSubCmd(0x25, 0x03, arg, &payload, &err, 3000)) {
        appendSystemLog(QString("自检 清错误失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    if (payload.size() < 2) {
        appendSystemLog(QString("自检 清错误响应长度不足: %1B").arg(payload.size()), QColor("#d08020"));
        return;
    }
    quint16 after = static_cast<quint8>(payload[0]) | (static_cast<quint8>(payload[1]) << 8);
    // 仅更新锁存位行
    if (m_zlrSelfTable && m_zlrSelfTable->rowCount() >= 1) {
        auto *valIt = new QTableWidgetItem(QString("0x%1  (CLEAR后)").arg(after, 4, 16, QChar('0')));
        auto *stIt  = new QTableWidgetItem(after == 0 ? "✓ 正常" : "✗ 异常");
        if (after != 0) {
            valIt->setBackground(QColor("#fdecea"));
            stIt->setBackground(QColor("#fdecea"));
            stIt->setForeground(QColor("#c05050"));
        } else stIt->setForeground(QColor("#2a8030"));
        m_zlrSelfTable->setItem(0, 1, valIt);
        m_zlrSelfTable->setItem(0, 2, stIt);
    }
    appendSystemLog(QString("自检 CLEAR mask=0x%1 → 剩余位图 0x%2").arg(mask, 4, 16, QChar('0')).arg(after, 4, 16, QChar('0')),
                    QColor("#50a050"));
}


// RFID 通用: 发送 + 解析响应 + 提取 UID
//   返回 true 表示成功拿到 UID; outUid 填 hex 大写
bool MainWindow::sendRfidAndParse(quint8 fc, quint8 subCmd, const QByteArray &req,
                                   QByteArray *outPayload, QString *outErrNote)
{
    // 通用 RFID 收发: 支持 FC=0x14(14443A) / FC=0x15(15693)
    //   fc=0x14 -> 解析器 parseRfid14443AResponse + rfid14443AResultToString
    //   fc=0x15 -> 解析器 parseRfidResponse       + rfidResultToString
    if (!m_transport || !m_transport->isOpen()) {
        if (outErrNote) *outErrNote = "请先打开通信接口";
        return false;
    }

    const bool is14443A = (fc == 0x14);
    QString subCmdText = is14443A
        ? ProtocolParser::rfid14443ASubCmdToString(subCmd)
        : ProtocolParser::rfidSubCmdToString(subCmd);

    m_rxBuffer.clear();
    m_transport->writeData(req);
    m_transport->waitForBytesWritten(5000);
    appendLog("[SYS][TX]", req, QColor("#c050a0"),
              QString("RFID %1 请求").arg(subCmdText));

    if (!waitForResponse(3000)) {
        if (outErrNote) *outErrNote = QString("超时 3s 无响应 (rxBuf=%1字节)").arg(m_rxBuffer.size());
        return false;
    }

    FrameData frame = m_parser->parseFrame(m_rxBuffer);
    if (!frame.valid) {
        if (outErrNote) *outErrNote = QString("帧解析失败: %1").arg(frame.error);
        return false;
    }
    // 响应 FC = 请求FC ^ 0xFF
    if (frame.fc != (fc ^ 0xFF)) {
        if (outErrNote) *outErrNote = QString("FC=0x%1 (期望 0x%2)")
                                          .arg(hex2(frame.fc)).arg(hex2(fc ^ 0xFF));
        return false;
    }

    RfidResponse rfid = is14443A
        ? m_parser->parseRfid14443AResponse(frame.data)
        : m_parser->parseRfidResponse(frame.data);
    if (!rfid.valid) {
        // 即便 RFID 层解析失败也打印原始响应, 避免误判"无返回"
        appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
                  QString("%1 响应解析失败: %2").arg(subCmdText).arg(rfid.error));
        if (outErrNote) *outErrNote = QString("RFID 响应解析失败: %1").arg(rfid.error);
        return false;
    }
    // 打印响应原始帧 + result 码 (与 sendRfidCtrl 一致, 排查"认证无返回"误判)
    {
        QString rst = is14443A
            ? ProtocolParser::rfid14443AResultToString(rfid.result)
            : ProtocolParser::rfidResultToString(rfid.result);
        appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
                  QString("%1 result=0x%2 (%3)")
                      .arg(subCmdText).arg(hex2(rfid.result)).arg(rst));
    }
    if (rfid.subCmd != subCmd) {
        if (outErrNote) *outErrNote = QString("RFID subCmd=0x%1 (期望 0x%2)")
                                          .arg(hex2(rfid.subCmd)).arg(hex2(subCmd));
        return false;
    }
    if (rfid.result != 0x00) {
        QString rst = is14443A
            ? ProtocolParser::rfid14443AResultToString(rfid.result)
            : ProtocolParser::rfidResultToString(rfid.result);
        if (outErrNote) *outErrNote = QString("RFID 失败: %1").arg(rst);
        return false;
    }
    if (outPayload) *outPayload = rfid.payload.mid(1); // 去掉 result 字节
    return true;
}

// 提取 UID 字符串 (HEX 大写, 无空格). payload = result 之后的数据, UID 8B (15693) / 4-10B (14443A)
static QString uidToString(const QByteArray &payload, int uidLen)
{
    QByteArray uidBytes = payload.left(uidLen);
    return QString::fromLatin1(uidBytes.toHex(' ')).toUpper().remove(' ');
}

// 14443A 卡类型识别 (Sw01 §8.2 GetUid 响应的 uidType 2B BE 卡型码)
static QString cardTypeFromUidType(quint16 uidType)
{
    switch (uidType) {
    case 0x0044: return "MF0 Ultralight";
    case 0x0004: return "MF1 S50";
    case 0x0002: return "MF1 S70";
    case 0x0344: return "MF3 DESFire";
    default:     return QString("Unknown(0x%1)").arg(uidType, 4, 16, QChar('0')).toUpper();
    }
}

// ============================ App Protocol FC 顶层命令 ============================

// 通用: 发请求 + 等响应 + 解析 1B result, 返回 true 表示成功
bool MainWindow::sendFcAndCheckResult(quint8 fc, const QString &note, int timeoutMs)
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog(QString("%1: 请先连接").arg(note));
        return false;
    }
    QByteArray req = m_parser->makeFrame(m_deviceAddr, fc);
    m_rxBuffer.clear();
    m_transport->writeData(req);
    m_transport->waitForBytesWritten(5000);
    appendLog("[SYS][TX]", req, QColor("#c050a0"),
              QString("%1 请求").arg(note));

    if (!waitForResponse(timeoutMs > 0 ? timeoutMs : 3000)) {
        appendSystemLog(QString("[%1] 超时 %2ms 无响应").arg(note).arg(timeoutMs), QColor("#d08020"));
        return false;
    }
    FrameData frame = m_parser->parseFrame(m_rxBuffer);
    if (!frame.valid) {
        appendSystemLog(QString("[%1] 帧解析失败: %2").arg(note).arg(frame.error), QColor("#c050a0"));
        return false;
    }
    if (frame.fc != (fc ^ 0xFF)) {
        appendSystemLog(QString("[%1] FC=0x%2 (期望 0x%3)")
                            .arg(note).arg(hex2(frame.fc)).arg(hex2(fc ^ 0xFF)),
                        QColor("#c050a0"));
        return false;
    }
    if (frame.data.isEmpty()) {
        appendSystemLog(QString("[%1] 响应 data 为空").arg(note), QColor("#c050a0"));
        return false;
    }
    quint8 result = static_cast<quint8>(frame.data[0]);
    appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
              QString("OK FC=0x%1 result=0x%2").arg(hex2(fc)).arg(hex2(result)));
    if (result != 0x00) {
        appendSystemLog(QString("[%1] 失败 result=0x%2").arg(note).arg(hex2(result)),
                        QColor("#c050a0"));
        return false;
    }
    return true;
}

void MainWindow::onFuncEnterBoot()
{
    // FC=0x02: 进 Boot 升级模式. 设备回 OK 后写 status=UPG, BOOT_DEFER_MS 后软复位 → Bootloader 接管
    if (sendFcAndCheckResult(0x02, "FC=0x02 进Boot", 3000)) {
        appendSystemLog("[进Boot] 设备将软复位, 进入 Boot 升级模式", QColor("#50a050"));
        m_funcStatusLabel->setText("● 已请求进 Boot, 等待设备复位...");
        m_funcStatusLabel->setStyleSheet("color: #d08020; font-size: 0.82em;");
    }
}

void MainWindow::onFuncReset()
{
    // FC=0x08: 软件复位. 设备回 1B OK, 20ms 后 NVIC_SystemReset
    if (sendFcAndCheckResult(0x08, "FC=0x08 复位", 3000)) {
        appendSystemLog("[复位] 设备将在 ~20ms 后软复位", QColor("#50a050"));
        m_funcStatusLabel->setText("● 已请求复位, 设备即将重启...");
        m_funcStatusLabel->setStyleSheet("color: #d08020; font-size: 0.82em;");
        // 200ms 后断开 UI 连接, 设备复位后需重新打开
        QTimer::singleShot(500, this, [this]() {
            if (m_transport && m_transport->isOpen()) {
                m_transport->close();
                m_transport = nullptr;
            }
            updateConnectionState(false);
            m_openBtn->setText(tr("Connect"));
            m_openBtn->setProperty("connected", false);
            m_openBtn->style()->unpolish(m_openBtn);
            m_openBtn->style()->polish(m_openBtn);
            // 清空版本信息 (设备已复位, 原值可能已失效)
            m_verAddrEdit->clear();
            m_verSwEdit->clear();
            m_verHwEdit->clear();
            m_funcUnlocked = false;
            enableFuncButtons(false);
        });
    }
}

// ============================ 射频控制 (FC=0x16) ============================

// FC=0x16 同步收发: 发请求 → 等响应 → 解析 [subCmd][0][1][result], result==0 返回 true
bool MainWindow::sendRfidCtrl(quint8 subCmd, const QByteArray &req, QString *outErrNote)
{
    if (!m_transport || !m_transport->isOpen()) {
        if (outErrNote) *outErrNote = "请先连接";
        return false;
    }
    m_rxBuffer.clear();
    m_transport->writeData(req);
    m_transport->waitForBytesWritten(5000);
    appendLog("[SYS][TX]", req, QColor("#c050a0"),
              QString("RF %1").arg(ProtocolParser::rfidCtrlSubCmdToString(subCmd)));

    if (!waitForResponse(3000)) {
        if (outErrNote) *outErrNote = QString("超时 3s 无响应 (rxBuf=%1字节)").arg(m_rxBuffer.size());
        return false;
    }
    FrameData frame = m_parser->parseFrame(m_rxBuffer);
    if (!frame.valid) {
        if (outErrNote) *outErrNote = QString("帧解析失败: %1").arg(frame.error);
        return false;
    }
    // 响应 FC = 0x16 ^ 0xFF = 0xE9
    if (frame.fc != (0x16 ^ 0xFF)) {
        if (outErrNote) *outErrNote = QString("FC=0x%1 (期望 0x%2)")
                                          .arg(hex2(frame.fc)).arg(hex2(0x16 ^ 0xFF));
        return false;
    }
    quint8 result = m_parser->parseRfidCtrlResponse(frame.data, subCmd);
    appendLog("[SYS][RX]", m_rxBuffer, QColor("#30b078"),
              QString("OK %1 result=0x%2")
                  .arg(ProtocolParser::rfidCtrlSubCmdToString(subCmd)).arg(hex2(result)));
    if (result != 0x00) {
        if (outErrNote) *outErrNote = QString("失败: %1")
                                          .arg(ProtocolParser::rfidResultToString(result));
        return false;
    }
    return true;
}

// 按协议 INIT+DELAY+OPEN 时序开启射频 (需求 7.3 典型时序 ①~④)
bool MainWindow::rfOpenWithProto(quint8 proto)
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("开启射频: 请先连接", QColor("#c050a0"));
        return false;
    }
    QString protoName = (proto == RFID_PROTO_14443A) ? "14443A" : "15693";
    appendSystemLog(QString("[开启射频] 配置 %1 协议 + 开启射频 (CLOSE→INIT→DELAY6→OPEN)").arg(protoName));

    QString err;
    // ① CLOSE
    if (!sendRfidCtrl(RFID_CTRL_CLOSE, m_parser->makeRfidCtrlClose(m_deviceAddr), &err)) {
        appendSystemLog(QString("[开启射频] CLOSE 失败: %1").arg(err), QColor("#c050a0"));
        return false;
    }
    // ② INIT proto
    if (!sendRfidCtrl(RFID_CTRL_INIT, m_parser->makeRfidCtrlInit(m_deviceAddr, proto), &err)) {
        appendSystemLog(QString("[开启射频] INIT 失败: %1").arg(err), QColor("#c050a0"));
        return false;
    }
    // ③ DELAY 6ms 稳定
    if (!sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 6), &err)) {
        appendSystemLog(QString("[开启射频] DELAY 失败: %1").arg(err), QColor("#c050a0"));
        return false;
    }
    // ④ OPEN
    if (!sendRfidCtrl(RFID_CTRL_OPEN, m_parser->makeRfidCtrlOpen(m_deviceAddr), &err)) {
        appendSystemLog(QString("[开启射频] OPEN 失败: %1").arg(err), QColor("#c050a0"));
        return false;
    }
    appendSystemLog(QString("[开启射频] %1 已开启, 可执行标签操作").arg(protoName), QColor("#50a050"));
    m_funcStatusLabel->setText(QString("● 射频已开启 (%1)").arg(protoName));
    m_funcStatusLabel->setStyleSheet("color: #50a050; font-size: 0.82em;");
    // 射频开启后启用下方卡操作按钮
    enableRfFuncButtons(true);
    return true;
}

void MainWindow::onRfOpen()
{
    // 按当前 Tab 协议配置并开启射频
    int idx = m_sccdTabs->currentIndex();
    quint8 proto = (idx == 1) ? RFID_PROTO_14443A : RFID_PROTO_15693;  // 默认/15693 Tab → 15693
    rfOpenWithProto(proto);
}

void MainWindow::onRfClose()
{
    QString err;
    if (!sendRfidCtrl(RFID_CTRL_CLOSE, m_parser->makeRfidCtrlClose(m_deviceAddr), &err)) {
        appendSystemLog(QString("[关闭射频] 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    appendSystemLog("[关闭射频] 已关闭", QColor("#50a050"));
    m_funcStatusLabel->setText("● 射频已关闭");
    m_funcStatusLabel->setStyleSheet("color: #b0a0c8; font-size: 0.82em;");
    // 射频关闭后禁用下方卡操作按钮
    enableRfFuncButtons(false);
}

void MainWindow::onRfReset()
{
    // 复位射频: CLOSE → INIT(当前协议) → DELAY → OPEN
    int idx = m_sccdTabs->currentIndex();
    quint8 proto = (idx == 1) ? RFID_PROTO_14443A : RFID_PROTO_15693;
    appendSystemLog(QString("[复位射频] 重启 %1 射频").arg(proto == RFID_PROTO_14443A ? "14443A" : "15693"));
    QString err;
    if (!sendRfidCtrl(RFID_CTRL_CLOSE, m_parser->makeRfidCtrlClose(m_deviceAddr), &err) ||
        !sendRfidCtrl(RFID_CTRL_INIT, m_parser->makeRfidCtrlInit(m_deviceAddr, proto), &err) ||
        !sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 6), &err) ||
        !sendRfidCtrl(RFID_CTRL_OPEN, m_parser->makeRfidCtrlOpen(m_deviceAddr), &err)) {
        appendSystemLog(QString("[复位射频] 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    appendSystemLog("[复位射频] 完成", QColor("#50a050"));
    enableRfFuncButtons(true);
}

// 启用/禁用 Sccd 内卡操作按钮 (射频开启后启用, 关闭/未开时禁用)
void MainWindow::enableRfFuncButtons(bool enable)
{
    // 15693
    if (m_15693InventoryBtn) m_15693InventoryBtn->setEnabled(enable);
    if (m_15693ReadBtn)      m_15693ReadBtn->setEnabled(enable);
    if (m_15693WriteBtn)     m_15693WriteBtn->setEnabled(enable);
    if (m_15693LockBtn)      m_15693LockBtn->setEnabled(enable);
    if (m_15693StayQuietBtn) m_15693StayQuietBtn->setEnabled(enable);
    if (m_15693DsfidBtn)     m_15693DsfidBtn->setEnabled(enable);
    if (m_15693EasBtn)       m_15693EasBtn->setEnabled(enable);
    if (m_15693WriteAfiBtn)  m_15693WriteAfiBtn->setEnabled(enable);
    if (m_15693LockAfiBtn)   m_15693LockAfiBtn->setEnabled(enable);
    // 14443A
    if (m_14443AInventoryBtn) m_14443AInventoryBtn->setEnabled(enable);
    if (m_14443AReadBtn)     m_14443AReadBtn->setEnabled(enable);
    if (m_14443AWriteBtn)    m_14443AWriteBtn->setEnabled(enable);
    if (m_14443AHaltBtn)    m_14443AHaltBtn->setEnabled(enable);
    if (m_14443ARatsBtn)    m_14443ARatsBtn->setEnabled(enable);
    if (m_14443ATransApduBtn) m_14443ATransApduBtn->setEnabled(enable);
    if (m_14443ATopazReadBtn)  m_14443ATopazReadBtn->setEnabled(enable);
    if (m_14443ATopazWriteBtn) m_14443ATopazWriteBtn->setEnabled(enable);
    if (m_14443AWalletInitBtn) m_14443AWalletInitBtn->setEnabled(enable);
    if (m_14443AWalletGetBtn)  m_14443AWalletGetBtn->setEnabled(enable);
    if (m_14443AWalletIncBtn)  m_14443AWalletIncBtn->setEnabled(enable);
    if (m_14443AWalletDecBtn)  m_14443AWalletDecBtn->setEnabled(enable);
}

void MainWindow::update14443AButtonsByCardType()
{
    // 按卡类型启用/禁用 M1 专属操作:
    //   M1 (Classic): KeyA/B 按钮 + Key 输入框 + 值块(钱包) 可用; 读/写走 0x30/0x31
    //   M0 (Ultralight): 无 M1 认证, 禁用 KeyA/B + 钱包; 读/写改走 0x21/0x20 (无认证), 输入输出区保留
    //   Halt/RATS/TransAPDU/Topaz: 通用, 不按 M1/M0 禁用
    const bool m1 = m_14443ACardIsM1;
    // KeyA / KeyB 选中按钮 + 输入框: 仅 M1 需要
    if (m_14443AKeyABtn)  m_14443AKeyABtn->setEnabled(m1);
    if (m_14443AKeyBBtn)  m_14443AKeyBBtn->setEnabled(m1);
    if (m_14443AKeyAEdit) m_14443AKeyAEdit->setEnabled(m1);
    if (m_14443AKeyBEdit) m_14443AKeyBEdit->setEnabled(m1);
    // 钱包组 (M1 值块): 仅 M1
    if (m_14443AWalletInitBtn) m_14443AWalletInitBtn->setEnabled(m1);
    if (m_14443AWalletGetBtn)  m_14443AWalletGetBtn->setEnabled(m1);
    if (m_14443AWalletIncBtn)  m_14443AWalletIncBtn->setEnabled(m1);
    if (m_14443AWalletDecBtn)  m_14443AWalletDecBtn->setEnabled(m1);
    // Read/Write/Halt/RATS/TransAPDU/TopazRead/TopazWrite: M1/M0 均可用, 不在此禁用
}

// 14443A KeyA / KeyB 互斥选中
void MainWindow::on14443AAuthKeyA()
{
    m_14443AKeyABtn->setChecked(true);
    m_14443AKeyBBtn->setChecked(false);
    appendSystemLog("[14443A] 选中 KeyA 认证", QColor("#8870a8"));
}

void MainWindow::on14443AAuthKeyB()
{
    m_14443AKeyABtn->setChecked(false);
    m_14443AKeyBBtn->setChecked(true);
    appendSystemLog("[14443A] 选中 KeyB 认证", QColor("#8870a8"));
}

// ----- 15693 -----

void MainWindow::on15693Inventory()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 Inventory: 请先连接"); return; }
    if (!rfEnsureOpen15693()) { appendSystemLog("[15693 Inventory] 射频开启失败", QColor("#c050a0")); return; }
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidInventory(m_deviceAddr);
    QByteArray payload;
    if (!sendRfidAndParse(0x15, RFID_SUB_INVENTORY, req, &payload, &err)) {
        appendSystemLog(QString("[15693 Inventory] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // §7.1: 响应 [result][uidCount][UID 8B...] → payload 去掉 result = [uidCount][UID...]
    if (payload.isEmpty()) {
        appendSystemLog("[15693 Inventory] 响应空", QColor("#c050a0")); return;
    }
    quint8 uidCount = static_cast<quint8>(payload[0]);
    if (uidCount < 1 || payload.size() < 1 + 8) {
        appendSystemLog(QString("[15693 Inventory] 响应异常: uidCount=%1 payload=%2B")
                            .arg(uidCount).arg(payload.size()), QColor("#c050a0")); return;
    }
    QString uid = uidToString(payload.mid(1), 8);
    m_15693UidEdit->setText(uid);
    appendSystemLog(QString("[15693 Inventory] 成功, uidCount=%1 UID=%2").arg(uidCount).arg(uid),
                    QColor("#50a050"));
}

// 从 UID 编辑框取 8B uid, 空则提示; 返回 false 表示格式错
bool MainWindow::uid15693Bytes(QByteArray &uidBytes)
{
    QString uidText = m_15693UidEdit->text().trimmed();
    if (uidText.isEmpty() || uidText == "Uid") return false;
    uidBytes = QByteArray::fromHex(uidText.toUtf8());
    return uidBytes.size() == 8;
}

// 15693 ReadBlock (Sw01 §7 0x01): [0x01][UID 8B][addr][count] → 响应 [result][data...] (可分片)
//   读 count 块, 每块 4B. count*4 > 53B 时设备分片, 当前只取片0(默认 count ≤ 13)
void MainWindow::on15693ReadBlock()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 Read: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 Read] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    bool ok = false;
    quint8 addr = static_cast<quint8>(m_15693AddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) { appendSystemLog("[15693 Read] 块地址格式错误", QColor("#c050a0")); return; }
    quint8 count = static_cast<quint8>(m_15693CountEdit->text().trimmed().toInt(&ok, 16));
    if (!ok || count == 0) { appendSystemLog("[15693 Read] 块数格式错误(需 1..32)", QColor("#c050a0")); return; }
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidReadBlock(m_deviceAddr, uidBytes, addr, count);
    QByteArray payload;
    if (!sendRfidAndParse(0x15, RFID_SUB_READ_BLOCK, req, &payload, &err)) {
        appendSystemLog(QString("[15693 Read] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // payload 去掉 result = data (count*4 字节, 单包)
    QString hex = QString::fromLatin1(payload.toHex(' ').toUpper());
    m_15693OutputArea->setPlainText(hex);
    appendSystemLog(QString("[15693 Read] 成功, addr=0x%1 count=%2 data(%3B)=%4")
                        .arg(hex2(addr)).arg(count).arg(payload.size()).arg(hex), QColor("#50a050"));
}

// 15693 WriteBlock (§7 0x02): [0x02][UID 8B][addr][count][data] (data = count*4 B)
//   data 取输入区 (count*4 个 hex 字符, 即 8*count hex 字符)
void MainWindow::on15693WriteBlock()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 Write: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 Write] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    bool ok = false;
    quint8 addr = static_cast<quint8>(m_15693AddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) { appendSystemLog("[15693 Write] 块地址格式错误", QColor("#c050a0")); return; }
    quint8 count = static_cast<quint8>(m_15693CountEdit->text().trimmed().toInt(&ok, 16));
    if (!ok || count == 0 || count > 8) {
        appendSystemLog(QString("[15693 Write] 块数需 1..8 (SDK 缓冲限制), 当前 %1")
                            .arg(count), QColor("#c050a0")); return;
    }
    QString hexInput = m_15693InputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    int needHex = count * 8;  // 每块 4B = 8 hex 字符
    if (hexInput.length() != needHex) {
        appendSystemLog(QString("[15693 Write] 输入区需 %1B (%2 hex 字符), 当前 %3")
                            .arg(count*4).arg(needHex).arg(hexInput.length()), QColor("#c050a0")); return;
    }
    QByteArray blkData = QByteArray::fromHex(hexInput.toUtf8());
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidWriteBlock(m_deviceAddr, uidBytes, addr, count, blkData);
    if (!sendRfidAndParse(0x15, RFID_SUB_WRITE_BLOCK, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 Write] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog(QString("[15693 Write] 成功, addr=0x%1 count=%2").arg(hex2(addr)).arg(count),
                    QColor("#50a050"));
}

// 15693 LockBlock (§7 0x05): [0x05][UID 8B][addr] → 响应仅 result
void MainWindow::on15693LockBlock()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 LockBlock: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 LockBlock] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    bool ok = false;
    quint8 addr = static_cast<quint8>(m_15693AddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) { appendSystemLog("[15693 LockBlock] 块地址格式错误", QColor("#c050a0")); return; }
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidLockBlock(m_deviceAddr, uidBytes, addr);
    if (!sendRfidAndParse(0x15, RFID_SUB_LOCK_BLOCK, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 LockBlock] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog(QString("[15693 LockBlock] 成功, addr=0x%1").arg(hex2(addr)), QColor("#50a050"));
}

// 15693 StayQuiet (§7 0x06): [0x06][UID 8B] → 响应仅 result. 卡进入静默, 后续操作需重 Inventory
void MainWindow::on15693StayQuiet()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 StayQuiet: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 StayQuiet] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidStayQuiet(m_deviceAddr, uidBytes);
    if (!sendRfidAndParse(0x15, RFID_SUB_STAY_QUIET, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 StayQuiet] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog("[15693 StayQuiet] 成功 (卡进入静默, 后续操作需重 Inventory)", QColor("#50a050"));
}

// 15693 DSFID (§7 0x03): [0x03][UID 8B][mode][value?]
//   mode: 0=读 / 1=写 (带 value) / 2=锁. 弹 QInputDialog 输入 mode (0/1/2), 写时再输 value (0..255)
void MainWindow::on15693Dsfid()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 DSFID: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 DSFID] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    bool ok = false;
    int mode = QInputDialog::getInt(this, tr("DSFID"), tr("Mode (0=读 / 1=写 / 2=锁):"), 0, 0, 2, 1, &ok);
    if (!ok) return;
    quint8 value = 0;
    if (mode == 1) {
        value = static_cast<quint8>(QInputDialog::getInt(this, tr("DSFID 写"),
                                  tr("Value (0..255):"), 0, 0, 255, 1, &ok));
        if (!ok) return;
    }
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidDsfid(m_deviceAddr, uidBytes, static_cast<quint8>(mode), value);
    QByteArray payload;
    if (!sendRfidAndParse(0x15, RFID_SUB_DSFID, req, &payload, &err)) {
        appendSystemLog(QString("[15693 DSFID] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // §7.1: 读 → [result][dsfid]; 写/锁 → 仅 [result]
    if (mode == 0 && !payload.isEmpty()) {
        quint8 dsfid = static_cast<quint8>(payload[0]);
        m_15693OutputArea->setPlainText(QString("DSFID = 0x%1").arg(hex2(dsfid)));
        appendSystemLog(QString("[15693 DSFID] 读成功, DSFID=0x%1").arg(hex2(dsfid)), QColor("#50a050"));
    } else {
        const char *op = (mode == 1) ? "写" : "锁";
        appendSystemLog(QString("[15693 DSFID] %1成功").arg(op), QColor("#50a050"));
    }
}

// 15693 EAS (§7 0x04): [0x04][UID 8B][op]   op: 0=置位 / 1=复位 / 2=锁定
void MainWindow::on15693Eas()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 EAS: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 EAS] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    bool ok = false;
    int op = QInputDialog::getInt(this, tr("EAS"), tr("Op (0=置位 / 1=复位 / 2=锁定):"), 0, 0, 2, 1, &ok);
    if (!ok) return;
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidEas(m_deviceAddr, uidBytes, static_cast<quint8>(op));
    if (!sendRfidAndParse(0x15, RFID_SUB_EAS, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 EAS] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    const char *opName = (op == 0) ? "置位" : (op == 1) ? "复位" : "锁定";
    appendSystemLog(QString("[15693 EAS] %1成功").arg(opName), QColor("#50a050"));
}

// 15693 WriteAFI (§7 0x07): [0x07][UID 8B][afi]   afi 取输入区 1B (2 hex 字符)
void MainWindow::on15693WriteAfi()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 WriteAFI: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 WriteAFI] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    QString hexInput = m_15693InputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    if (hexInput.length() != 2) {
        appendSystemLog(QString("[15693 WriteAFI] 输入区需 1B AFI (2 hex 字符), 当前 %1")
                            .arg(hexInput.length()), QColor("#c050a0")); return;
    }
    quint8 afi = static_cast<quint8>(hexInput.toUInt(nullptr, 16));
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidWriteAfi(m_deviceAddr, uidBytes, afi);
    if (!sendRfidAndParse(0x15, RFID_SUB_WRITE_AFI, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 WriteAFI] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog(QString("[15693 WriteAFI] 成功, AFI=0x%1").arg(hex2(afi)), QColor("#50a050"));
}

// 15693 LockAFI (§7 0x08): [0x08][UID 8B] → 响应仅 result
void MainWindow::on15693LockAfi()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("15693 LockAFI: 请先连接"); return; }
    QByteArray uidBytes;
    if (!uid15693Bytes( uidBytes)) {
        appendSystemLog("[15693 LockAFI] 请先 Inventory 获取 UID (需 8B)", QColor("#d08020")); return;
    }
    if (!rfEnsureOpen15693()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfidLockAfi(m_deviceAddr, uidBytes);
    if (!sendRfidAndParse(0x15, RFID_SUB_LOCK_AFI, req, nullptr, &err)) {
        appendSystemLog(QString("[15693 LockAFI] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog("[15693 LockAFI] 成功", QColor("#50a050"));
}

// ----- 14443A -----

// 14443A 内部: OPEN + DELAY 5ms 前置 (需求 7.4/7.5 时序, 射频已开启时复用)
//   返回 true 表示前置成功 (射频已开则跳过 OPEN)
bool MainWindow::rfEnsureOpen14443A()
{
    // 已开启射频 (状态标签含 "已开启") 则跳过; 否则按 proto=14443A 开启
    if (m_funcStatusLabel->text().contains("已开启")) return true;
    return rfOpenWithProto(RFID_PROTO_14443A);
}

bool MainWindow::rfEnsureOpen15693()
{
    if (m_funcStatusLabel->text().contains("已开启")) return true;
    return rfOpenWithProto(RFID_PROTO_15693);
}

void MainWindow::on14443AInventory()
{
    // 14443A 盘点 (FC=0x14, subCmd=0x00 GetUid)
    //   流程 (需求 7.4): OPEN → DELAY 5ms → GetUid → 返回解析
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("14443A 盘点: 请先连接");
        return;
    }
    // 前置 OPEN + DELAY 5ms (射频未开则先配置+开启)
    if (!rfEnsureOpen14443A()) {
        appendSystemLog("[14443A 盘点] 射频开启失败", QColor("#c050a0"));
        return;
    }
    QString err;
    if (!sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err)) {
        appendSystemLog(QString("[14443A 盘点] DELAY 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    QByteArray req = m_parser->makeRfid14443AGetUid(m_deviceAddr);
    QByteArray payload;
    if (!sendRfidAndParse(0x14, RFID_14443A_GETUID, req, &payload, &err)) {
        appendSystemLog(QString("[14443A 盘点] 失败: %1").arg(err), QColor("#c050a0"));
        m_14443ACardTypeEdit->clear();
        m_14443ACardIsM1 = false;
        update14443AButtonsByCardType();
        return;
    }
    // 响应 (去掉 result 后, §8.2 GetUid 格式 Round 064): [count][uidType 2B BE][uidLen][uid..][sak]
    //   uidType = 卡型码: 0x0044=MF0 Ultralight / 0x0004=MF1 S50 / 0x0002=MF1 S70 / 0x0344=MF3 DESFire
    //   count 上限 1
    if (payload.size() < 4) {
        appendSystemLog("[14443A 盘点] 响应过短, 无法解析 UID", QColor("#c050a0"));
        return;
    }
    quint8 count = static_cast<quint8>(payload[0]);
    quint16 uidType = (static_cast<quint8>(payload[1]) << 8) | static_cast<quint8>(payload[2]);  // 2B BE
    quint8 uidLen = static_cast<quint8>(payload[3]);
    if (uidLen == 0 || payload.size() < 4 + uidLen) {
        appendSystemLog(QString("[14443A 盘点] UID 长度异常: len=%1 payload=%2B")
                            .arg(uidLen).arg(payload.size()), QColor("#c050a0"));
        return;
    }
    QString uid = uidToString(payload.mid(4), uidLen);
    m_14443AUidEdit->setText(uid);
    // 按 uidType 识别卡类型填入卡类型框
    QString cardType = cardTypeFromUidType(uidType);
    m_14443ACardTypeEdit->setText(cardType);
    // 记录当前卡是否 M1 (Classic), M0(Ultralight) 则禁用 M1 专属操作 (KeyA/B, 读/写块, 锁块)
    m_14443ACardIsM1 = (uidType == 0x0004 || uidType == 0x0002);  // MF1 S50 / S70
    update14443AButtonsByCardType();
    appendSystemLog(QString("[14443A 盘点] 成功, count=%1 uidType=0x%2 %3 UID=%4")
                        .arg(count)
                        .arg(uidType, 4, 16, QChar('0')).toUpper()
                        .arg(cardType).arg(uid), QColor("#50a050"));
}

// 14443A 读块 (Sw01 §8): 按卡类型分流
//   M1 (Classic): OPEN → DELAY 5ms → AuthReadBlockM1(0x30) 一键 [blockAddr][mode][key 6B][uid 4B]
//   M0 (Ultralight): OPEN → DELAY 5ms → ReadBlockM0(0x21) 无认证 [blockAddr] 读 16B(4 页连读)
//   结果(16B hex)显示在输出区
void MainWindow::on14443AReadBlock()
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("14443A 读块: 请先连接");
        return;
    }
    // 块地址 (2 hex 字符, 默认 0x00)
    bool ok = false;
    quint8 blockAddr = static_cast<quint8>(m_14443AAddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) {
        appendSystemLog("[14443A 读块] 块地址格式错误 (需 2 hex 字符)", QColor("#c050a0"));
        return;
    }

    // 前置 OPEN + DELAY 5ms
    if (!rfEnsureOpen14443A()) {
        appendSystemLog("[14443A 读块] 射频开启失败", QColor("#c050a0"));
        return;
    }
    QString err;
    if (!sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err)) {
        appendSystemLog(QString("[14443A 读块] DELAY 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }

    QByteArray readReq;
    quint8 subCmd;
    if (m_14443ACardIsM1) {
        // M1: 一键 AuthReadBlockM1(0x30)
        bool useKeyB = m_14443AKeyBBtn->isChecked();
        quint8 authMode = useKeyB ? RFID_AUTH_KEYB : RFID_AUTH_KEYA;   // 0x60=KeyA, 0x61=KeyB
        QString keyText = useKeyB ? m_14443AKeyBEdit->text().trimmed()
                                  : m_14443AKeyAEdit->text().trimmed();
        QString keyHex = keyText; keyHex.remove(' ');
        if (keyHex.length() != 12) {
            appendSystemLog(QString("[14443A 读块] Key 长度需 12 hex 字符 (6B), 当前 %1")
                                .arg(keyHex.length()), QColor("#c050a0"));
            return;
        }
        QByteArray keyBytes = QByteArray::fromHex(keyHex.toUtf8());
        if (keyBytes.size() != 6) {
            appendSystemLog("[14443A 读块] Key hex 解析失败", QColor("#c050a0"));
            return;
        }
        // UID: 一键模式, 空则用 00 00 00 00 (设备内部 GetUid); 非空用下发 uid (直接 AuthM1)
        QString uidText = m_14443AUidEdit->text().trimmed();
        QByteArray uidBytes;
        if (!uidText.isEmpty() && uidText != "Uid") {
            uidBytes = QByteArray::fromHex(uidText.toUtf8());
            if (uidBytes.size() < 4) uidBytes.clear();
        }
        if (uidBytes.isEmpty()) uidBytes = QByteArray(4, 0);
        readReq = m_parser->makeRfid14443AAuthReadBlockM1(m_deviceAddr, blockAddr,
                                                          authMode, keyBytes, uidBytes.left(4));
        subCmd = RFID_14443A_AUTH_READ_BLOCK;
    } else {
        // M0: 无认证 ReadBlockM0(0x21)
        readReq = m_parser->makeRfid14443AReadBlockM0(m_deviceAddr, blockAddr);
        subCmd = RFID_14443A_READ_BLOCK_M0;
    }

    QByteArray readPayload;
    if (!sendRfidAndParse(0x14, subCmd, readReq, &readPayload, &err)) {
        appendSystemLog(QString("[14443A 读块] 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    // readPayload (去掉 result) = data 16B
    QString hexData = QString::fromLatin1(readPayload.toHex(' ').toUpper());
    m_14443AOutputArea->setPlainText(hexData);
    appendSystemLog(QString("[14443A 读块] 成功, addr=0x%1, %2, data(16B)=%3")
                        .arg(hex2(blockAddr))
                        .arg(m_14443ACardIsM1 ? "M1" : "M0")
                        .arg(hexData), QColor("#50a050"));
}

// 14443A 写块 (Sw01 §8 一键模式): OPEN → DELAY 5ms → AuthWriteBlockM1(0x31)
//   0x31 帧 [blockAddr][mode][key 6B][uid 4B][data 16B], 设备内部 GetUid+AuthM1+WriteBlock(0xA0, 16B)
// 14443A 写块 (Sw01 §8): 按卡类型分流
//   M1 (Classic): OPEN → DELAY 5ms → AuthWriteBlockM1(0x31) [blockAddr][mode][key 6B][uid 4B][data 16B]
//   M0 (Ultralight): OPEN → DELAY 5ms → WriteBlockM0(0x20) 无认证 [blockAddr][data 4B]
void MainWindow::on14443AWriteBlock()
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog("14443A 写块: 请先连接");
        return;
    }
    // 从输入区取数据 (hex 无空格): M1 需 16B(32 hex), M0 需 4B(8 hex)
    QString hexInput = m_14443AInputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    const int needHex = m_14443ACardIsM1 ? 32 : 8;
    if (hexInput.length() != needHex) {
        appendSystemLog(QString("[14443A 写块] 输入区需 %1B (%2 hex 字符), 当前 %3")
                            .arg(m_14443ACardIsM1 ? 16 : 4).arg(needHex).arg(hexInput.length()),
                        QColor("#c050a0"));
        return;
    }
    bool ok = false;
    quint8 blockAddr = static_cast<quint8>(m_14443AAddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) {
        appendSystemLog("[14443A 写块] 块地址格式错误", QColor("#c050a0"));
        return;
    }
    QByteArray dataBytes = QByteArray::fromHex(hexInput.toUtf8());

    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);

    QByteArray writeReq;
    quint8 subCmd;
    if (m_14443ACardIsM1) {
        // M1: 一键 AuthWriteBlockM1(0x31)
        bool useKeyB = m_14443AKeyBBtn->isChecked();
        quint8 authMode = useKeyB ? RFID_AUTH_KEYB : RFID_AUTH_KEYA;
        QString keyHex = (useKeyB ? m_14443AKeyBEdit->text() : m_14443AKeyAEdit->text()).trimmed();
        keyHex.remove(' ');
        if (keyHex.length() != 12) {
            appendSystemLog(QString("[14443A 写块] Key 长度需 12 hex 字符 (6B), 当前 %1")
                                .arg(keyHex.length()), QColor("#c050a0"));
            return;
        }
        QByteArray keyBytes = QByteArray::fromHex(keyHex.toUtf8());
        if (keyBytes.size() != 6) {
            appendSystemLog("[14443A 写块] Key hex 解析失败", QColor("#c050a0"));
            return;
        }
        QString uidText = m_14443AUidEdit->text().trimmed();
        QByteArray uidBytes;
        if (!uidText.isEmpty() && uidText != "Uid") {
            uidBytes = QByteArray::fromHex(uidText.toUtf8());
            if (uidBytes.size() < 4) uidBytes.clear();
        }
        if (uidBytes.isEmpty()) uidBytes = QByteArray(4, 0);
        writeReq = m_parser->makeRfid14443AAuthWriteBlockM1(m_deviceAddr, blockAddr,
                                                            authMode, keyBytes,
                                                            uidBytes.left(4), dataBytes);
        subCmd = RFID_14443A_AUTH_WRITE_BLOCK;
    } else {
        // M0: 无认证 WriteBlockM0(0x20) 写 4B 页
        writeReq = m_parser->makeRfid14443AWriteBlockM0(m_deviceAddr, blockAddr, dataBytes);
        subCmd = RFID_14443A_WRITE_BLOCK_M0;
    }

    if (!sendRfidAndParse(0x14, subCmd, writeReq, nullptr, &err)) {
        appendSystemLog(QString("[14443A 写块] 失败: %1").arg(err), QColor("#c050a0"));
        return;
    }
    appendSystemLog(QString("[14443A 写块] 成功, addr=0x%1, %2")
                        .arg(hex2(blockAddr)).arg(m_14443ACardIsM1 ? "M1" : "M0"), QColor("#50a050"));
}

// Halt (Sw01 §8 0x01): [0x01] 无参 → 响应仅 result
void MainWindow::on14443AHalt()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("14443A Halt: 请先连接"); return; }
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfid14443AHalt(m_deviceAddr);
    if (!sendRfidAndParse(0x14, RFID_14443A_HALT, req, nullptr, &err)) {
        appendSystemLog(QString("[14443A Halt] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog("[14443A Halt] 成功", QColor("#50a050"));
}

// RATS (Sw01 §8 0x10): [0x10] 无参 → 响应 [result][ATS..] 变长, 输出到输出区
void MainWindow::on14443ARats()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("14443A RATS: 请先连接"); return; }
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfid14443ARats(m_deviceAddr);
    QByteArray payload;
    if (!sendRfidAndParse(0x14, RFID_14443A_RATS, req, &payload, &err)) {
        appendSystemLog(QString("[14443A RATS] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    QString hex = QString::fromLatin1(payload.toHex(' ').toUpper());
    m_14443AOutputArea->setPlainText(hex);
    appendSystemLog(QString("[14443A RATS] 成功, ATS(%1B)=%2").arg(payload.size()).arg(hex), QColor("#50a050"));
}

// TransAPDU (Sw01 §8 0x11): [0x11][apdu 1..64B] → 响应 [result][R-APDU..]; APDU 取输入区
void MainWindow::on14443ATransApdu()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("14443A TransAPDU: 请先连接"); return; }
    QString hexInput = m_14443AInputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    if (hexInput.length() < 2 || hexInput.length() > 128 || hexInput.length() % 2 != 0) {
        appendSystemLog(QString("[14443A TransAPDU] 输入区需 1..64B (偶数 hex 字符), 当前 %1")
                            .arg(hexInput.length()), QColor("#c050a0")); return;
    }
    QByteArray apdu = QByteArray::fromHex(hexInput.toUtf8());
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfid14443ATransApdu(m_deviceAddr, apdu);
    QByteArray payload;
    if (!sendRfidAndParse(0x14, RFID_14443A_TRANS_APDU, req, &payload, &err)) {
        appendSystemLog(QString("[14443A TransAPDU] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    QString hex = QString::fromLatin1(payload.toHex(' ').toUpper());
    m_14443AOutputArea->setPlainText(hex);
    appendSystemLog(QString("[14443A TransAPDU] 成功, R-APDU(%1B)=%2").arg(payload.size()).arg(hex), QColor("#50a050"));
}

// Topaz Read (Sw01 §8 0x12): [0x12][addr] → 响应 [result][data 8B]; addr 取块地址框
void MainWindow::on14443ATopazRead()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("14443A TopazRead: 请先连接"); return; }
    bool ok = false;
    quint8 addr = static_cast<quint8>(m_14443AAddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) { appendSystemLog("[14443A TopazRead] 块地址格式错误", QColor("#c050a0")); return; }
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfid14443ATopazRead(m_deviceAddr, addr);
    QByteArray payload;
    if (!sendRfidAndParse(0x14, RFID_14443A_TOPAZ_READ, req, &payload, &err)) {
        appendSystemLog(QString("[14443A TopazRead] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    QString hex = QString::fromLatin1(payload.toHex(' ').toUpper());
    m_14443AOutputArea->setPlainText(hex);
    appendSystemLog(QString("[14443A TopazRead] 成功, addr=0x%1, data(8B)=%2").arg(hex2(addr)).arg(hex), QColor("#50a050"));
}

// Topaz Write (Sw01 §8 0x13): [0x13][addr][data 8B] → 响应仅 result; addr 取块地址, data 8B 取输入区
void MainWindow::on14443ATopazWrite()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("14443A TopazWrite: 请先连接"); return; }
    bool ok = false;
    quint8 addr = static_cast<quint8>(m_14443AAddrEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) { appendSystemLog("[14443A TopazWrite] 块地址格式错误", QColor("#c050a0")); return; }
    QString hexInput = m_14443AInputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    if (hexInput.length() != 16) {
        appendSystemLog(QString("[14443A TopazWrite] 输入区需 8B (16 hex 字符), 当前 %1")
                            .arg(hexInput.length()), QColor("#c050a0")); return;
    }
    QByteArray data8 = QByteArray::fromHex(hexInput.toUtf8());
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req = m_parser->makeRfid14443ATopazWrite(m_deviceAddr, addr, data8);
    if (!sendRfidAndParse(0x14, RFID_14443A_TOPAZ_WRITE, req, nullptr, &err)) {
        appendSystemLog(QString("[14443A TopazWrite] 失败: %1").arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog(QString("[14443A TopazWrite] 成功, addr=0x%1").arg(hex2(addr)), QColor("#50a050"));
}

// ===== 钱包 (M1 值块, 固定块2) =====
// 协议 §8: 值块格式 [value 4B][~value 4B][value 4B][addr 1B][~addr 1B][pad 2B] (共16B)
// 钱包块号 = m_14443AWalletBlock (默认2), Inc/Dec 的 transAddr = 钱包块自身

// 取当前 Key(选中者) + uid, 返回 false 表示 Key 格式错
bool MainWindow::walletKeyUid(quint8 &authMode, QByteArray &keyBytes, QByteArray &uidBytes)
{
    bool useKeyB = m_14443AKeyBBtn->isChecked();
    authMode = useKeyB ? RFID_AUTH_KEYB : RFID_AUTH_KEYA;
    QString keyHex = (useKeyB ? m_14443AKeyBEdit->text() : m_14443AKeyAEdit->text()).trimmed();
    keyHex.remove(' ');
    if (keyHex.length() != 12) return false;
    keyBytes = QByteArray::fromHex(keyHex.toUtf8());
    if (keyBytes.size() != 6) return false;
    QString uidText = m_14443AUidEdit->text().trimmed();
    uidBytes.clear();
    if (!uidText.isEmpty() && uidText != "Uid") {
        uidBytes = QByteArray::fromHex(uidText.toUtf8());
        if (uidBytes.size() < 4) uidBytes.clear();
    }
    if (uidBytes.isEmpty()) uidBytes = QByteArray(4, 0);
    return true;
}

// 读回值块前4字节(LE) → 十进制填入余额框 (6字符居中)
void MainWindow::update14443AWalletBalance(const QByteArray &block16)
{
    if (block16.size() < 4) { if (m_14443AWalletBalEdit) m_14443AWalletBalEdit->setText("0"); return; }
    // 前4字节小端 → quint32
    quint32 val = static_cast<quint8>(block16[0])
                | (static_cast<quint8>(block16[1]) << 8)
                | (static_cast<quint8>(block16[2]) << 16)
                | (static_cast<quint8>(block16[3]) << 24);
    if (m_14443AWalletBalEdit)
        m_14443AWalletBalEdit->setText(QString::number(val));
}

// 钱包初始化: ① AuthWriteBlockM1(块2, value=0 值块) ② AuthInc(块2,块2,0x0A) ③ AuthReadBlockM1(块2) 读回验证
void MainWindow::on14443AWalletInit()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("钱包初始化: 请先连接"); return; }
    if (!m_14443ACardIsM1) { appendSystemLog("[钱包初始化] M0 卡无钱包 (仅 M1)", QColor("#d08020")); return; }
    quint8 authMode; QByteArray keyBytes, uidBytes;
    if (!walletKeyUid(authMode, keyBytes, uidBytes)) {
        appendSystemLog("[钱包初始化] Key 长度需 12 hex 字符 (6B)", QColor("#c050a0")); return;
    }
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);

    const quint8 blk = m_14443AWalletBlock;
    // ① 写块2 = value=0 的值块: 00 00 00 00 FF FF FF FF 00 00 00 00 02 FD 02 FD
    QByteArray initBlk = QByteArray::fromHex("00000000FFFFFFFF0000000002FD02FD");
    QByteArray wReq = m_parser->makeRfid14443AAuthWriteBlockM1(m_deviceAddr, blk, authMode,
                                                              keyBytes, uidBytes.left(4), initBlk);
    if (!sendRfidAndParse(0x14, RFID_14443A_AUTH_WRITE_BLOCK, wReq, nullptr, &err)) {
        appendSystemLog(QString("[钱包初始化] 写值块失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // ② AuthInc(valueAddr=块2, transAddr=块2, value=0x0A000000)
    QByteArray incVal(4, 0); incVal[0] = 0x0A;  // LE: 0x0000000A
    QByteArray iReq = m_parser->makeRfid14443AAuthInc(m_deviceAddr, blk, blk, authMode,
                                                      keyBytes, uidBytes.left(4), incVal);
    if (!sendRfidAndParse(0x14, RFID_14443A_AUTH_INC, iReq, nullptr, &err)) {
        appendSystemLog(QString("[钱包初始化] 增值失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // ③ 读块2验证
    QByteArray rReq = m_parser->makeRfid14443AAuthReadBlockM1(m_deviceAddr, blk, authMode,
                                                             keyBytes, uidBytes.left(4));
    QByteArray rPayload;
    if (!sendRfidAndParse(0x14, RFID_14443A_AUTH_READ_BLOCK, rReq, &rPayload, &err)) {
        appendSystemLog(QString("[钱包初始化] 读回验证失败: %1").arg(err), QColor("#c050a0")); return;
    }
    // 期望: 0A 00 00 00 F5 FF FF FF 0A 00 00 00 02 FD 02 FD
    QByteArray expect = QByteArray::fromHex("0A000000F5FFFFFF0A00000002FD02FD");
    QString hexData = QString::fromLatin1(rPayload.toHex(' ').toUpper());
    if (rPayload == expect) {
        update14443AWalletBalance(rPayload);  // 填余额 = 10
        appendSystemLog(QString("[钱包初始化] 成功, 块%1 = %2 (余额=10)").arg(blk).arg(hexData), QColor("#50a050"));
    } else {
        appendSystemLog(QString("[钱包初始化] 读回数据与期望不符: %1").arg(hexData), QColor("#d08020"));
    }
}

// 获取余额: 读块2 → 前4字节 LE → 十进制填余额框
void MainWindow::on14443AWalletGet()
{
    if (!m_transport || !m_transport->isOpen()) { appendSystemLog("获取余额: 请先连接"); return; }
    if (!m_14443ACardIsM1) { appendSystemLog("[获取余额] M0 卡无钱包 (仅 M1)", QColor("#d08020")); return; }
    quint8 authMode; QByteArray keyBytes, uidBytes;
    if (!walletKeyUid(authMode, keyBytes, uidBytes)) {
        appendSystemLog("[获取余额] Key 长度需 12 hex 字符 (6B)", QColor("#c050a0")); return;
    }
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    const quint8 blk = m_14443AWalletBlock;
    QByteArray rReq = m_parser->makeRfid14443AAuthReadBlockM1(m_deviceAddr, blk, authMode,
                                                             keyBytes, uidBytes.left(4));
    QByteArray rPayload;
    if (!sendRfidAndParse(0x14, RFID_14443A_AUTH_READ_BLOCK, rReq, &rPayload, &err)) {
        appendSystemLog(QString("[获取余额] 读块失败: %1").arg(err), QColor("#c050a0")); return;
    }
    if (rPayload.size() < 4) {
        appendSystemLog(QString("[获取余额] 读回数据不足4B: %1").arg(rPayload.size()), QColor("#c050a0")); return;
    }
    update14443AWalletBalance(rPayload);
    appendSystemLog(QString("[获取余额] 成功, 块%1 = %2")
                        .arg(blk).arg(QString::fromLatin1(rPayload.toHex(' ').toUpper())), QColor("#50a050"));
}

// 钱包增值 (0x32): value 取输入区 4B, valueAddr=transAddr=块2
void MainWindow::on14443AWalletInc()
{
    rfid14443AValueOp(RFID_14443A_AUTH_INC, "钱包增值");
}

// 钱包减值 (0x33): value 取输入区 4B, valueAddr=transAddr=块2
void MainWindow::on14443AWalletDec()
{
    rfid14443AValueOp(RFID_14443A_AUTH_DEC, "钱包减值");
}

// 钱包增/减公共流程 (0x32/0x33)
//   valueAddr = transAddr = 钱包块(块2); value(4B) 取输入区; 成功后读回更新余额
void MainWindow::rfid14443AValueOp(quint8 subCmd, const QString &name)
{
    if (!m_transport || !m_transport->isOpen()) {
        appendSystemLog(QString("14443A %1: 请先连接").arg(name)); return;
    }
    if (!m_14443ACardIsM1) {
        appendSystemLog(QString("[14443A %1] M0 卡无值块操作 (仅 M1)").arg(name), QColor("#d08020")); return;
    }
    quint8 authMode; QByteArray keyBytes, uidBytes;
    if (!walletKeyUid(authMode, keyBytes, uidBytes)) {
        appendSystemLog(QString("[14443A %1] Key 长度需 12 hex 字符 (6B)").arg(name), QColor("#c050a0")); return;
    }
    // value 4B 取输入区 (8 hex 字符)
    QString hexInput = m_14443AInputArea->toPlainText().trimmed();
    hexInput.remove(' ').remove('\n');
    if (hexInput.length() != 8) {
        appendSystemLog(QString("[14443A %1] 输入区需 4B 值 (8 hex 字符), 当前 %2")
                            .arg(name).arg(hexInput.length()), QColor("#c050a0")); return;
    }
    QByteArray valueBytes = QByteArray::fromHex(hexInput.toUtf8());

    const quint8 blk = m_14443AWalletBlock;
    if (!rfEnsureOpen14443A()) return;
    QString err;
    sendRfidCtrl(RFID_CTRL_DELAY, m_parser->makeRfidCtrlDelay(m_deviceAddr, 5), &err);
    QByteArray req;
    if (subCmd == RFID_14443A_AUTH_INC)
        req = m_parser->makeRfid14443AAuthInc(m_deviceAddr, blk, blk, authMode,
                                              keyBytes, uidBytes.left(4), valueBytes);
    else
        req = m_parser->makeRfid14443AAuthDec(m_deviceAddr, blk, blk, authMode,
                                              keyBytes, uidBytes.left(4), valueBytes);
    if (!sendRfidAndParse(0x14, subCmd, req, nullptr, &err)) {
        appendSystemLog(QString("[14443A %1] 失败: %2").arg(name).arg(err), QColor("#c050a0")); return;
    }
    appendSystemLog(QString("[14443A %1] 成功, 块=0x%2").arg(name).arg(hex2(blk)), QColor("#50a050"));
    // 成功后自动读回更新余额
    on14443AWalletGet();
}

// ============================ Debug Panel ============================

void MainWindow::onDebugToggle()
{
    m_debugPanel->setVisible(!m_debugPanel->isVisible());
}

void MainWindow::onDebugPack()
{
    QString addrText = m_debugAddrEdit->text().trimmed();
    bool ok;
    quint8 devAddr = static_cast<quint8>(addrText.toInt(&ok, 16));
    if (!ok) {
        appendSystemLog("通信调试: 地址格式错误", QColor("#c050a0"));
        return;
    }

    quint8 fc = static_cast<quint8>(m_debugFcEdit->text().trimmed().toInt(&ok, 16));
    if (!ok) {
        appendSystemLog("通信调试: 功能码格式错误", QColor("#c050a0"));
        return;
    }

    QByteArray paramData;
    QString paramText = m_debugParamEdit->text().trimmed();
    if (!paramText.isEmpty()) {
        QString hex = paramText;
        hex.remove(' ');
        paramData = QByteArray::fromHex(hex.toUtf8());
        if (paramData.size() > 512) {
            appendSystemLog(QString("通信调试: 参数过长 %1 > 512").arg(paramData.size()), QColor("#c050a0"));
            return;
        }
    }

    QByteArray frame = m_parser->makeFrame(devAddr, fc, paramData);
    m_debugFrameLabel->setText(frame.toHex(' ').toUpper());

    // 清空发送区并填入组帧结果
    m_sendEdit->clear();
    m_sendEdit->setPlainText(frame.toHex(' ').toUpper());
    m_sendHexCheck->setChecked(true);

    appendSystemLog(QString("通信调试: 已组帧，填入发送区"));
}

// ============================ Connection State ============================

void MainWindow::updateConnectionState(bool isConnected)
{
    m_ifaceCombo->setEnabled(false); // 始终禁用 (端口固定 USB)
    // m_refreshBtn removed (Handshake Area 无刷新按钮, 刷新由 onOpen 自动选 VID 5377)
    m_openBtn->setText(isConnected ? tr("DisConnect") : tr("Connect"));
    m_openBtn->setProperty("connected", isConnected);
    m_openBtn->style()->unpolish(m_openBtn);
    m_openBtn->style()->polish(m_openBtn);

    // 串口打开后解锁：发送区、功能区. 日志区/清空键始终启用(断开也能复制日志/手动清空).
    m_logHexCheck->setEnabled(isConnected);
    m_sendBtn->setEnabled(isConnected);
    m_sendHexCheck->setEnabled(isConnected);
    m_sendEdit->setEnabled(isConnected);
    m_timerSendCheck->setEnabled(isConnected);
    m_timerSendSpin->setEnabled(isConnected);

    if (isConnected) {
        enableFuncButtons(true);
    } else {
        m_funcUnlocked = false;  // 断开时重置握手状态, 下次连接需重新握手才开放其他按钮
        enableFuncButtons(false);
        enableRfFuncButtons(false);  // 断开时禁用 Sccd 内卡操作按钮
        // 握手按钮恢复红色 (未握手)
        m_funcHandshakeBtn->setProperty("handshaked", false);
        m_funcHandshakeBtn->style()->unpolish(m_funcHandshakeBtn);
        m_funcHandshakeBtn->style()->polish(m_funcHandshakeBtn);
        m_funcStatusLabel->setText("● 设备未连接");
        m_funcStatusLabel->setStyleSheet("color: #b0a0c8; font-size: 0.82em;");
        m_timerSendCheck->setChecked(false);
        m_timerSend->stop();
    }
}

void MainWindow::enableFuncButtons(bool enable)
{
    // 握手按钮: 设备已连接即可用 (其他按钮握手成功后才开放)
    m_funcHandshakeBtn->setEnabled(enable);
    m_verAddrEdit->setEnabled(enable);
    // 其余 Fun 区按钮: 握手成功 (m_funcUnlocked) 后才开放
    m_updateBtn->setEnabled(enable && m_funcUnlocked);
    m_debugBtn->setEnabled(enable && m_funcUnlocked);
    m_sccdBtn->setEnabled(enable && m_funcUnlocked);
    if (m_zlrBtn) m_zlrBtn->setEnabled(enable && m_funcUnlocked);
    m_protoEnterBootBtn->setEnabled(enable && m_funcUnlocked);
    m_protoResetBtn->setEnabled(enable && m_funcUnlocked);
    // 射频控制按钮: 握手成功后开放 (Sccd Area 内)
    m_rfOpenBtn->setEnabled(enable && m_funcUnlocked);
    m_rfCloseBtn->setEnabled(enable && m_funcUnlocked);
    m_rfResetBtn->setEnabled(enable && m_funcUnlocked);
}

// Round028: 握手后按 SW 字段启用 Supported Devices 区按钮 (互斥 + 不匹配则全禁用)
//   swText: SW 字段值 (如 "ZLR5401_V1.0" / "SCCD_V1.0" / 空)
//   规则:
//     以 "ZLR5401" 开头 -> 仅启用 ZLR5401, Sccd 按钮灰禁用 + 区域隐藏
//     以 "SCCD"    开头 -> 仅启用 Sccd,   ZLR5401 按钮灰禁用 + 区域隐藏
//     其它 / 空        -> 两个都禁用, 区域隐藏 (握手前默认状态)
void MainWindow::enableSuppBySw(const QString &swText)
{
    if (!m_sccdBtn || !m_zlrBtn) return;
    QString s = swText.trimmed().toUpper();
    if (s.startsWith("ZLR5401")) {
        m_sccdBtn->setEnabled(false);
        m_sccdBtn->setChecked(false);          // 收起 Sccd 区域
        m_zlrBtn->setEnabled(true);
        m_zlrBtn->setChecked(true);             // 自动展开 ZLR5401 区域
    } else if (s.startsWith("SCCD")) {
        m_sccdBtn->setEnabled(true);
        m_sccdBtn->setChecked(true);            // 自动展开 Sccd 区域
        m_zlrBtn->setEnabled(false);
        m_zlrBtn->setChecked(false);            // 收起 ZLR5401 区域
    } else {
        // 其它 / 空 - 全禁用
        m_sccdBtn->setEnabled(false);
        m_sccdBtn->setChecked(false);
        m_zlrBtn->setEnabled(false);
        m_zlrBtn->setChecked(false);
    }
}

void MainWindow::resetUiToInitialState()
{
    // 版本信息
    if (m_verAddrEdit) m_verAddrEdit->clear();
    if (m_verSwEdit)   m_verSwEdit->clear();
    if (m_verHwEdit)   m_verHwEdit->clear();

    // 日志区: 不主动清空(仅"清空"键清), 新连接/断开后旧日志保留且可复制. 发送区/接收缓冲仍复位.
    if (m_sendEdit)  m_sendEdit->clear();
    m_rxBuffer.clear();

    // Sccd Area 收起
    if (m_sccdBtn && m_sccdBtn->isChecked()) {
        m_sccdBtn->setChecked(false);   // 触发 onSccdToggle 隐藏 m_sccdBox + 复位按钮底色
    }
    // ZLR5401 Area 收起
    if (m_zlrBtn && m_zlrBtn->isChecked()) {
        m_zlrBtn->setChecked(false);    // 触发 onZlrToggle 隐藏 m_zlrBox + 复位按钮底色
    }
    // 复位 ZLR5401 区域标题/提示 (断开后还原默认)
    if (m_zlrBox) m_zlrBox->setTitle(tr("ZLR5401 Area"));
    if (m_zlrBtn) m_zlrBtn->setToolTip(tr("切换 ZLR5401 Area 显示/隐藏 (电机 / UHF / AM / 开锁器 / RGB)"));
    // Round029 D5: 同步刷新窗口标题
    if (m_zlrBtn) refreshWindowTitleArea();
    // Round028: 断开后按 SW 重置 Supported Devices 区按钮 (握手前已全禁用, 此处确保)
    enableSuppBySw("");

    // 14443A Tab
    if (m_14443AUidEdit)     m_14443AUidEdit->clear();
    if (m_14443ACardTypeEdit) m_14443ACardTypeEdit->clear();              // 卡类型清空
    m_14443ACardIsM1 = false;                                            // 卡类型标记复位
    if (m_14443AAddrEdit)    m_14443AAddrEdit->setText("00");              // 块地址默认 0x00
    if (m_14443AKeyAEdit)    m_14443AKeyAEdit->setText("FFFFFFFFFFFF");    // KeyA 默认 FFFFFFFFFFFF
    if (m_14443AKeyBEdit)   m_14443AKeyBEdit->setText("FFFFFFFFFFFF");    // KeyB 默认 FFFFFFFFFFFF
    if (m_14443AKeyABtn)    { m_14443AKeyABtn->setChecked(true);  m_14443AKeyBBtn->setChecked(false); }
    if (m_14443AInputArea)  m_14443AInputArea->clear();
    if (m_14443AOutputArea) m_14443AOutputArea->clear();
    if (m_14443AWalletBalEdit) m_14443AWalletBalEdit->setText("0");   // 余额复位

    // 15693 Tab
    if (m_15693UidEdit)     m_15693UidEdit->clear();
    if (m_15693AddrEdit)    m_15693AddrEdit->setText("00");           // 块地址默认 0x00
    if (m_15693CountEdit)   m_15693CountEdit->setText("01");          // 块数默认 1
    if (m_15693InputArea)   m_15693InputArea->clear();
    if (m_15693OutputArea)  m_15693OutputArea->clear();
}

// ============================ Wait For Response ============================

bool MainWindow::waitForResponse(int timeoutMs)
{
    m_replyTimer->stop();
    QByteArray snapshot = m_rxBuffer;
    int originalSize = snapshot.size();

    QElapsedTimer timer;
    timer.start();
    bool gotData = false;
    while (timer.elapsed() < timeoutMs) {
        // Round 019: input_cb 已在 CFRunLoop 线程直接 append (锁保护), readAll 锁内
        // 拷贝清空 m_rxBuf. processEvents+readAll 连续短间隔轮询, 缩短 msleep 窗口
        // (原 5ms → 1ms) 减少两次轮询间 IN report 到达 → 下次 readAll 才被拉走 的延迟,
        // 避免短超时窗口内错过 RX (FC_UPGRADE_START 1B result, ~0.3s 回, 之前 5ms 窗口临界).
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QThread::msleep(1);
        if (m_transportType == TransportType::Usb && m_transport) {
            QByteArray polled = m_transport->readAll();
            if (!polled.isEmpty()) m_rxBuffer.append(polled);
        }
        if (m_rxBuffer.size() > originalSize) {
            gotData = true;
            // 等待数据流停止（帧间间隔>30ms视为帧结束）
            int lastSize = m_rxBuffer.size();
            QElapsedTimer settle;
            settle.start();
            while (settle.elapsed() < 200) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
                if (m_transportType == TransportType::Usb && m_transport) {
                    QByteArray polled = m_transport->readAll();
                    if (!polled.isEmpty()) m_rxBuffer.append(polled);
                }
                if (m_rxBuffer.size() != lastSize) {
                    lastSize = m_rxBuffer.size();
                    settle.restart();
                }
                if (settle.elapsed() > m_frameGapSpin->value() && m_rxBuffer.size() == lastSize)
                    break;
            }
            break;
        }
    }
    m_replyTimer->stop();
    return gotData;
}

// ============================ Log ============================

void MainWindow::appendLog(const QString &prefix, const QByteArray &data, const QColor &color, const QString &note)
{
    QString timestamp = QTime::currentTime().toString("HH:mm:ss.zzz");
    QString content;
    if (m_logHexCheck->isChecked()) {
        // 一行连续 hex, 不按 32 字节切分
        content = data.toHex(' ').toUpper();
    } else {
        content = QString::fromUtf8(data);
    }

    QString html = QString("<span style=\"color:#b0a0c8\">%1</span> <span style=\"color:%2;font-weight:%3\">%4</span> %5")
        .arg(timestamp)
        .arg(color.name())
        .arg(prefix == "[TX]" ? "bold" : "normal")
        .arg(prefix)
        .arg(content.toHtmlEscaped());

    if (!note.isEmpty()) {
        html += QString(" <span style=\"color:#c050a0;font-style:italic\">(%1)</span>").arg(note);
    }

    m_logDisplay->append(html);
    m_logDisplay->moveCursor(QTextCursor::End);
    m_saveLogBtn->setEnabled(true);
}

void MainWindow::appendSystemLog(const QString &msg, const QColor &color)
{
    QString timestamp = QTime::currentTime().toString("HH:mm:ss.zzz");
    m_logDisplay->append(QString("<span style=\"color:#b0a0c8\">%1</span> <span style=\"color:%2\">[SYS]</span> %3")
        .arg(timestamp)
        .arg(color.name())
        .arg(msg.toHtmlEscaped()));
    m_logDisplay->moveCursor(QTextCursor::End);
    m_saveLogBtn->setEnabled(true);
}

// ============================ Save Log ============================

void MainWindow::onSaveLog()
{
    QDir logDir(QCoreApplication::applicationDirPath() + "/log");
    if (!logDir.exists()) {
        logDir.mkpath(".");
    }

    QString fileName = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".md";
    QString filePath = logDir.filePath(fileName);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法创建日志文件: %1").arg(filePath));
        return;
    }

    QTextStream out(&file);
    out << "# ComforTool 日志\n\n";
    out << "**保存时间:** " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n\n";
    out << "## 日志内容\n\n";

    QString plain = m_logDisplay->toPlainText();
    QStringList lines = plain.split('\n');
    for (const QString &line : lines) {
        out << line << "\n";
    }

    file.close();
    appendSystemLog(QString("日志已保存: %1").arg(filePath));
}

// ============================ Upgrade ============================

void MainWindow::onUpdateFirmware()
{
    QString binPath = QFileDialog::getOpenFileName(
        this, tr("选择固件文件"), QString(), tr("BIN文件 (*.bin)"));
    if (binPath.isEmpty()) return;

    appendSystemLog(QString("开始固件更新: %1").arg(QFileInfo(binPath).fileName()));
    flashControls(true);
    startUpgrade(binPath);
}

// ============================ Upgrade ============================

// 内部: 等待并解析Boot握手响应，返回layer值，失败返回-1
// 用广播 0xFF 握手: 设备在 Boot 层地址可能与 App 不同(UPG 态曾回 0xff/0x00),
// 0x01 在某些状态下被静默丢弃, 广播必响应且回告真实 devAddr, 与 Search 路径一致.
int waitBootHandshake(MainWindow *mw, ITransport *tp, quint8 &outDevAddr, quint8 &outStatus, quint32 &outDeviceUidHash)
{
    mw->getRxBuffer().clear();
    QByteArray req = mw->getParser()->makeHandshakeFrame(0xFF);
    tp->writeData(req);
    tp->waitForBytesWritten(5000);
    mw->appendLog("[TX]", req, QColor("#c050a0"));

    if (!mw->waitForResponse(5000))
        return -1;

    FrameData frame = mw->getParser()->parseFrame(mw->getRxBuffer());
    if (!frame.valid || frame.fc != 0xFE)
        return -1;

    // Round 019 HANDSHAKE 28B (App/Boot 一致, 移除冗余 version 字段):
    //   [0]result [1]protoVer [2]status [3..14]UID(12B) [15..18]uidHash(LE)
    //   [19]layer [20..23]upgradeCount(LE) [24..27]baudRate(LE)
    // 复用 ProtocolParser::parseDeviceInfo (其 28B 分支与本布局一致), 避免再次偏移错位
    DeviceInfo info = mw->getParser()->parseDeviceInfo(frame.data);
    if (!info.valid)
        return -1;

    outDevAddr = frame.devAddr;
    outStatus = info.deviceStatus;
    outDeviceUidHash = info.uidHash; // ★ FULL 绑定校验 BindVerify 依赖此值, 旧代码误取 data[2..5]

    return static_cast<int>(info.layer); // ★ 旧代码误取 data[6](UID 字节), 导致 Boot/App 误判
}

// 内部: 发送FC=0x02进入Boot + FC=0x08触发软复位, 等待设备复位
//   协议: ENTER_BOOT 回 OK 后 App 写 status=UPG, 之后会自动软复位进 Boot;
//   但个别固件版本 ENTER_BOOT 后不自动复位, 故双发 RESET(0x08) 作双保险.
//   RESET 响应可能来不及回(设备已复位), 已容错: waitForResponse(500) 不强求.
bool enterBoot(MainWindow *mw, ITransport *tp, quint8 devAddr)
{
    ProtocolParser *parser = mw->getParser();

    // ① FC=0x02 ENTER_BOOT: App 写 status=UPG (不自动复位)
    mw->getRxBuffer().clear();
    QByteArray req = parser->makeEnterBootFrame(devAddr);
    tp->writeData(req);
    tp->waitForBytesWritten(5000);
    mw->appendLog("[TX]", req, QColor("#c050a0"));
    if (mw->waitForResponse(3000)) {
        FrameData eb = parser->parseFrame(mw->getRxBuffer());
        if (eb.valid && eb.fc == (0x02 ^ 0xFF) && !eb.data.isEmpty()) {
            quint8 r = static_cast<quint8>(eb.data[0]);
            if (r != 0x00) {
                mw->appendSystemLog(QString("ENTER_BOOT 失败: result=0x%1 (0x02=OK, 0x02=PARAM_ERR)")
                                    .arg(hex2(r)), QColor("#c050a0"));
                return false;
            }
        }
    } else {
        // 个别固件 ENTER_BOOT 后即复位, 无响应也容错继续
        mw->appendSystemLog("ENTER_BOOT 无响应(设备可能已复位), 继续发 RESET", QColor("#d08020"));
    }

    // ② FC=0x08 RESET: 触发 ~20ms 后 NVIC_SystemReset → Boot
    mw->getRxBuffer().clear();
    QByteArray rst = parser->makeResetFrame(devAddr);
    tp->writeData(rst);
    tp->waitForBytesWritten(5000);
    mw->appendLog("[TX]", rst, QColor("#c050a0"));
    mw->waitForResponse(500); // RESET 响应可能来不及回, 不强求
    QThread::msleep(200);      // 设备复位 + USB 重新枚举需要时间
    return true;
}

// 内部: 执行一个固件数据帧发送，超时返回false
bool sendFirmwareChunk(MainWindow *mw, ITransport *tp,
                               ProtocolParser *parser, quint8 devAddr,
                               quint16 seq, int offset,
                               const QByteArray &fwData, int chunkSize)
{
    mw->getRxBuffer().clear(); // 先清RX再发，避免串入旧数据
    QByteArray chunk = fwData.mid(offset, chunkSize);
    QByteArray req = parser->makeUpgradeDataFrame(devAddr, seq, static_cast<quint16>(offset), chunk);
    tp->writeData(req);
    tp->waitForBytesWritten(5000); // 等待实际发送完成
    mw->appendLog("[TX]", req, QColor("#c050a0"));
    mw->appendSystemLog(QString("[seq=%1 chunk=%2] 等待响应(60s)...").arg(seq).arg(chunkSize));

    bool gotData = mw->waitForResponse(60000);
    QByteArray &rxBuf = mw->getRxBuffer();
    if (!rxBuf.isEmpty())
        mw->appendLog("[RX]", rxBuf, QColor("#30b078"));
    mw->appendSystemLog(QString("[seq=%1] 收到%2字节: %3")
        .arg(seq).arg(rxBuf.size())
        .arg(rxBuf.size() > 0 ? rxBuf.toHex(' ').left(80).constData() : "无"));

    if (!gotData || rxBuf.isEmpty()) {
        mw->appendSystemLog(QString("固件数据帧%1超时，无响应").arg(seq), QColor("#c050a0"));
        return false;
    }

    FrameData resp = parser->parseFrame(rxBuf);
    mw->appendSystemLog(QString("[seq=%1] 帧解析: valid=%2 fc=0x%3")
        .arg(seq).arg(resp.valid).arg(hex2(resp.fc)));
    if (!resp.valid) {
        mw->appendSystemLog(QString("帧解析失败: %1").arg(resp.error), QColor("#c050a0"));
        return false;
    }
    if (resp.fc != (0x04 ^ 0xFF)) {
        mw->appendSystemLog(QString("FC错误: 期望0x%1 实际0x%2")
            .arg(hex2(0x04 ^ 0xFF)).arg(hex2(resp.fc)), QColor("#c050a0"));
        return false;
    }

    // 协议: Data[0~1]=Seq, Data[2]=Result
    quint8 result = resp.data.size() >= 3 ? static_cast<quint8>(resp.data[2]) : 0xFF;
    mw->appendSystemLog(QString("[seq=%1] Result=0x%2").arg(seq).arg(hex2(result)));
    if (result != 0x00) {
        mw->appendSystemLog(QString("固件数据帧%1写入失败: %2").arg(seq)
            .arg(ProtocolParser::resultToString(result, true)), QColor("#c050a0"));
        return false;
    }
    return true;
}

// 内部: 检查升级是否完成(Layer=1)或仍需重新升级(Layer=0)
bool checkUpgradeDoneAfterExec(MainWindow *mw, ITransport *tp)
{
    mw->appendSystemLog("等待设备重启(3秒)...");
    QThread::sleep(3);

    quint8 devAddr = 0;
    quint8 status = 0;
    quint32 dummyUidHash = 0;
    int layer = waitBootHandshake(mw, tp, devAddr, status, dummyUidHash);
    if (layer < 0) {
        mw->appendSystemLog("重启后握手超时", QColor("#c050a0"));
        return false;
    }
    if (layer == 1) {
        mw->appendSystemLog("升级成功! 设备已进入APP", QColor("#50a050"));
        return true;
    } else {
        mw->appendSystemLog(QString("重启后Layer=%1，需要重新升级").arg(layer), QColor("#c050a0"));
        return false;
    }
}


void MainWindow::startUpgrade(const QString &binPath)
{
    m_upgrading = true;  // 升级流程开始: 期间 close transport 不清空日志区
    QFile file(binPath);
    if (!file.open(QIODevice::ReadOnly)) {
        appendSystemLog(QString("固件文件打开失败: %1").arg(file.errorString()), QColor("#c050a0"));
        endUpgrade(false);
        return;
    }
    QByteArray fwData = file.readAll();
    file.close();
    quint32 fwSize = fwData.size();
    quint32 fwCrc = ProtocolParser::crc32(reinterpret_cast<const quint8*>(fwData.constData()), fwData.size());

    appendSystemLog(QString("固件: %1 bytes, CRC32=0x%2")
        .arg(fwSize).arg(fwCrc, 8, 16, QChar('0')).toUpper());
    appendSystemLog("[升级] 步骤1/6: Boot层握手...", QColor("#7830b0"));

    const int CHUNK_SIZE = (m_transportType == TransportType::Usb) ? 48 : 512;
    int totalFrames = (fwSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    m_logProgressBar->setMaximum(totalFrames);
    m_logProgressBar->setValue(0);
    m_logProgressBar->setVisible(true);
    QCoreApplication::processEvents();

    // ===== Step 1: Boot层握手 + Layer!=0时自动发FC=0x02 =====
    quint8 bootDevAddr = 0;
    quint8 bootStatus = 0;
    quint32 deviceUidHash = 0;
    int layer = waitBootHandshake(this, m_transport, bootDevAddr, bootStatus, deviceUidHash);
    if (layer < 0) {
        appendSystemLog("Boot握手超时或解析失败", QColor("#c050a0"));
        endUpgrade(false);
        return;
    }

    QStringList statusNames = {"IDLE", "RUN", "UPG", "FAULT"};
    QString statusStr = bootStatus < statusNames.size() ? statusNames[bootStatus] : QString("未知(%1)").arg(bootStatus);
    appendSystemLog(QString("握手成功: Layer=%1, Status=%2, DevAddr=0x%3, UID=0x%4")
        .arg(layer).arg(statusStr).arg(hex2(bootDevAddr))
        .arg(QString("%1").arg(deviceUidHash, 8, 16, QChar('0'))).toUpper());

    if (layer != 0) {
        appendSystemLog(QString("设备在APP层(Layer=%1)，发送进入Boot指令...").arg(layer), QColor("#d08020"));
        enterBoot(this, m_transport, bootDevAddr);

        // USB 模式: 设备软复位后 HID 重新枚举, 旧 handle 失效, 需重连
        bool usbReopened = true;
        if (m_transportType == TransportType::Usb) {
#ifdef COMFORTOOL_ENABLE_USB
            m_transport->close();                 // 关闭旧 HID handle
            m_transport = nullptr;
            appendSystemLog("USB: 设备进入Boot, 等待重新枚举(最多10s)...", QColor("#d08020"));
            bool found = false;
            for (int i = 0; i < 20 && !found; ++i) {
                QThread::msleep(500);
                auto devs = HidManager::enumerate(0x5377, 0x5378);
                if (!devs.isEmpty()) {
                    QVariantMap params;
                    params["path"] = devs.first().path;
                    if (m_hidManager->open(params)) {
                        m_transport = m_hidManager;
                        found = true;
                    }
                }
            }
            if (!found) {
                appendSystemLog("USB: 重新枚举超时, 未发现设备", QColor("#c050a0"));
                endUpgrade(false);
                return;
            }
            appendSystemLog("USB: 已重新连接设备", QColor("#50a050"));
            usbReopened = true;
#endif
        } else {
            // COM 模式: 端口常驻, 仅轮询握手确认进入 Boot
            QThread::msleep(500);
        }

        // 轮询握手最多10秒确认 layer==0
        bool bootOk = false;
        quint32 reUidHash = 0;
        for (int i = 0; i < 20; ++i) {
            if (i > 0 || m_transportType == TransportType::Usb) QThread::msleep(500);
            layer = waitBootHandshake(this, m_transport, bootDevAddr, bootStatus, reUidHash);
            if (layer == 0) {
                bootOk = true;
                deviceUidHash = reUidHash;
                break;
            }
        }
        if (!bootOk) {
            appendSystemLog("进入Boot后握手超时", QColor("#c050a0"));
            endUpgrade(false);
            return;
        }
        appendSystemLog(QString("已进入Boot层 Layer=%1").arg(layer));
    }

    // ===== Step 2: 获取设备信息 =====
    appendSystemLog("[升级] 步骤2/6: 获取设备UID...", QColor("#7830b0"));
    QCoreApplication::processEvents();
    QByteArray reqInfo = m_parser->makeDeviceInfoFrame(bootDevAddr);
    m_rxBuffer.clear();
    m_transport->writeData(reqInfo);
    m_transport->waitForBytesWritten(5000);
    appendLog("[TX]", reqInfo, QColor("#c050a0"));

    if (!waitForResponse(3000)) {
        appendSystemLog("设备信息请求超时", QColor("#c050a0"));
        endUpgrade(false);
        return;
    }

    FrameData infoFrame = m_parser->parseFrame(m_rxBuffer);
    if (!infoFrame.valid || infoFrame.fc != (0x07 ^ 0xFF)) {
        appendSystemLog("设备信息解析失败", QColor("#c050a0"));
        endUpgrade(false);
        return;
    }

    if (!infoFrame.data.isEmpty() && static_cast<quint8>(infoFrame.data[0]) != 0x00) {
        appendSystemLog(QString("设备信息请求失败: Result=0x%1")
            .arg(hex2(static_cast<int>(infoFrame.data[0]))), QColor("#c050a0"));
        endUpgrade(false);
        return;
    }

    quint32 bindVerify = ProtocolParser::calcBindVerify(
        reinterpret_cast<const quint8*>(fwData.constData()), fwData.size(), deviceUidHash);
    appendSystemLog(QString("BindVerify=0x%1 (由固件CRC+UID哈希计算)")
        .arg(bindVerify, 8, 16, QChar('0')).toUpper(), QColor("#8870a8"));

    // ===== Step 3+4: 升级开始 + 固件数据（超时重试，最多3次从头开始）=====
    const int MAX_RETRIES = 3;
    bool upgradeSuccess = false;

    for (int attempt = 1; attempt <= MAX_RETRIES && !upgradeSuccess; ++attempt) {
        if (attempt > 1) {
            appendSystemLog(QString("传输中断，第%1次重试...").arg(attempt), QColor("#d08020"));
            // 重试前重新握手确认设备状态
            m_rxBuffer.clear();
            quint8 retryAddr = 0, retryStatus = 0;
            quint32 retryUidHash = 0;
            int retryLayer = waitBootHandshake(this, m_transport, retryAddr, retryStatus, retryUidHash);
            if (retryLayer < 0) {
                appendSystemLog("重试握手超时，跳过本轮", QColor("#c050a0"));
                continue;
            }
            if (retryLayer != 0) {
                appendSystemLog(QString("重试时设备已进入APP(Layer=%1)，无法继续升级").arg(retryLayer), QColor("#c050a0"));
                endUpgrade(false);
                return;
            }
            bootDevAddr = retryAddr;
            m_logProgressBar->setValue(0);
            QCoreApplication::processEvents();
        }
        appendSystemLog(QString("[升级] 步骤3/6: 擦除Flash(%1/%2)...").arg(attempt).arg(MAX_RETRIES), QColor("#7830b0"));
        QCoreApplication::processEvents();
        // 校验级别硬编码 FULL(0): CRC+Size+向量+绑定, 固件必须过设备绑定校验, 生产级安全
        const quint8 verifyLevel = 0;
        quint32 fwVer = 0;
        QByteArray reqStart = m_parser->makeUpgradeStartFrame(bootDevAddr, fwSize, fwCrc, fwVer, bindVerify, verifyLevel);
        appendSystemLog(QString("校验级别: 全级(FULL, CRC+Size+向量+绑定)"));
        m_rxBuffer.clear();
        m_transport->writeData(reqStart);
        m_transport->waitForBytesWritten(5000);
        appendLog("[TX]", reqStart, QColor("#c050a0"));

        if (!waitForResponse(15000)) {
            appendSystemLog("升级开始超时", QColor("#c050a0"));
            continue; // 重试
        }

        FrameData startFrame = m_parser->parseFrame(m_rxBuffer);
        if (!startFrame.valid || startFrame.fc != (0x03 ^ 0xFF)) {
            appendSystemLog("升级开始响应解析失败", QColor("#c050a0"));
            continue;
        }

        quint8 startResult = startFrame.data.isEmpty() ? 0 : static_cast<quint8>(startFrame.data[0]);
        if (startResult != 0x00) {
            appendSystemLog(QString("升级开始失败: %1").arg(ProtocolParser::resultToString(startResult, true)), QColor("#c050a0"));
            continue;
        }
        appendSystemLog("Flash擦除成功");
        m_logProgressBar->setValue(1);

        // Step 4: 固件数据
        appendSystemLog("[升级] 步骤4/6: 传输固件数据...", QColor("#7830b0"));
        QCoreApplication::processEvents();

        quint16 seq = 0;
        bool transferFailed = false;
        for (int offset = 0; offset < fwSize; offset += CHUNK_SIZE) {
            int chunkSize = qMin(CHUNK_SIZE, static_cast<int>(fwSize - offset));
            if (!sendFirmwareChunk(this, m_transport, m_parser, bootDevAddr, seq, offset, fwData, chunkSize)) {
                appendSystemLog(QString("固件数据帧%1传输失败，重试中...").arg(seq), QColor("#d08020"));
                transferFailed = true;
                break;
            }
            seq++;
            m_logProgressBar->setValue(1 + seq);
            // 进度日志: 每 32 帧或最后一帧打一次, 避免日志区刷屏 (TX/RX 帧已自带 seq)
            if (seq % 32 == 0 || seq == totalFrames)
                appendSystemLog(QString("[升级] 传输中 %1/%2 帧 (%3%)")
                                .arg(seq).arg(totalFrames)
                                .arg(seq * 100 / totalFrames), QColor("#8870a8"));
            QCoreApplication::processEvents();
        }

        if (transferFailed)
            continue;

        // Step 5: 升级校验
        appendSystemLog("[升级] 步骤5/6: 校验固件...", QColor("#7830b0"));
        QCoreApplication::processEvents();
        QByteArray reqVerify = m_parser->makeUpgradeVerifyFrame(bootDevAddr);
        m_rxBuffer.clear();
        m_transport->writeData(reqVerify);
        m_transport->waitForBytesWritten(5000);
        appendLog("[TX]", reqVerify, QColor("#c050a0"));

        if (!waitForResponse(10000)) {
            appendSystemLog("升级校验超时", QColor("#c050a0"));
            continue;
        }

        if (!m_rxBuffer.isEmpty())
            appendLog("[RX]", m_rxBuffer, QColor("#30b078"));
        FrameData verifyFrame = m_parser->parseFrame(m_rxBuffer);
        if (!verifyFrame.valid || verifyFrame.fc != (0x05 ^ 0xFF)) {
            appendSystemLog("升级校验响应解析失败", QColor("#c050a0"));
            continue;
        }

        quint8 verifyResult = verifyFrame.data.isEmpty() ? 0 : static_cast<quint8>(verifyFrame.data[0]);
        quint8 verifyDetail = verifyFrame.data.size() >= 2 ? static_cast<quint8>(verifyFrame.data[1]) : 0;
        if (verifyResult != 0x00) {
            QString errMsg = ProtocolParser::verifyResultToString(verifyResult, verifyDetail);
            // CRC(1)/SIZE(2) 失败: 重传 DATA 可恢复(重发 START 重置到 ERASED 再传一轮).
            // VECTOR(3)/BIND(4) 失败: 固件头部坏或设备绑定不匹配, 重传无益, 直接终止.
            if (verifyResult == 1 || verifyResult == 2) {
                appendSystemLog(QString("固件校验失败: %1, 重试重新传输").arg(errMsg), QColor("#c050a0"));
                continue;
            }
            appendSystemLog(QString("固件校验失败: %1，请检查固件文件/绑定配置").arg(errMsg), QColor("#c050a0"));
            endUpgrade(false);
            return;
        }
        appendSystemLog("固件校验通过");
        m_logProgressBar->setValue(totalFrames + 1);
        // Step 6: 升级执行
        appendSystemLog("[升级] 步骤6/6: 执行升级...", QColor("#7830b0"));
        QCoreApplication::processEvents();
        QByteArray reqExec = m_parser->makeUpgradeExecFrame(bootDevAddr);
        m_rxBuffer.clear();
        m_transport->writeData(reqExec);
        m_transport->waitForBytesWritten(5000);
        appendLog("[TX]", reqExec, QColor("#c050a0"));

        // 按文档9.5: 超时不放弃，等待3秒后握手检查Layer
        bool gotExecResp = waitForResponse(3000);
        if (gotExecResp) {
            if (!m_rxBuffer.isEmpty())
                appendLog("[RX]", m_rxBuffer, QColor("#30b078"));
            FrameData execFrame = m_parser->parseFrame(m_rxBuffer);
            if (execFrame.valid && execFrame.fc == (0x06 ^ 0xFF)) {
                quint8 execResult = execFrame.data.isEmpty() ? 0 : static_cast<quint8>(execFrame.data[0]);
                if (execResult != 0x00) {
                    appendSystemLog(QString("升级执行失败: %1").arg(ProtocolParser::resultToString(execResult, true)), QColor("#c050a0"));
                    endUpgrade(false);
                    return;
                }
            }
        }

        // USB 模式: 执行后设备软复位, HID 重新枚举, 重连后再轮询握手
        if (m_transportType == TransportType::Usb) {
#ifdef COMFORTOOL_ENABLE_USB
            m_transport->close();
            m_transport = nullptr;
            appendSystemLog("USB: 升级执行完成, 等待设备重启重新枚举...", QColor("#d08020"));
            bool found = false;
            for (int i = 0; i < 40 && !found; ++i) {  // 最多20s
                QThread::msleep(500);
                auto devs = HidManager::enumerate(0x5377, 0x5378);
                if (!devs.isEmpty()) {
                    QVariantMap params;
                    params["path"] = devs.first().path;
                    if (m_hidManager->open(params)) {
                        m_transport = m_hidManager;
                        found = true;
                        QThread::msleep(800);  // 端点刚就绪稍等, 避免 setReport NotResponding
                    }
                }
            }
            if (!found) {
                appendSystemLog("USB: 重启后重新枚举超时", QColor("#c050a0"));
                endUpgrade(false);
                return;
            }
#endif
        }

        // 轮询等待设备重启完成，最多30秒.
        // 设备软复位后 USB 重新枚举, 重连循环拿到的可能是半就绪 handle(EP1 IN 未稳定/USB Address 变).
        // 故握手无响应时每隔几秒重新 close+enumerate+open 拿新 handle, 设备完全启动后才能响应.
        appendSystemLog("等待设备重启(最多30s)...");
        // 等待期间显示遮罩, 防止用户误操作主窗口
        m_waitDialog = new QProgressDialog(tr("设备正在重启, 等待重新连接..."), QString(), 0, 0, this);
        m_waitDialog->setWindowModality(Qt::ApplicationModal);
        m_waitDialog->setWindowFlags(m_waitDialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
        m_waitDialog->setCancelButton(nullptr);
        m_waitDialog->setMinimumDuration(0);
        m_waitDialog->show();
        QCoreApplication::processEvents();
        bool bootCheckDone = false;
        int noRespCount = 0;
        for (int waitSec = 0; waitSec < 30 && !bootCheckDone; ++waitSec) {
            QThread::sleep(1);
            quint8 devAddr2 = 0, status2 = 0;
            quint32 dummyUid2 = 0;
            int finalLayer = waitBootHandshake(this, m_transport, devAddr2, status2, dummyUid2);
            if (finalLayer == 1) {
                appendSystemLog("升级成功! 设备已进入APP", QColor("#50a050"));
                upgradeSuccess = true;
                bootCheckDone = true;
            } else if (finalLayer == 0) {
                // 设备还在BOOT，可能需要更多时间跳转，继续等待
                if (waitSec >= 9) {
                    // 已等待10秒仍在BOOT，判定为失败
                    appendSystemLog("升级执行后设备仍停留在Boot(等待10s)，固件可能不兼容，请检查固件文件", QColor("#c050a0"));
                    endUpgrade(false);
                    return;
                }
            } else {
                // 握手无响应(设备正在重启/旧handle失效): 每3次无响应重新拿新handle
                if (++noRespCount >= 3) {
                    noRespCount = 0;
#ifdef COMFORTOOL_ENABLE_USB
                    if (m_transportType == TransportType::Usb) {
                        appendSystemLog("USB: 旧handle无响应, 重新枚举连接...", QColor("#d08020"));
                        if (m_transport) { m_transport->close(); m_transport = nullptr; }
                        for (int i = 0; i < 4; ++i) {
                            QThread::msleep(400);
                            auto devs = HidManager::enumerate(0x5377, 0x5378);
                            if (!devs.isEmpty()) {
                                QVariantMap params;
                                params["path"] = devs.first().path;
                                if (m_hidManager->open(params)) { m_transport = m_hidManager; break; }
                            }
                        }
                    }
#endif
                }
            }
        }
        if (!bootCheckDone) {
            appendSystemLog("重启后等待超时(30s)，设备未回应", QColor("#c050a0"));
            endUpgrade(false);
            return;
        }
    }

    if (!upgradeSuccess) {
        appendSystemLog(QString("固件更新失败(已重试%1次)").arg(MAX_RETRIES), QColor("#c050a0"));
        endUpgrade(false);
        return;
    }

    appendSystemLog("固件更新成功!", QColor("#50a050"));
    m_logProgressBar->setValue(totalFrames + 2);
    appendSystemLog("[升级] 固件更新完成, 自动触发握手以解锁 FUN 区", QColor("#50a050"));
    endUpgrade(true);
    // 自动握手: 验证设备在 APP 层并解锁 FUN 区
    QTimer::singleShot(500, this, &MainWindow::onFuncHandshake);
}

void MainWindow::flashControls(bool disable)
{
    bool open = m_transport && m_transport->isOpen();
    m_openBtn->setEnabled(!disable);
    m_ifaceCombo->setEnabled(false);
    // m_refreshBtn removed
    m_funcHandshakeBtn->setEnabled(!disable && open);
    m_verAddrEdit->setEnabled(!disable && open);
    m_updateBtn->setEnabled(!disable && m_funcUnlocked);
    m_debugBtn->setEnabled(!disable && m_funcUnlocked);
    m_sccdBtn->setEnabled(!disable && m_funcUnlocked);
    m_protoEnterBootBtn->setEnabled(!disable && m_funcUnlocked);
    m_protoResetBtn->setEnabled(!disable && m_funcUnlocked);
    if (m_rfOpenBtn)  m_rfOpenBtn->setEnabled(!disable && m_funcUnlocked);
    if (m_rfCloseBtn) m_rfCloseBtn->setEnabled(!disable && m_funcUnlocked);
    if (m_rfResetBtn) m_rfResetBtn->setEnabled(!disable && m_funcUnlocked);
    // 升级控件已整合进 LOG 区(进度条) + 左下日志(状态/校验描述), 不再需要独立控件行
}

void MainWindow::endUpgrade(bool success)
{
    m_upgrading = false;  // 升级流程结束: 恢复 close 即清日志的默认行为
    // 关闭升级等待遮罩(若仍显示)
    if (m_waitDialog) { m_waitDialog->close(); m_waitDialog = nullptr; }
    if (success) {
        if (m_logProgressBar->maximum() > 0)
            m_logProgressBar->setValue(m_logProgressBar->maximum());
        QTimer::singleShot(2000, this, [this]() {
            m_logProgressBar->setVisible(false);
            flashControls(false);
        });
    } else {
        m_logProgressBar->setVisible(false);
        flashControls(false);
    }
}

void MainWindow::onUpgradeTimeout()
{
    appendSystemLog("固件更新超时", QColor("#c050a0"));
    endUpgrade(false);
}

void MainWindow::onUpgradeResponse()
{
    // Handled inline in upgrade flow
}
