#!/bin/bash
# orchestrate.sh - Malcolm-Strict 实验主控脚本
# 
# 用法: ./orchestrate.sh [--exp=all|a|b|c] [--duration=120]
#
# 运行三组对比实验:
#   Exp A: Power-of-2 (Baseline 1)
#   Exp B: Original Malcolm (Baseline 2)
#   Exp C: Malcolm-Strict (本方法)

set -euo pipefail

# ======================== 配置 ========================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$PROJECT_ROOT/results"
LOG_DIR="$PROJECT_ROOT/logs"
BUILD_DIR="$PROJECT_ROOT/build"

# 节点 IP 配置 (从 ip.txt 解析)
# 格式: Node X    IP
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

# 角色映射
CLIENT_NODES=("node0" "node1")
LB_NODE="node2"
FAST_WORKERS=("node3" "node4")
SLOW_WORKERS=("node5" "node6" "node7")
ALL_WORKERS=("${FAST_WORKERS[@]}" "${SLOW_WORKERS[@]}")

# 实验参数
DURATION_SEC=120        # 每轮实验持续时间
WARMUP_SEC=30           # 预热时间
TARGET_RPS=500000       # 目标 RPS
PARETO_ALPHA=1.2        # Pareto 分布参数 (重尾)
SERVICE_TIME_MIN_US=10  # 最小服务时间

# 模型路径
MALCOLM_MODEL="$PROJECT_ROOT/models/malcolm_nash.pt"
MALCOLM_STRICT_MODEL="$PROJECT_ROOT/models/malcolm_strict_iqn.pt"

# ======================== 工具函数 ========================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

log_error() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: $1" >&2
}

# 获取当前节点 IP (缓存) - 匹配 10.10.x.x 网段
get_current_ip() {
    if [ -z "${CURRENT_IP:-}" ]; then
        # 获取 10.10.x.x 网段的 IP
        CURRENT_IP=$(hostname -I | tr ' ' '\n' | grep "^10\.10\." | head -1)
        if [ -z "$CURRENT_IP" ]; then
            # 如果没有 10.10.x.x，使用第一个 IP
            CURRENT_IP=$(hostname -I | awk '{print $1}')
        fi
    fi
    echo "$CURRENT_IP"
}

ssh_run() {
    local node=$1
    shift
    local ip="${NODES[$node]}"
    local current_ip=$(get_current_ip)
    
    if [ "$ip" == "$current_ip" ]; then
        # 本地执行
        bash -c "$*"
    else
        ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ip" "$*"
    fi
}

ssh_run_bg() {
    local node=$1
    shift
    local ip="${NODES[$node]}"
    local current_ip=$(get_current_ip)
    local cmd="$*"
    
    if [ "$ip" == "$current_ip" ]; then
        # 本地后台执行
        eval "$cmd" &
    else
        # 远程后台执行 - nohup 包装整个 shell 命令
        ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$ip" \
            "nohup sh -c '$cmd' </dev/null >/dev/null 2>&1 &"
    fi
}

# 等待进程启动
wait_for_port() {
    local node=$1
    local port=$2
    local timeout=${3:-30}
    local ip="${NODES[$node]}"
    
    log "Waiting for $node:$port to be ready..."
    for ((i=0; i<timeout; i++)); do
        if ssh_run "$node" "nc -z localhost $port" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    log_error "Timeout waiting for $node:$port"
    return 1
}

# ======================== 集群操作 ========================

# 停止所有进程
stop_all() {
    log "Stopping all processes on cluster..."
    for node in "${!NODES[@]}"; do
        ssh_run "$node" "pkill -9 -f 'worker|load_balancer|client' 2>/dev/null || true" &
    done
    wait
    sleep 2
}

# 配置异构性 - 使用 cgroups v2 限制 CPU
setup_heterogeneity() {
    log "Configuring heterogeneous workers (cgroups v2)..."
    
    # Fast Workers: 不需要限制
    for node in "${FAST_WORKERS[@]}"; do
        log "  $node -> FAST (100% CPU)"
    done
    
    # Slow Workers: 创建 cgroup 并设置 20% CPU 限制
    for node in "${SLOW_WORKERS[@]}"; do
        log "  $node -> SLOW (20% CPU via cgroups v2)"
        ssh_run "$node" "echo '+cpu' | sudo tee /sys/fs/cgroup/cgroup.subtree_control >/dev/null 2>&1 || true; \
                          sudo mkdir -p /sys/fs/cgroup/malcolm_slow 2>/dev/null || true; \
                          echo '20000 100000' | sudo tee /sys/fs/cgroup/malcolm_slow/cpu.max >/dev/null; \
                          sudo chmod 666 /sys/fs/cgroup/malcolm_slow/cgroup.procs 2>/dev/null || true"
    done
}

