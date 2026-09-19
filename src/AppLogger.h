#pragma once
#include <QString>
#include <QByteArray>

// ============================ 全局操作日志 (Round_099) ============================
// 会话级自动落盘: 启动即建 exe 同目录 log/session_<时间戳>.log, 全程追加写.
// 记录: 启动环境 / 连接动作 / 每帧收发(hex) / 全部错误码, 供 PC 现场问题远程根因分析.
// 设计约束:
//   - 线程安全 (HID 读线程也直接写)
//   - exe 目录不可写时回退 系统临时目录/SwComForTool_log/ (单文件 session_fallback.log)
//   - 自动只保留最近 20 个会话文件, 防止无限膨胀
//   - 与界面"保存"按钮(onSaveLog → log/*.md)互不影响; 本机制无需任何手动操作
class AppLogger
{
public:
    // 会话开始: 创建文件 + 写启动头(版本/系统/Qt/架构). 进程内幂等, 越早调用越好.
    static void init();
    // 追加一行: "HH:mm:ss.zzz [prefix] msg"
    static void line(const QString &prefix, const QString &msg);
    // 帧日志: "HH:mm:ss.zzz [prefix] nB: AA BB CC ... (note)" — 恒为 hex, 与界面显示模式无关
    static void frame(const QString &prefix, const QByteArray &data, const QString &note = QString());
    // 常见 Windows 错误码转可读描述 (未知码返回通用文案), 便于现场直接看懂
    static QString winErrText(int err);
    // 当前会话文件绝对路径 (空 = 未初始化或不可写)
    static QString sessionFile();

private:
    static void write(const QString &text); // 内部: 加锁追加一行 (带回退逻辑)
    static void rotate();                   // 保留最近 20 个 session_*.log
};
