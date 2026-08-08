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
    resize(960, 540);

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
    sccdOuterLayout->setContentsMargins(8, 6, 8, 6);
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

    mainLayout->addWidget(m_sccdBox);

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
    m_sccdBox->setVisible(checked);
    // 按钮底色: 关闭 = 默认紫; 展开 = 高亮绿
    m_sccdBtn->setProperty("active", checked);
    m_sccdBtn->style()->unpolish(m_sccdBtn);
    m_sccdBtn->style()->polish(m_sccdBtn);
    if (checked)
        statusBar()->showMessage(tr("Sccd Area 已展开"), 2000);
    else
        statusBar()->showMessage(tr("Sccd Area 已隐藏"), 2000);
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

    if (!waitForResponse(timeoutMs)) {
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
    m_protoEnterBootBtn->setEnabled(enable && m_funcUnlocked);
    m_protoResetBtn->setEnabled(enable && m_funcUnlocked);
    // 射频控制按钮: 握手成功后开放 (Sccd Area 内)
    m_rfOpenBtn->setEnabled(enable && m_funcUnlocked);
    m_rfCloseBtn->setEnabled(enable && m_funcUnlocked);
    m_rfResetBtn->setEnabled(enable && m_funcUnlocked);
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
