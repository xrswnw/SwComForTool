#!/bin/bash
# ComforTool 工程级全量快照备份脚本
# 用法: ./backup.sh "本轮变更说明"
# 规则: 全量快照(非增量) / 32层轮转 / 单层<=500MB / 白名单范围 / TrashCan可恢复删除
set -u

PROJECT_ROOT="/Users/swnw/Documents/Software/SwComForTool"
# PRIMARY: 用户期望路径 (Record 卷下). 路径含空格, 命令中必须双引号包住.
PRIMARY_BACKUP="/Volumes/Record/Person Code/BackupArea/ComforTool"
# FALLBACK: 主盘本地备份, 不可访问 PRIMARY 时使用
FALLBACK_BACKUP="/Users/swnw/Documents/BackupArea/ComforTool"
MAX_ROUNDS=32
MAX_SIZE_MB=500
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

# 选择可访问的备份根
if [ -d "/Volumes/Record/Person Code/BackupArea" ]; then
    BACKUP_ROOT="$PRIMARY_BACKUP"
else
    BACKUP_ROOT="$FALLBACK_BACKUP"
fi

mkdir -p "$BACKUP_ROOT/Changelog"
CHANGELOG="$BACKUP_ROOT/Changelog/Changelog.log"
TRASHCAN="$PROJECT_ROOT/TrashCan"
mkdir -p "$TRASHCAN"

DESC="${1:-未填写变更说明}"

# ---- 已有扁平备份迁移: 若无任何 Round_* 则把现有内容移入 Round_001 ----
# 用 find 替代 ls glob, 规避 BACKUP_ROOT 含空格(如 /Volumes/Record/Person Code/...)时的 word splitting 风险
# 注意: macOS /bin/bash 是 3.2.57, 没有 mapfile, 用 while-read 累加数组
existing_rounds=()
while IFS= read -r d; do existing_rounds+=("$d"); done < <(find "$BACKUP_ROOT" -maxdepth 1 -mindepth 1 -type d -name 'Round_*' 2>/dev/null | sort)
if [ ${#existing_rounds[@]} -eq 0 ]; then
    # 检查备份根是否有非 Changelog 的内容 (用 find 处理含空格路径)
    flat_items=()
    while IFS= read -r f; do
        bn="$(basename "$f")"
        # macOS 大小写不敏感, 统一小写比较避免误移 Changelog 目录
        bn_lower="$(echo "$bn" | tr 'A-Z' 'a-z')"
        [ "$bn_lower" = "changelog" ] && continue
        flat_items+=("$f")
    done < <(find "$BACKUP_ROOT" -maxdepth 1 -mindepth 1 2>/dev/null)
    if [ ${#flat_items[@]} -gt 0 ]; then
        migrate_dir="$BACKUP_ROOT/Round_001_$TIMESTAMP"
        mkdir -p "$migrate_dir"
        for f in "${flat_items[@]}"; do
            mv "$f" "$migrate_dir/"
        done
        echo "已迁移扁平备份到 $migrate_dir"
        # 迁移后时间戳延续用于本轮新快照编号(新编号=002)
        TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
    fi
fi

# ---- 确定新编号 = 当前最大编号 +1 ----
max_num=0
for d in "$BACKUP_ROOT"/Round_*; do
    [ -d "$d" ] || continue
    bn="$(basename "$d")"
    num="$(echo "$bn" | sed -n 's/^Round_0*\([0-9]*\).*/\1/p')"
    [ -z "$num" ] && continue
    [ "$num" -gt "$max_num" ] && max_num="$num"
done
new_num=$((max_num + 1))
new_round=$(printf "Round_%03d_%s" "$new_num" "$TIMESTAMP")
ROUND_DIR="$BACKUP_ROOT/$new_round"
mkdir -p "$ROUND_DIR"

# ---- 白名单全量快照 (保证<=500MB) ----
# 白名单目录
WHITELIST_DIRS=(src res docs icon issue deploy Agent)
# 白名单根文件
WHITELIST_FILES=(CMakeLists.txt CLAUDE.md backup.sh build_win.bat log_change.sh watch_backup.sh)

copy_count=0
for d in "${WHITELIST_DIRS[@]}"; do
    src="$PROJECT_ROOT/$d"
    [ -d "$src" ] || continue
    rsync -a --exclude='.DS_Store' "$src/" "$ROUND_DIR/$d/"
    copy_count=$((copy_count + 1))
done
file_count=0
for f in "${WHITELIST_FILES[@]}"; do
    src="$PROJECT_ROOT/$f"
    [ -f "$src" ] || continue
    cp -p "$src" "$ROUND_DIR/$f"
    file_count=$((file_count + 1))
done

# 全局 CLAUDE.md 副本
if [ -f "/Users/swnw/Documents/Software/CLAUDE.md" ]; then
    cp -p "/Users/swnw/Documents/Software/CLAUDE.md" "$ROUND_DIR/_global_CLAUDE.md"
    file_count=$((file_count + 1))
fi

# ---- 大小校验 ----
size_bytes=$(du -sk "$ROUND_DIR" 2>/dev/null | awk '{print $1}')
size_mb=0
[ -n "$size_bytes" ] && size_mb=$((size_bytes / 1024))
size_status="$size_mb M (<=${MAX_SIZE_MB}M OK)"
rotate_action="无"
if [ "$size_mb" -gt "$MAX_SIZE_MB" ]; then
    size_status="$size_mb M (>${MAX_SIZE_MB}M 超限, 检查白名单)"
fi

# ---- 32层轮转: 超过则 mv 最旧 Round 到 TrashCan (可恢复) ----
# 用 find 替代 ls glob, 规避路径含空格的 word splitting 问题
# 注意: macOS /bin/bash 是 3.2.57, 没有 mapfile, 用 while-read 累加数组
all_rounds=()
while IFS= read -r d; do all_rounds+=("$d"); done < <(find "$BACKUP_ROOT" -maxdepth 1 -mindepth 1 -type d -name 'Round_*' 2>/dev/null | sort)
round_total=${#all_rounds[@]}
if [ "$round_total" -gt "$MAX_ROUNDS" ]; then
    oldest="${all_rounds[0]}"
    mv "$oldest" "$TRASHCAN/"
    rotate_action="删除(mv至TrashCan) $(basename "$oldest")"
    round_total=$((round_total - 1))
fi

# ---- 验证: 源-目的文件计数对比 ----
src_files=$(find "${WHITELIST_DIRS[@]/#/$PROJECT_ROOT/}" -type f ! -name '.DS_Store' 2>/dev/null | wc -l | tr -d ' ')
dst_files=$(find "$ROUND_DIR" -type f ! -name '.DS_Store' 2>/dev/null | wc -l | tr -d ' ')
# 加根文件
[ -n "$src_files" ] || src_files=0
src_total=$((src_files + file_count))

# ---- 写 Changelog ----
cat >> "$CHANGELOG" <<EOF
[$(date "+%Y-%m-%d %H:%M:%S")] Round $new_num
变更说明: $DESC
备份范围: 白名单目录(${WHITELIST_DIRS[*]}) + 根脚本 + 全局CLAUDE.md副本
快照路径: $ROUND_DIR
快照大小: $size_status
轮转动作: $rotate_action
验证: 复制目录=$copy_count 根文件=$file_count / 源文件(含根)=$src_total 目的文件=$dst_files
---
EOF

echo "备份完成: Round $new_num"
echo "  路径: $ROUND_DIR"
echo "  大小: $size_status"
echo "  轮转: $rotate_action"
echo "  Changelog: $CHANGELOG"
