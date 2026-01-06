#!/bin/bash
# collect_logs.sh - 从所有远程节点收集实验日志
#
# 用法: 
#   ./collect_logs.sh                    # 收集所有日志
#   ./collect_logs.sh --continuous       # 连续监控模式（每 10 秒收集一次）
#   ./collect_logs.sh --monitor=5        # 自定义监控间隔（5 秒）

set -euo pipefail

# ======================== 配置 ========================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_ROOT/logs"

# 节点 IP 配置
declare -A NODES=(
    ["node0"]="10.10.1.1"   # Client 0
    ["node1"]="10.10.1.2"   # Client 1
    ["node2"]="10.10.1.3"   # Load Balancer
    ["node3"]="10.10.1.4"   # Worker 0 (Fast)
    ["node4"]="10.10.1.5"   # Worker 1 (Fast)
    ["node5"]="10.10.1.6"   # Worker 2 (Slow)
    ["node6"]="10.10.1.7"   # Worker 3 (Slow)
    ["node7"]="10.10.1.8"   # Worker 4 (Slow)
)

declare -A LOG_FILES=(
    ["node0"]="client_0.log"
    ["node1"]="client_1.log"
    ["node2"]="lb.log"
    ["node3"]="worker_0.log"
    ["node4"]="worker_1.log"
    ["node5"]="worker_2.log"
    ["node6"]="worker_3.log"
    ["node7"]="worker_4.log"
)

CONTINUOUS_MODE=false
MONITOR_INTERVAL=10

# ======================== 解析参数 ========================
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --continuous)
                CONTINUOUS_MODE=true
                shift
                ;;
            --monitor=*)
                MONITOR_INTERVAL="${1#*=}"
                CONTINUOUS_MODE=true
                shift
                ;;
            *)
                echo "Unknown option: $1"
                echo "Usage: $0 [--continuous] [--monitor=SECONDS]"
                exit 1
                ;;
        esac
    done
}

# ======================== 工具函数 ========================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

log_error() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: $1" >&2
}

# 获取当前节点 IP
get_current_ip() {
    if [ -z "${CURRENT_IP:-}" ]; then
        CURRENT_IP=$(hostname -I | tr ' ' '\n' | grep "^10\.10\." | head -1)
        if [ -z "$CURRENT_IP" ]; then
            CURRENT_IP=$(hostname -I | awk '{print $1}')
        fi
    fi
    echo "$CURRENT_IP"
}

# 从单个节点收集日志
collect_node_log() {
    local node=$1
    local ip="${NODES[$node]}"
    local log_file="${LOG_FILES[$node]}"
    local local_path="$LOG_DIR/$log_file"
    local remote_path="/users/Mingyang/microSec/logs/$log_file"
    local current_ip=$(get_current_ip)
    
    # 确保本地日志目录存在
    mkdir -p "$LOG_DIR"
    
    if [ "$ip" == "$current_ip" ]; then
        # 本地节点 - 直接复制
        if [ -f "/users/Mingyang/microSec/logs/$log_file" ]; then
            cp "/users/Mingyang/microSec/logs/$log_file" "$local_path" 2>/dev/null || true
            echo "✓"
        else
            echo "⊘"
        fi
    else
        # 远程节点 - 通过 scp 复制
        if scp -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
            "$ip:$remote_path" "$local_path" 2>/dev/null; then
            echo "✓"
        else
            echo "✗"
        fi
    fi
}

# 单次收集所有日志
collect_all_logs() {
    log "Collecting logs from all nodes..."
    
    local collected=0
    local failed=0
    
    for node in "${!NODES[@]}"; do
        local ip="${NODES[$node]}"
        local log_file="${LOG_FILES[$node]}"
        printf "  %-12s (%-12s) %s ... " "$node" "$ip" "$log_file"
        
        result=$(collect_node_log "$node")
        if [ "$result" = "✓" ]; then
            echo "OK"
            ((collected++))
        elif [ "$result" = "⊘" ]; then
            echo "SKIP (file not found)"
        else
            echo "FAIL"
            ((failed++))
        fi
    done
    
    log "Collection summary: $collected OK, $failed FAILED"
}

# 计算日志文件的最新修改时间
get_log_mtime() {
    local log_file="$1"
    if [ -f "$log_file" ]; then
        stat -f%m "$log_file" 2>/dev/null || stat -c%Y "$log_file" 2>/dev/null || echo 0
    else
        echo 0
    fi
}

# 监控日志变化
monitor_logs() {
    log "Starting continuous log monitoring (interval: ${MONITOR_INTERVAL}s)"
    log "Press Ctrl+C to stop"
    echo ""
    
    # 初始化 mtime 追踪
    declare -A prev_mtime
    declare -A curr_mtime
    
    local cycle=0
    while true; do
        ((cycle++))
        log "=== Collection cycle $cycle ==="
        
        # 收集所有日志
        for node in "${!NODES[@]}"; do
            local log_file="${LOG_FILES[$node]}"
            local local_path="$LOG_DIR/$log_file"
            
            collect_node_log "$node" > /dev/null 2>&1 || true
            
            # 检查日志文件是否有更新
            curr_mtime[$node]=$(get_log_mtime "$local_path")
            prev_mtime[$node]=${prev_mtime[$node]:-0}
            
            if [ "${curr_mtime[$node]}" != "${prev_mtime[$node]}" ]; then
                local lines=$(wc -l < "$local_path" 2>/dev/null || echo 0)
                log "  $node: UPDATED ($lines lines)"
            fi
            
            prev_mtime[$node]=${curr_mtime[$node]}
        done
        
        echo ""
        log "Waiting ${MONITOR_INTERVAL} seconds before next collection..."
        sleep "$MONITOR_INTERVAL"
    done
}

# ======================== 主函数 ========================

main() {
    parse_args "$@"
    
    # 验证日志目录
    if [ ! -d "$LOG_DIR" ]; then
        log "Creating log directory: $LOG_DIR"
        mkdir -p "$LOG_DIR"
    fi
    
    if [ "$CONTINUOUS_MODE" = true ]; then
        # 连续监控模式
        monitor_logs
    else
        # 单次收集模式
        collect_all_logs
        log "Logs collected to: $LOG_DIR"
    fi
}

main "$@"