# 同步代码到所有节点
sync_code() {
    log "Syncing code to all nodes..."
    for node in "${!NODES[@]}"; do
        if [ "$node" != "node0" ]; then
            rsync -az --exclude 'build' --exclude 'results' --exclude 'logs' \
                "$PROJECT_ROOT/" "${NODES[$node]}:$PROJECT_ROOT/" &
        fi
    done
    wait
}

# 构建项目 (在所有节点)
build_all() {
    log "Building on all nodes..."
    for node in "${!NODES[@]}"; do
        ssh_run "$node" "cd $PROJECT_ROOT && mkdir -p build && cd build && \
                         cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)" &
    done
    wait
}

# ======================== 启动组件 ========================

start_workers() {
    local scheduler=$1  # "fcfs" 或 "edf"
    log "Starting workers with scheduler=$scheduler..."
    
    local worker_id=0
    
    # Fast Workers
    for node in "${FAST_WORKERS[@]}"; do
        log "  Starting $node (FAST, id=$worker_id)"
        ssh_run_bg "$node" "cd $PROJECT_ROOT && mkdir -p $LOG_DIR $RESULTS_DIR && $BUILD_DIR/worker --id=$worker_id --port=31850 --mode=fast --scheduler=$scheduler --output=$RESULTS_DIR/worker_${worker_id} > $LOG_DIR/worker_${worker_id}.log 2>&1"
        ((worker_id++))
    done
    
    # Slow Workers - 使用 cgroups v2 限制到 20% CPU
    for node in "${SLOW_WORKERS[@]}"; do
        log "  Starting $node (SLOW, id=$worker_id) with 20% CPU limit"
        ssh_run_bg "$node" "cd $PROJECT_ROOT && mkdir -p $LOG_DIR $RESULTS_DIR && (echo \$\$ | sudo tee /sys/fs/cgroup/malcolm_slow/cgroup.procs >/dev/null; exec $BUILD_DIR/worker --id=$worker_id --port=31850 --mode=slow --scheduler=$scheduler --output=$RESULTS_DIR/worker_${worker_id}) > $LOG_DIR/worker_${worker_id}.log 2>&1"
        ((worker_id++))
    done
    
    sleep 3  # 等待 Workers 启动
}

start_load_balancer() {
    local algorithm=$1  # "po2", "malcolm", "malcolm_strict"
    local model_path=${2:-""}
    
    log "Starting Load Balancer with algorithm=$algorithm..."
    
    # 构建 Worker 列表
    local worker_list=""
    for node in "${ALL_WORKERS[@]}"; do
        if [ -n "$worker_list" ]; then
            worker_list+=","
        fi
        worker_list+="${NODES[$node]}:31850"
    done
    
    local model_opt=""
    if [ -n "$model_path" ]; then
        model_opt="--model=$model_path"
    fi
    
    ssh_run_bg "$LB_NODE" "cd $PROJECT_ROOT && mkdir -p $LOG_DIR && $BUILD_DIR/load_balancer --algorithm=$algorithm --port=31850 --workers=$worker_list $model_opt > $LOG_DIR/lb.log 2>&1"
    
    sleep 2
}

