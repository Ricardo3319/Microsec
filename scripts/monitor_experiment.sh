#!/bin/bash
# monitor_experiment.sh - 实时监控实验进行中的日志
#
# 用法:
#   ./monitor_experiment.sh                    # 每 10 秒收集一次日志
#   ./monitor_experiment.sh --interval=5       # 自定义间隔（5 秒）
#   ./monitor_experiment.sh --tail             # 同时显示日志尾部
#
# 这个脚本通常在后台运行，配合 orchestrate.sh 使用：
#   ./orchestrate.sh --exp=a &
#   ORCHESTRATE_PID=$!
#   ./monitor_experiment.sh --tail &
#   wait $ORCHESTRATE_PID
#   fg

set -euo pipefail

# ======================== 配置 ========================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_ROOT/logs"

# 节点 IP 配置
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

MONITOR_INTERVAL=10
SHOW_TAIL=false
TAIL_LINES=3

# ======================== 解析参数 ========================
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --interval=*)
                MONITOR_INTERVAL="${1#*=}"
                shift
                ;;
            --tail)
                SHOW_TAIL=true
                shift
                ;;
            --tail-lines=*)
                TAIL_LINES="${1#*=}"
                shift
                ;;
            *)
                echo "Unknown option: $1"
                echo "Usage: $0 [--interval=SECONDS] [--tail] [--tail-lines=N]"
                exit 1
                ;;
        esac
    done
}

# ======================== 工具函数 ========================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
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

# 收集单个节点日志
collect_node_log_quiet() {
    local node=$1
    local ip="${NODES[$node]}"
    local log_file="${LOG_FILES[$node]}"
    local local_path="$LOG_DIR/$log_file"
    local remote_path="/users/Mingyang/microSec/logs/$log_file"
    local current_ip=$(get_current_ip)
    
    mkdir -p "$LOG_DIR"
    
    if [ "$ip" == "$current_ip" ]; then
        if [ -f "/users/Mingyang/microSec/logs/$log_file" ]; then
            cp "/users/Mingyang/microSec/logs/$log_file" "$local_path" 2>/dev/null || true
        fi
    else
        scp -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
            "$ip:$remote_path" "$local_path" 2>/dev/null || true
    fi
}

# 显示实时进度统计
show_progress() {
    clear
    log "================================ 实验进行中 ================================"
    
    # 收集所有日志 (静默模式)
    for node in "${!NODES[@]}"; do
        collect_node_log_quiet "$node"
    done
    
    # 显示统计信息
    echo ""
    echo "📊 日志行数统计:"
    echo "─────────────────────────────────────────────────────"
    
    declare -A line_counts
    for node in "${!NODES[@]}"; do
        local log_file="${LOG_FILES[$node]}"
        local local_path="$LOG_DIR/$log_file"
        
        if [ -f "$local_path" ]; then
            local lines=$(wc -l < "$local_path" 2>/dev/null || echo 0)
            line_counts[$node]=$lines
            printf "  %-12s: %6d 行\n" "$node" "$lines"
        else
            printf "  %-12s: %s\n" "$node" "未启动"
        fi
    done
    
    # 显示日志尾部 (如果启用)
    if [ "$SHOW_TAIL" = true ]; then
        echo ""
        echo "📝 最新日志（尾部 $TAIL_LINES 行）:"
        echo "─────────────────────────────────────────────────────"
        for node in "${!NODES[@]}"; do
            local log_file="${LOG_FILES[$node]}"
            local local_path="$LOG_DIR/$log_file"
            
            if [ -f "$local_path" ] && [ -s "$local_path" ]; then
                echo ""
                echo "  [$node - $log_file]"
                tail -$TAIL_LINES "$local_path" | sed 's/^/    /'
            fi
        done
    fi
    
    echo ""
    echo "─────────────────────────────────────────────────────"
    echo "按 Ctrl+C 停止监控"
    echo ""
}

# ======================== 主函数 ========================

main() {
    parse_args "$@"
    
    if [ ! -d "$LOG_DIR" ]; then
        mkdir -p "$LOG_DIR"
    fi
    
    log "开始实时监控日志 (间隔: ${MONITOR_INTERVAL}s)"
    
    # 如果启用了 --tail，则进入交互式监控
    if [ "$SHOW_TAIL" = true ]; then
        while true; do
            show_progress
            sleep "$MONITOR_INTERVAL"
        done
    else
        # 简单模式 - 只显示行数
        local cycle=0
        while true; do
            ((cycle++))
            log "=== 收集周期 $cycle (间隔 ${MONITOR_INTERVAL}s) ==="
            
            for node in "${!NODES[@]}"; do
                collect_node_log_quiet "$node"
                local log_file="${LOG_FILES[$node]}"
                local local_path="$LOG_DIR/$log_file"
                
                if [ -f "$local_path" ]; then
                    local lines=$(wc -l < "$local_path" 2>/dev/null || echo 0)
                    printf "  %-12s: %6d 行\n" "$node" "$lines"
                fi
            done
            
            echo ""
            sleep "$MONITOR_INTERVAL"
        done
    fi
}

main "$@"
