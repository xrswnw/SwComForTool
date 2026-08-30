# ComforTool

基于 Qt6 的串口通信桌面程序。

## 开发规则

- **禁止使用 `rm -f` 等不可恢复的删除命令**，所有删除文件移至 `TrashCan/` 目录
- **备份机制**：工程备份位于 `/Volumes/Untitled 1/Files/BackupArea/ComforTool`（如不可访问则使用 `/Users/swnw/Documents/BackupArea`），文件变更需同步备份并记录日志
- **中文为主**
- **文件路径优于文件名**：存在同名文件时以路径为准

## 技术栈

- Qt6 (Widgets + SerialPort)
- CMake 构建
- C++17

## 目录结构

- `src/` — 源码
- `TrashCan/` — 回收站（删除文件暂存）
- `Agent/` — 需求与报告
- `.claude/` — 配置
