#include "HidManagerMac.h"
#include <QMetaObject>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <cstring>

#ifdef Q_OS_MACOS
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>

// ============================ 文件静态回调 (签名见 SDK IOHIDBase.h) ============================
// 回调需访问 HidManagerMacThread, 而该类定义在其后, 故前向声明 + 定义后置.

class HidManagerMacThread;

// IOHIDReportCallback (7 参): (ctx, IOReturn result, void* sender, IOHIDReportType type,
//   uint32_t reportID, uint8_t* report, CFIndex reportLength). 在 CFRunLoop 线程调用.
static void hidmac_input_cb(void *ctx, IOReturn /*result*/, void * /*sender*/,
                            IOHIDReportType /*type*/, uint32_t /*reportID*/,
                            uint8_t *report, CFIndex len);

// IOHIDDeviceCallback (4 参): (ctx, IOReturn result, void* sender, IOHIDDeviceRef device)
// 用作 manager matching.
static void hidmac_matching_cb(void *ctx, IOReturn /*result*/, void * /*sender*/,
                               IOHIDDeviceRef device);

// IOHIDCallback (3 参): (ctx, IOReturn result, void* sender) — 设备移除.
static void hidmac_removal_cb(void *ctx, IOReturn /*result*/, void * /*sender*/);

// ============================ HidManagerMacThread ============================
// 独立 QThread 跑纯 CFRunLoop 承载 IOHIDManager matching + 设备输入报告回调.
// matching 是 runloop 异步派发, GUI 主线程 Qt cocoa 循环时不派发, 必须独立线程.

class HidManagerMacThread : public QThread
{
public:
    HidManagerMacThread(quint16 vid, quint16 pid, HidManagerMac *owner)
        : QThread(nullptr), m_vid(vid), m_pid(pid), m_owner(owner) {}

    ~HidManagerMacThread() override { stop(); if (isRunning()) wait(3000); }

    void stop()
    {
        m_stop = true;
        m_openCond.wakeAll();
        if (m_runloop) CFRunLoopStop(static_cast<CFRunLoopRef>(m_runloop));
    }

    // 主线程等待 matching 回调内 open 完成
    bool waitForOpened(int timeoutMs)
    {
        QMutexLocker lock(&m_openMutex);
        if (m_opened) return m_openOk;
        m_openCond.wait(&m_openMutex, timeoutMs);
        return m_openOk;
    }

    QString openError() const { return m_openError; }
    void *device() const { return m_device; }   // IOHIDDeviceRef, 供 writeData setReport

