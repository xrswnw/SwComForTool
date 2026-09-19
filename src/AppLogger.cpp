#include "AppLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QSysInfo>
#include <QTime>

namespace {
QMutex g_mutex;   // HID 读线程与主线程并发写
QString g_file;   // 当前会话文件绝对路径 (空 = 未初始化)
const int kMaxSessionFiles = 20; // 会话文件保留上限
}

// 会话文件轮转: 目录内按名序(含时间戳即时间序)只保留最近 kMaxSessionFiles 个
void AppLogger::rotate()
{
    QDir d(QFileInfo(g_file).dir());
    const QStringList logs = d.entryList(QStringList() << "session_*.log",
                                         QDir::Files, QDir::Name);
    for (int i = 0; i + kMaxSessionFiles < logs.size(); ++i)
        d.remove(logs.at(i));
}

void AppLogger::init()
{
    QMutexLocker lock(&g_mutex);
    if (!g_file.isEmpty()) return; // 幂等

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    // 首选: exe 同目录 log/ (绿色部署场景可直接写; Program Files 等受限目录由 write() 回退)
    QDir dir(QCoreApplication::applicationDirPath() + "/log");
    if (dir.mkpath(".")) {
        g_file = dir.filePath("session_" + stamp + ".log");
        QFile probe(g_file);
        if (probe.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            probe.close();
        } else {
            g_file.clear();
        }
    }
    if (g_file.isEmpty()) {
        // 回退: 系统临时目录 (单文件追加, 跨会话共用)
        QDir t(QDir::tempPath() + "/SwComForTool_log");
        if (!t.mkpath(".")) return;
        g_file = t.filePath("session_fallback.log");
    }

    // 启动头: 环境信息一次写全, 分析时不依赖现场再问
    QFile f(g_file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        g_file.clear();
        return;
    }
    f.write(QString("\n==== SwComForTool 会话 %1 ====\n")
                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                .toUtf8());
    f.write(QString("版本:%1 Qt:%2 系统:%3 构建架构:%4 运行架构:%5\n")
                .arg(QCoreApplication::applicationVersion())
                .arg(QString::fromLatin1(qVersion()))
                .arg(QSysInfo::prettyProductName())
                .arg(QSysInfo::buildCpuArchitecture())
                .arg(QSysInfo::currentCpuArchitecture())
                .toUtf8());
    f.close();

    rotate();
}

void AppLogger::line(const QString &prefix, const QString &msg)
{
    write(QTime::currentTime().toString("HH:mm:ss.zzz") + " [" + prefix + "] " + msg);
}

void AppLogger::frame(const QString &prefix, const QByteArray &data, const QString &note)
{
    QString s = QTime::currentTime().toString("HH:mm:ss.zzz") + " [" + prefix + "] "
        + QString::number(data.size()) + "B: " + QString::fromLatin1(data.toHex(' ').toUpper());
    if (!note.isEmpty()) s += " (" + note + ")";
    write(s);
}

void AppLogger::write(const QString &text)
{
    QMutexLocker lock(&g_mutex);
    if (g_file.isEmpty()) return;
    QFile f(g_file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // exe 目录中途变只读等场景: 回退临时目录 (仅一次)
        QDir t(QDir::tempPath() + "/SwComForTool_log");
        if (!t.mkpath(".")) return;
        g_file = t.filePath("session_fallback.log");
        QFile f2(g_file);
        if (!f2.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
        f2.write(text.toUtf8());
        f2.write("\n");
        f2.close();
        return;
    }
    f.write(text.toUtf8());
    f.write("\n");
    f.close();
}

QString AppLogger::sessionFile()
{
    QMutexLocker lock(&g_mutex);
    return g_file;
}

QString AppLogger::winErrText(int err)
{
    switch (err) {
    case 6:   return QStringLiteral("句柄无效, 设备句柄已失效(多为设备重枚举/重插)");
    case 31:  return QStringLiteral("通用失败, 固件拒绝该传输方式(如控制传输 SET_REPORT)");
    case 23:  return QStringLiteral("数据错误, CRC 校验失败(线缆干扰/固件错误)");
    case 87:  return QStringLiteral("参数错误, 报文长度与设备描述不符");
    case 1117: return QStringLiteral("I/O 设备超时, 设备无响应");
    case 1167: return QStringLiteral("设备未连接, 设备已拔出或从总线掉线");
    case 121: return QStringLiteral("信号灯超时, 设备长时间无响应");
    case 997: return QStringLiteral("重叠 I/O 进行中(异步正常态, 报此多为等待逻辑缺陷)");
    default:
        return QStringLiteral("错误码%1, 详见 winerror.h").arg(err);
    }
}