start_clients() {
    local output_dir=$1
    log "Starting clients, output=$output_dir..."
    
    local client_id=0
    local rps_per_client=$((TARGET_RPS / ${#CLIENT_NODES[@]}))
    
    for node in "${CLIENT_NODES[@]}"; do
        log "  Starting $node (client_id=$client_id, target_rps=$rps_per_client)"
        ssh_run_bg "$node" "cd $PROJECT_ROOT && mkdir -p $LOG_DIR $output_dir && $BUILD_DIR/client --id=$client_id --lb=${NODES[$LB_NODE]}:31850 --threads=8 --target_rps=$rps_per_client --duration=$DURATION_SEC --warmup=$WARMUP_SEC --pareto_alpha=$PARETO_ALPHA --output=$output_dir/client_${client_id} > $LOG_DIR/client_${client_id}.log 2>&1"
        ((client_id++))
    done
}

# ======================== 结果收集 ========================

collect_results() {
    local exp_name=$1
    local output_dir="$RESULTS_DIR/$exp_name"
    
    log "Collecting results for $exp_name..."
    mkdir -p "$output_dir"
    
    # 从 Clients 收集延迟数据
    for node in "${CLIENT_NODES[@]}"; do
        scp -r "${NODES[$node]}:$output_dir/*" "$output_dir/" 2>/dev/null || true
    done
    
    # 从 Workers 收集日志
    for node in "${ALL_WORKERS[@]}"; do
        scp "${NODES[$node]}:$LOG_DIR/*.log" "$output_dir/" 2>/dev/null || true
    done
    
    # 合并直方图
    if [ -f "$SCRIPT_DIR/merge_histograms.py" ]; then
        python3 "$SCRIPT_DIR/merge_histograms.py" \
            --inputs "$output_dir/client_*/*.hdr" \
            --output "$output_dir/combined_latency.csv" \
            2>/dev/null || true
    fi
    
    log "Results saved to $output_dir"
}

# ======================== 单次实验 ========================

run_experiment() {
    local exp_name=$1
    local algorithm=$2
    local scheduler=$3
    local model_path=${4:-""}
    
    log "=========================================="
    log "Running Experiment: $exp_name"
    log "  Algorithm:  $algorithm"
    log "  Scheduler:  $scheduler"
    log "  Duration:   $((WARMUP_SEC + DURATION_SEC))s"
    log "=========================================="
    
    # 清空所有节点的旧日志 (只需执行一次，不影响性能)
    log "Cleaning up old logs on all nodes..."
    for node in "${!NODES[@]}"; do
        ssh_run "$node" "rm -f $LOG_DIR/*.log 2>/dev/null || true" &
    done
    wait
    
    # 准备
    stop_all
    setup_heterogeneity
    
    local output_dir="$RESULTS_DIR/$exp_name"
    mkdir -p "$output_dir"
    
    # 启动组件
    start_workers "$scheduler"
    start_load_balancer "$algorithm" "$model_path"
    start_clients "$output_dir"
    
    # 等待实验完成
    local total_time=$((WARMUP_SEC + DURATION_SEC + 10))
    log "Experiment running for ${total_time}s..."
    
    for ((i=0; i<total_time; i+=10)); do
        sleep 10
        local remaining=$((total_time - i))
        if ((remaining > 0)); then
            echo -ne "\r  Progress: $((i * 100 / total_time))% ($remaining seconds remaining)   "
        fi
    done
    echo ""
    
    # 停止、收集结果
    stop_all
    collect_results "$exp_name"
    
    # 只在实验结束后收集日志 (不在实验进行中收集，以避免性能影响)
    log "Collecting logs from all remote nodes (after experiment)..."
    bash "$SCRIPT_DIR/collect_logs_simple.sh" "$exp_name"
    
    log "Experiment $exp_name completed!"
}

# ======================== 主流程 ========================

main() {
    local run_exp="all"
    
    # 解析参数
    for arg in "$@"; do
        case $arg in
            --exp=*)
                run_exp="${arg#*=}"
                ;;
            --duration=*)
                DURATION_SEC="${arg#*=}"
                ;;
            --help)
                echo "Usage: $0 [--exp=all|a|b|c] [--duration=120]"
                exit 0
                ;;
        esac
    done
    
    mkdir -p "$LOG_DIR" "$RESULTS_DIR"
    
    log "=============================================="
    log "Malcolm-Strict Experiment Suite"
    log "============================================="
    log "Project Root:  $PROJECT_ROOT"
    log "Experiments:   $run_exp"
    log "Duration:      ${DURATION_SEC}s per experiment"
    log "Target RPS:    $TARGET_RPS"
    log "=============================================="
    
    # 获取当前节点 IP
    local current_ip=$(get_current_ip)
    log "Current node IP: $current_ip"
    
    # 确保集群就绪
    log "Checking cluster connectivity..."
    for node in "${!NODES[@]}"; do
        local target_ip="${NODES[$node]}"
        if [ "$target_ip" == "$current_ip" ]; then
            log "  $node ($target_ip) - local node, OK"
            continue
        fi
        if ! ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$target_ip" "echo ok" > /dev/null 2>&1; then
            log_error "Cannot connect to $node ($target_ip)"
            exit 1
        fi
        log "  $node ($target_ip) - OK"
    done
    log "All nodes reachable."
    
    # 运行实验
    case $run_exp in
        all|a)
            # Exp A: Power-of-2 Baseline
            run_experiment "exp_a_po2" "po2" "fcfs"
            ;&  # fall through
        all|b)
            # Exp B: Original Malcolm
            run_experiment "exp_b_malcolm" "malcolm" "fcfs" "$MALCOLM_MODEL"
            ;&
        all|c)
            # Exp C: Malcolm-Strict
            run_experiment "exp_c_malcolm_strict" "malcolm_strict" "edf" "$MALCOLM_STRICT_MODEL"
            ;;
        a)
            run_experiment "exp_a_po2" "po2" "fcfs"
            ;;
        b)
            run_experiment "exp_b_malcolm" "malcolm" "fcfs" "$MALCOLM_MODEL"
            ;;
        c)
            run_experiment "exp_c_malcolm_strict" "malcolm_strict" "edf" "$MALCOLM_STRICT_MODEL"
            ;;
    esac
    
    # 生成对比报告
    if [ "$run_exp" = "all" ]; then
        log "Generating comparison report..."
        if [ -f "$SCRIPT_DIR/generate_report.py" ]; then
            python3 "$SCRIPT_DIR/generate_report.py" \
                --results_dir "$RESULTS_DIR" \
                --output "$RESULTS_DIR/comparison_report.pdf" \
                2>/dev/null || log "Report generation skipped"
        fi
    fi
    
    log "=============================================="
    log "All experiments completed!"
    log "Results: $RESULTS_DIR"
    log "=============================================="
}

main "$@"