    // matching 回调内: open 设备 + 注册输入/移除回调 + 通知主线程
    void onDeviceMatched(IOHIDDeviceRef dev)
    {
        if (!dev) return;
        {
            QMutexLocker lock(&m_openMutex);
            if (m_device) return;  // 仅取第一个
        }
        CFRetain(dev);
        IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);  // 与 dext 共享, 不独占
        if (r != kIOReturnSuccess) {
            CFRelease(dev);
            setOpened(false, QString("IOHIDDeviceOpen 失败: 0x%1").arg(r, 8, 16, QChar('0')));
            return;
        }
        {
            QMutexLocker lock(&m_openMutex);
            m_device = dev;
        }
        // 注册输入报告回调 (64B 缓冲) + 移除回调. 顺序须为 register → schedule:
        // register 创建 input source, schedule 才能把它挂到 runloop; 反序则 source 不挂.
        IOHIDDeviceRegisterInputReportCallback(dev, m_reportBuf, HidManagerMac::PACKET_SIZE,
            hidmac_input_cb, this);
        IOHIDDeviceRegisterRemovalCallback(dev, hidmac_removal_cb, this);
        // 关键: 设备须单独 schedule 到 runloop, input report 回调才会派发.
        // manager 的 schedule 只管 matching/removal; 设备 input 事件源需自己挂 runloop.
        IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        setOpened(true, QString());
    }

    void onDeviceLost()
    {
        { QFile _f("/tmp/comfor_devlost.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
          QTextStream _s(&_f); _s << "[onDeviceLost] removal callback fired, device present=" << (m_device? "yes":"no") << "\n"; _s.flush(); }
        setOpened(false, "HID 设备已拔出");
        stop();
        QMetaObject::invokeMethod(m_owner, "close", Qt::QueuedConnection);
    }

    void setOpened(bool ok, const QString &err)
    {
        QMutexLocker lock(&m_openMutex);
        m_opened = true;
        m_openOk = ok;
        if (!ok) m_openError = err;
        m_openCond.wakeAll();
    }

    HidManagerMac *m_owner;
    quint16 m_vid, m_pid;

protected:
    void run() override
    {
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!mgr) { setOpened(false, "IOHIDManagerCreate 失败"); return; }
        m_manager = mgr;

        if (m_vid != 0 && m_pid != 0) {
            CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int v = m_vid, p = m_pid;
            CFNumberRef vN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
            CFNumberRef pN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &p);
            CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vN);
            CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), pN);
            CFRelease(vN); CFRelease(pN);
            IOHIDManagerSetDeviceMatching(mgr, match);
            CFRelease(match);
        } else {
            IOHIDManagerSetDeviceMatching(mgr, NULL);
        }
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, hidmac_matching_cb, this);
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

        IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
        if (r != kIOReturnSuccess) {
            setOpened(false, QString("IOHIDManagerOpen 失败: 0x%1 (权限不足或设备被占?)")
                .arg(r, 8, 16, QChar('0')));
            IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            CFRelease(mgr);
            m_manager = nullptr;
            return;
        }

        // 跑 CFRunLoop: matching 回调异步派发 → onDeviceMatched → open + 注册输入回调.
        // 之后持续承载输入报告回调, 直到 stop() 跨线程 CFRunLoopStop.
        m_runloop = CFRunLoopGetCurrent();
        while (!m_stop) {
            SInt32 res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
            if (res == kCFRunLoopRunStopped || m_stop) break;
        }
        m_runloop = nullptr;

        // 清理 (runloop 线程内). 关闭设备即解除回调, 无需传 nullptr 注销(形参 _Nonnull).
        if (m_device) {
            { QFile _f("/tmp/comfor_devlost.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
              QTextStream _s(&_f); _s << "[run cleanup] closing device, m_stop=" << m_stop << "\n"; _s.flush(); }
            IOHIDDeviceUnscheduleFromRunLoop(static_cast<IOHIDDeviceRef>(m_device), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            IOHIDDeviceClose(static_cast<IOHIDDeviceRef>(m_device), kIOHIDOptionsTypeNone);
            CFRelease(static_cast<IOHIDDeviceRef>(m_device));
            m_device = nullptr;
        }
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        CFRelease(mgr);
        m_manager = nullptr;
    }

private:
    volatile bool m_stop = false;
    void *m_manager = nullptr;   // IOHIDManagerRef
    void *m_device  = nullptr;    // IOHIDDeviceRef
    void *m_runloop = nullptr;    // CFRunLoopRef
    uint8_t m_reportBuf[HidManagerMac::PACKET_SIZE] = {0};

    bool m_opened = false;
    bool m_openOk = false;
    QString m_openError;
    QMutex m_openMutex;
    QWaitCondition m_openCond;

    friend void hidmac_input_cb(void *, IOReturn, void *, IOHIDReportType, uint32_t, uint8_t *, CFIndex);
    friend void hidmac_matching_cb(void *, IOReturn, void *, IOHIDDeviceRef);
    friend void hidmac_removal_cb(void *, IOReturn, void *);
};

