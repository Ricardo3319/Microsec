#!/bin/bash
# collect_logs_simple.sh - 从所有远程节点收集日志 (实验结束后使用)
# 最小化开销，只在实验结束后执行一次

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_ROOT/logs"
RESULTS_DIR="$PROJECT_ROOT/results"

# 可选参数：实验名称
EXP_NAME="${1:-}"

# 节点配置
declare -A NODES=(
    ["node0"]="10.10.1.1"
    ["node1"]="10.10.1.2"
    ["node2"]="10.10.1.3"
    ["node3"]="10.10.1.4"
    ["node4"]="10.10.1.5"
    ["node5"]="10.10.1.6"
    ["node6"]="10.10.1.7"
    ["node7"]="10.10.1.8"
)

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

# 确定日志保存位置
if [ -n "$EXP_NAME" ]; then
    TARGET_LOG_DIR="$RESULTS_DIR/$EXP_NAME/logs"
else
    TARGET_LOG_DIR="$LOG_DIR"
fi

mkdir -p "$TARGET_LOG_DIR"

log "Collecting logs from remote nodes → $TARGET_LOG_DIR"

# 从所有节点并行收集日志 (使用超时防止卡住)
for node in "${!NODES[@]}"; do
    ip="${NODES[$node]}"
    (
        timeout 10 scp -r -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
            "$ip:$LOG_DIR/"*.log "$TARGET_LOG_DIR/" 2>/dev/null || \
        log "  ⚠ $node: timeout or no logs"
    ) &
done

# 等待所有传输完成
wait

log "✓ Logs collected at: $TARGET_LOG_DIR"
[ -d "$TARGET_LOG_DIR" ] && ls -1 "$TARGET_LOG_DIR"/*.log 2>/dev/null | wc -l | xargs -I {} echo "  ({} log files)"
