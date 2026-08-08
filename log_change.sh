#!/bin/bash
# 变更日志记录脚本
# 用法: ./log_change.sh <变更类型> <文件路径> <变更描述>
# 变更类型: CREATE / MODIFY / DELETE / MOVE

BACKUP_ROOT="/Volumes/Untitled 1/Files/BackupArea/ComforTool"
PROJECT_ROOT="/Users/swnw/Documents/Software/ComforTool"
CHANGELOG_DIR="$BACKUP_ROOT/ChangeLog"
LOG_FILE="$CHANGELOG_DIR/changelog_$(date +%Y%m).log"

# 检查备份目录可访问性
if [ ! -d "$BACKUP_ROOT" ]; then
    BACKUP_ROOT="/Users/swnw/Documents/BackupArea/ComforTool"
    CHANGELOG_DIR="$BACKUP_ROOT/ChangeLog"
    LOG_FILE="$CHANGELOG_DIR/changelog_$(date +%Y%m).log"
    mkdir -p "$CHANGELOG_DIR"
fi

CHANGE_TYPE="$1"
FILE_PATH="$2"
DESCRIPTION="$3"
TIMESTAMP=$(date "+%Y-%m-%d %H:%M:%S")

# 记录日志
echo "[$TIMESTAMP] [$CHANGE_TYPE] $FILE_PATH | $DESCRIPTION" >> "$LOG_FILE"

# 同步备份（排除 TrashCan、build、.DS_Store）
rsync -a --exclude='.DS_Store' --exclude='TrashCan' --exclude='build' --exclude='.claude' "$PROJECT_ROOT/" "$BACKUP_ROOT/" 2>/dev/null