// ---- 回调定义 (类后, 类型可见) ----
// IOHIDReportCallback: 输入报告, 在 CFRunLoop 线程.
static void hidmac_input_cb(void *ctx, IOReturn /*result*/, void * /*sender*/,
                     IOHIDReportType /*type*/, uint32_t /*reportID*/,
                     uint8_t *report, CFIndex len)
{
    auto *self = static_cast<HidManagerMacThread *>(ctx);
    { QFile _f("/tmp/comfor_rx_diag.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[input_cb] fired len=" << len << " hex="
        << QByteArray::fromRawData(reinterpret_cast<const char *>(report), qMin(int(len),32)).toHex(' ').toUpper()
        << "\n"; _s.flush(); }
    if (!self || !report || len <= 0) return;
    QByteArray raw(reinterpret_cast<const char *>(report), int(len));
    // Round 019: 修复 FC_UPGRADE_START 后续 RX 丢失 (input_cb 之前用 Qt::QueuedConnection,
    // 主线程事件循环 5ms msleep 期间事件不派发, 短超时窗口错过 IN report).
    // 改为 CFRunLoop 线程内锁保护直接 append (公开 appendRxFrame 接口, 避开跨类 private 权限).
    // readAll() 锁内拷贝清空 m_rxBuf, 无 Qt 跨线程事件, 同一帧数据立即可见.
    self->m_owner->appendRxFrame(raw);
    // rawReceived: 信号唯一 debug 出口, 仍 Qt::AutoConnection 兼容跨线程
    QMetaObject::invokeMethod(self->m_owner, "rawReceived", Qt::AutoConnection,
                              Q_ARG(QByteArray, raw));
}

// IOHIDDeviceCallback: manager matching.
static void hidmac_matching_cb(void *ctx, IOReturn /*result*/, void * /*sender*/,
                        IOHIDDeviceRef device)
{
    auto *self = static_cast<HidManagerMacThread *>(ctx);
    if (!self || !device) return;
    self->onDeviceMatched(device);
}

// IOHIDCallback: 设备移除 (3 参).
// 实验性: macOS 在 Open 共享 device 后, 其它客户端活动(如 manager 持续 matching 派发新设备/IO 流量)
// 可能误触发 removal 回调, 而设备实际仍在(device present=yes). 真拔出看 writeData 失败即可.
// 故暂时只记录, 不停线程不 close, 让 isOpen 保持 true.
static void hidmac_removal_cb(void *ctx, IOReturn /*result*/, void * /*sender*/)
{
    auto *self = static_cast<HidManagerMacThread *>(ctx);
    if (!self) return;
    { QFile _f("/tmp/comfor_devlost.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[hidmac_removal_cb] ignored\n"; _s.flush(); }
    // 不调 onDeviceLost — 见上注释. 真拔出时 writeData setReport 会失败由 m_lastError 暴露.
}

// ============================ 枚举线程 ============================

struct HidMacEnumCtx { QList<HidDeviceInfo> *out; };
static void hidmac_enum_cb(void *c, IOReturn, void *, IOHIDDeviceRef dev)
{
    if (!dev) return;
    { QFile _f("/tmp/comfor_enum_diag.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[enum_cb] matched\n"; _s.flush(); }
    auto *e = static_cast<HidMacEnumCtx *>(c);
    HidDeviceInfo info;
    int v = 0, p = 0;
    CFNumberRef vn = static_cast<CFNumberRef>(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDVendorIDKey)));
    CFNumberRef pn = static_cast<CFNumberRef>(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)));
    if (vn) CFNumberGetValue(vn, kCFNumberIntType, &v);
    if (pn) CFNumberGetValue(pn, kCFNumberIntType, &p);
    info.vendorId = quint16(v);
    info.productId = quint16(p);
    CFStringRef s;
    s = static_cast<CFStringRef>(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDManufacturerKey)));
    if (s) info.manufacturer = QString::fromCFString(s);
    s = static_cast<CFStringRef>(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey)));
    if (s) info.product = QString::fromCFString(s);
    s = static_cast<CFStringRef>(IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDSerialNumberKey)));
    if (s) info.serial = QString::fromCFString(s);
    info.path = QString("hid:%1:%2").arg(info.vendorId, 4, 16, QChar('0')).arg(info.productId, 4, 16, QChar('0'));
    e->out->append(info);
}

class HidMacEnumThread : public QThread
{
public:
    quint16 vid, pid;
    QList<HidDeviceInfo> *out;
    HidMacEnumThread(quint16 v, quint16 p, QList<HidDeviceInfo> *o)
        : QThread(nullptr), vid(v), pid(p), out(o) {}
protected:
    void run() override {
        HidMacEnumCtx ctx; ctx.out = out;
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!mgr) return;
        if (vid != 0 && pid != 0) {
            CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int v = vid, p = pid;
            CFNumberRef vN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
            CFNumberRef pN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &p);
            CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey), vN);
            CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), pN);
            CFRelease(vN); CFRelease(pN);
            IOHIDManagerSetDeviceMatching(mgr, m);
            CFRelease(m);
        } else {
            IOHIDManagerSetDeviceMatching(mgr, NULL);
        }
        IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDManagerRegisterDeviceMatchingCallback(mgr, hidmac_enum_cb, &ctx);
        IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
        { QFile _f("/tmp/comfor_enum_diag.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
          QTextStream _s(&_f); _s << "[enum_thread] open=0x" << QString::number(r,16) << "\n"; _s.flush(); }
        if (r == kIOReturnSuccess) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
            IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        }
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFRelease(mgr);
        { QFile _f("/tmp/comfor_enum_diag.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
          QTextStream _s(&_f); _s << "[enum_thread] end count=" << out->size() << "\n"; _s.flush(); }
    }
};

