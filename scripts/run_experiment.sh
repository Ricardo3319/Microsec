#!/bin/bash
# Malcolm-Strict Experiment Runner
# 使用方法: ./run_experiment.sh [algorithm]
# algorithm: po2, malcolm, malcolm_strict

ALGORITHM=${1:-po2}
DURATION=60  # 秒 (比默认短，用于测试)
WARMUP=10
TARGET_RPS=50000  # 先用较低 RPS 测试

echo "=========================================="
echo "Malcolm-Strict Experiment"
echo "Algorithm: $ALGORITHM"
echo "Duration:  ${DURATION}s (warmup: ${WARMUP}s)"
echo "Target RPS: $TARGET_RPS"
echo "=========================================="

# Worker 配置
declare -A WORKERS
WORKERS["10.10.1.4"]="0:31850:fast"
WORKERS["10.10.1.5"]="1:31851:fast"
WORKERS["10.10.1.6"]="2:31852:slow"
WORKERS["10.10.1.7"]="3:31853:slow"
WORKERS["10.10.1.8"]="4:31854:slow"

LB_IP="10.10.1.3"
LB_PORT=31860
WORKER_LIST="10.10.1.4:31850,10.10.1.5:31851,10.10.1.6:31852,10.10.1.7:31853,10.10.1.8:31854"

cleanup() {
    echo ""
    echo "=== Cleaning up ==="
    for ip in "${!WORKERS[@]}"; do
        ssh $ip "sudo pkill -9 worker" 2>/dev/null &
    done
    ssh $LB_IP "sudo pkill -9 load_balancer" 2>/dev/null &
    sudo pkill -9 client 2>/dev/null
    wait
    echo "Done"
}

trap cleanup EXIT

# Step 1: 启动 Workers
echo ""
echo "=== Step 1: Starting Workers ==="
for ip in "${!WORKERS[@]}"; do
    IFS=':' read -r id port mode <<< "${WORKERS[$ip]}"
    echo "Starting Worker $id on $ip:$port ($mode)..."
    ssh $ip "cd /users/Mingyang/microSec && sudo ./build/worker --id=$id --port=$port --mode=$mode --scheduler=fcfs > logs/worker_$id.log 2>&1" &
done

echo "Waiting for Workers to initialize..."
sleep 5

# 验证 Workers 启动
echo "Verifying Workers..."
ALL_OK=true
for ip in "${!WORKERS[@]}"; do
    if ! ssh $ip "pgrep -f './build/worker'" >/dev/null 2>&1; then
        echo "ERROR: Worker on $ip not running!"
        ALL_OK=false
    fi
done
if [ "$ALL_OK" = false ]; then
    echo "Some Workers failed to start. Check logs."
    exit 1
fi
echo "All Workers started successfully!"

# Step 2: 启动 LB
echo ""
echo "=== Step 2: Starting Load Balancer ==="
ssh $LB_IP "cd /users/Mingyang/microSec && sudo ./build/load_balancer --port=$LB_PORT --algorithm=$ALGORITHM --workers=$WORKER_LIST > logs/lb.log 2>&1" &
LB_PID=$!

echo "Waiting for LB to connect to Workers..."
sleep 10

# 验证 LB
if ! ssh $LB_IP "pgrep -f './build/load_balancer'" >/dev/null 2>&1; then
    echo "ERROR: LB not running! Check logs/lb.log"
    ssh $LB_IP "tail -20 /users/Mingyang/microSec/logs/lb.log"
    exit 1
fi
echo "LB started successfully!"

# Step 3: 启动 Client
echo ""
echo "=== Step 3: Starting Client ==="
echo "Running for $((WARMUP + DURATION)) seconds..."

cd /users/Mingyang/microSec
sudo ./build/client --id=0 --lb=$LB_IP:$LB_PORT --threads=4 --target_rps=$TARGET_RPS --duration=$DURATION --warmup=$WARMUP --output=results/exp_${ALGORITHM}

echo ""
echo "=== Experiment Complete ==="
echo "Results in: results/exp_${ALGORITHM}/"
