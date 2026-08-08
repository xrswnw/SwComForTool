#!/bin/bash
# 文件变更监控与自动备份同步脚本
# 使用fswatch监控工程目录，变更时自动同步备份并记录日志

BACKUP_ROOT="/Volumes/Untitled 1/Files/BackupArea/ComforTool"
PROJECT_ROOT="/Users/swnw/Documents/Software/ComforTool"
CHANGELOG_DIR="$BACKUP_ROOT/ChangeLog"

# 检查备份目录
if [ ! -d "$BACKUP_ROOT" ]; then
    BACKUP_ROOT="/Users/swnw/Documents/BackupArea/ComforTool"
    CHANGELOG_DIR="$BACKUP_ROOT/ChangeLog"
    mkdir -p "$CHANGELOG_DIR"
fi

LOG_FILE="$CHANGELOG_DIR/changelog_$(date +%Y%m).log"

echo "监控启动: $PROJECT_ROOT"
echo "备份目标: $BACKUP_ROOT"
echo "日志文件: $LOG_FILE"

fswatch -r --event Updated --event Created --event Removed --event Renamed \
    --exclude ".*\.DS_Store" \
    --exclude "build/" \
    --exclude ".venv/" \
    --exclude "TrashCan/" \
    --exclude "__pycache__/" \
    "$PROJECT_ROOT" | while read -r path action; do

    TIMESTAMP=$(date "+%Y-%m-%d %H:%M:%S")
    # 截取相对路径
    REL_PATH="${path#$PROJECT_ROOT/}"

    case "$action" in
        Updated)  TYPE="MODIFY" ;;
        Created)  TYPE="CREATE" ;;
        Removed)  TYPE="DELETE" ;;
        Renamed)  TYPE="MOVE" ;;
        *)        TYPE="EVENT" ;;
    esac

    echo "[$TIMESTAMP] [$TYPE] $REL_PATH" >> "$LOG_FILE"

    # 同步备份
    rsync -a --exclude='.DS_Store' --exclude='TrashCan' --exclude='build' --exclude='.venv' --exclude='.claude' "$PROJECT_ROOT/" "$BACKUP_ROOT/" 2>/dev/null
done