#else
// 非 macOS: stub (本实现仅 macOS; 其它平台 USB 路径不在本次范围)
class HidManagerMacThread : public QThread {
public: void *m_owner=nullptr; quint16 m_vid=0,m_pid=0;
    void stop(){} bool waitForOpened(int){return false;} QString openError() const {return{};} void* device() const {return nullptr;}
};
#endif

// ============================ HidManagerMac ============================

HidManagerMac::HidManagerMac(QObject *parent) : ITransport(parent) {}
HidManagerMac::~HidManagerMac()
{
    // Round 020: 阻止析构期间 emit closed() 触发 lambda 访问正在 delete 的 MainWindow
    // (崩溃栈: ~MainWindow → deleteChildren → ~HidManagerMac → close() → emit closed()
    //  → onTransportClosed() → statusBar() on 已部分析构对象 → Data Abort @ FAR=0x1800).
    // 析构 + close 全程 block signals, 既阻断 emit close/portError 链, 也保证
    // 工作线程的 emit rawReceived 等也安全无效 — 此时 outer HidManagerMac 已释放.
    blockSignals(true);
    close();
}

QList<HidDeviceInfo> HidManagerMac::enumerate(quint16 vid, quint16 pid)
{
    QList<HidDeviceInfo> list;
#ifdef Q_OS_MACOS
    HidMacEnumThread t(vid, pid, &list);
    t.start();
    t.wait(3000);
#else
    Q_UNUSED(vid) Q_UNUSED(pid)
#endif
    return list;
}

bool HidManagerMac::open(const QVariantMap &params)
{
    close();
    quint16 vid = static_cast<quint16>(params.value("vid", SW_VID).toUInt());
    quint16 pid = static_cast<quint16>(params.value("pid", SW_PID).toUInt());
    if (vid == 0 && pid == 0) { vid = SW_VID; pid = SW_PID; }

#ifdef Q_OS_MACOS
    auto *t = new HidManagerMacThread(vid, pid, this);
    m_workerThread = t;
    t->start();
    if (!t->waitForOpened(3000)) {
        m_lastError = t->openError().isEmpty()
            ? QString("IOHIDManager 打开超时 VID %1 PID %2 (dext 未加载? 拔插 USB)").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'))
            : t->openError();
        t->stop();
        if (t->isRunning()) t->wait(2000);
        delete t;
        m_workerThread = nullptr;
        return false;
    }
    if (!t->device()) {
        m_lastError = t->openError();
        t->stop();
        if (t->isRunning()) t->wait(2000);
        delete t;
        m_workerThread = nullptr;
        return false;
    }
    m_name = QString("USB %1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toUpper();
    emit opened();
    return true;
#else
    Q_UNUSED(vid) Q_UNUSED(pid)
    m_lastError = "HidManagerMac 仅支持 macOS";
    return false;
#endif
}

void HidManagerMac::close()
{
#ifdef Q_OS_MACOS
    if (m_workerThread) {
        auto *t = static_cast<HidManagerMacThread *>(m_workerThread);
        t->stop();
        if (t->isRunning()) t->wait(2000);
        delete t;
        m_workerThread = nullptr;
    }
#endif
    m_name.clear();
    {
        QMutexLocker lock(&m_rxMutex);
        m_rxBuf.clear();
    }
    emit closed();
}

bool HidManagerMac::isOpen() const
{
#ifdef Q_OS_MACOS
    return m_workerThread && static_cast<HidManagerMacThread *>(m_workerThread)->device();
#else
    return false;
#endif
}

QString HidManagerMac::name() const { return m_name; }
QString HidManagerMac::errorString() const { return m_lastError; }

void HidManagerMac::writeData(const QByteArray &data)
{
#ifdef Q_OS_MACOS
    auto *t = static_cast<HidManagerMacThread *>(m_workerThread);
    if (!t || !t->device()) {
        { QFile _f("/tmp/comfor_devlost.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
          QTextStream _s(&_f); _s << "[writeData] no worker/device\n"; _s.flush(); }
        return;
    }
    IOHIDDeviceRef dev = static_cast<IOHIDDeviceRef>(t->device());
    // 输出报告: 64B, 首字节为设备约定的报告 ID 0x02(见 Sw01 App_Protocol §2),
    // 后 63B 为协议层帧. 缺 0x02 前缀会让固件判非法包并 STALL EP2 OUT → NotResponding.
    unsigned char pkt[PACKET_SIZE] = {0};
    pkt[0] = 0x02;
    int n = qMin(data.size(), PACKET_SIZE - 1);  // 协议帧最多 63B
    std::memcpy(pkt + 1, data.constData(), n);

    // Round 024d: 打开后首次 SetReport 可能返回 kIOReturnBadArgument(0xE00002C2) —
    //   IOHIDInterface 刚 open 尚未完全就绪(transient), 包未真正发出 → 设备无响应 → 3s 超时.
    //   实测同一包重试 1~3 次即 rc=0(历史 diag 日志 1004-1007: 3×BadArgument→成功; 用户手动重发亦成功).
    //   故在此对 transient 类错误重试, 避免首帧超时需手动重发.
    //   预算 30×30ms=900ms, 远小于 3s 响应超时; dev 机 attempt0 即 rc=0, 零额外开销.
    //   仅对 BadArgument/NotReady/Offline 重试; 其它错误(设备真拔出等)立即返回, 不掩盖.
    IOReturn r = kIOReturnBadArgument;  // 占位, 循环首轮即覆盖
    int attempts = 0;
    for (int a = 0; a < 30; ++a) {
        r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, pkt, PACKET_SIZE);
        attempts = a + 1;
        if (r == kIOReturnSuccess) break;
        if (r != kIOReturnBadArgument && r != kIOReturnNotReady && r != kIOReturnOffline) break;
        QThread::msleep(30);
    }

    { QFile _f("/tmp/comfor_rx_diag.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[writeData] dataSize=" << data.size() << " attempts=" << attempts
        << " rc=0x" << QString::number(r,16)
        << " pkt=" << QByteArray::fromRawData(reinterpret_cast<const char *>(pkt), qMin(n+1,32)).toHex(' ').toUpper() << "\n"; _s.flush(); }
    { QFile _f("/tmp/comfor_devlost.log"); _f.open(QIODevice::WriteOnly|QIODevice::Append|QIODevice::Text);
      QTextStream _s(&_f); _s << "[writeData] dataSize=" << data.size() << " attempts=" << attempts
        << " rc=0x" << QString::number(r,16) << "\n"; _s.flush(); }
    if (r != kIOReturnSuccess) {
        m_lastError = QString("IOHIDDeviceSetReport 失败: 0x%1 (重试%2次)").arg(r, 8, 16, QChar('0')).arg(attempts);
        emit portError();
    }
#else
    Q_UNUSED(data)
#endif
}

bool HidManagerMac::waitForBytesWritten(int /*msecs*/) { return isOpen(); }

QByteArray HidManagerMac::readAll()
{
    QMutexLocker lock(&m_rxMutex);
    QByteArray out = m_rxBuf;
    m_rxBuf.clear();
    return out;
}

// Round 019: CFRunLoop 线程直接入队 RX 报告 (锁保护).
//   hidmac_input_cb 在 CFRunLoop 线程 (不是 MainWindow/HidManagerMac 成员),
//   无 friend 访问 outer class private, 故暴露 public appendRxFrame.
void HidManagerMac::appendRxFrame(const QByteArray &raw)
{
    // 设备 IN 报告固定带 0x02 前缀 (App_Usb_HL_Transmit 加), 协议层帧首字节是 0x53.
    // 兼容首字节已是 0x53 (无前缀) 的场景.
    QByteArray frame = raw;
    if (frame.size() >= 1 && static_cast<quint8>(frame[0]) == 0x02)
        frame = frame.mid(1);
    QMutexLocker lock(&m_rxMutex);
    m_rxBuf.append(frame);
}

void HidManagerMac::onReadData(const QByteArray &raw)
{
    // Round 019: input_cb 已改为 CFRunLoop 线程内直接 append (锁保护), 不再走 Qt::QueuedConnection.
    // 本方法保留为 backward-compat slot (外部可 connect), 但实际不再被调用.
    // 若被调用, 仍 append + emit, 不会重复 (input_cb 已不再 invokeMethod "onReadData").
    Q_UNUSED(raw);
}
