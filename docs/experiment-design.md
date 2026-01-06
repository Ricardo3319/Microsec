# Malcolm-Strict 实验架构设计与实施指南

> **目标**：在 CloudLab 8x xl170 集群上验证 Malcolm-Strict 在微秒级尾延迟保证方面的优越性

---

## 1. 物理拓扑与角色分配 (8 Nodes Strategy)

### 1.1 角色分配方案

```
┌─────────────────────────────────────────────────────────────────────┐
│                         8-Node Topology                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌─────────┐  ┌─────────┐                                          │
│   │ Node 0  │  │ Node 1  │    ──── CLIENT NODES (压力生成层)         │
│   │10.10.1.1│  │10.10.1.2│         2 台专用客户端                    │
│   │ Client0 │  │ Client1 │         每台运行多个 eRPC 线程            │
│   └────┬────┘  └────┬────┘                                          │
│        │            │                                                │
│        └─────┬──────┘                                                │
│              │                                                       │
│              ▼                                                       │
│   ┌─────────────────┐                                               │
│   │     Node 2      │         ──── LOAD BALANCER (调度决策层)        │
│   │   10.10.1.3     │              1 台专用 LB                       │
│   │ Load Balancer   │              运行 DRL 推理引擎                 │
│   └────────┬────────┘                                               │
│            │                                                         │
│     ┌──────┴──────┬───────────┬───────────┬──────────┐              │
│     ▼             ▼           ▼           ▼          ▼              │
│ ┌───────┐   ┌───────┐   ┌───────┐   ┌───────┐   ┌───────┐          │
│ │Node 3 │   │Node 4 │   │Node 5 │   │Node 6 │   │Node 7 │          │
│ │ FAST  │   │ FAST  │   │ SLOW  │   │ SLOW  │   │ SLOW  │          │
│ │Worker0│   │Worker1│   │Worker2│   │Worker3│   │Worker4│          │
│ │.1.4   │   │.1.5   │   │.1.6   │   │.1.7   │   │.1.8   │          │
│ └───────┘   └───────┘   └───────┘   └───────┘   └───────┘          │
│                                                                      │
│  ◀────── FAST TIER (2) ──────▶  ◀────── SLOW TIER (3) ──────▶      │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 角色详解与资源预算

| 角色 | 节点 | IP | 核心功能 | CPU 核分配 |
|------|------|-----|----------|------------|
| **Client** | Node 0, 1 | 10.10.1.1-2 | 闭环压力生成，Deadline 注入 | 全部 10 核 |
| **Load Balancer** | Node 2 | 10.10.1.3 | DRL 推理 + 请求路由 | 8 核调度 + 2 核推理 |
| **Fast Worker** | Node 3, 4 | 10.10.1.4-5 | 高吞吐后端 | 全部 10 核 |
| **Slow Worker** | Node 5, 6, 7 | 10.10.1.6-8 | 模拟异构/降级后端 | **限制为 2-4 核** |

### 1.3 设计理由

1. **2 台 Client**：xl170 有 10 核，每台可运行 8-10 个 eRPC 客户端线程，总计产生 **~2M RPS** 的开环压力
2. **1 台 LB**：集中式调度，便于公平对比三种算法（Po2、Malcolm、Malcolm-Strict）
3. **5 台 Worker (2 Fast + 3 Slow)**：非对称比例 (2:3) 模拟真实异构场景，Slow 节点更多以放大"均值陷阱"效应

---

## 2. 异构性模拟 (Simulating Heterogeneity)

### 2.1 方法选择：`cgroups v2` + `taskset` 组合

**避免使用 `cpulimit`**（基于 SIGSTOP/SIGCONT，引入不可控抖动）。推荐使用 **cgroups v2 CPU 限制**，这是 Linux 内核原生的资源隔离机制，提供稳定的 CPU 时间配额。

### 2.2 配置脚本

```bash
#!/bin/bash
# heterogeneity_setup.sh - 在各 Worker 节点上执行

NODE_ROLE=$1  # "fast" 或 "slow"

# 创建 cgroup
sudo mkdir -p /sys/fs/cgroup/malcolm-worker

case $NODE_ROLE in
    "fast")
        # Fast Node: 不限制，使用全部 10 核
        echo "max 100000" | sudo tee /sys/fs/cgroup/malcolm-worker/cpu.max
        ALLOWED_CORES="0-9"
        ;;
    "slow")
        # Slow Node: 限制为 20% CPU (模拟 2 核效果)
        # cpu.max 格式: $QUOTA $PERIOD (微秒)
        # 20000/100000 = 20% CPU
        echo "20000 100000" | sudo tee /sys/fs/cgroup/malcolm-worker/cpu.max
        ALLOWED_CORES="0-3"  # 同时限制可用核心
        ;;
esac

# 将 Worker 进程加入 cgroup
# 假设 Worker 进程 PID 存储在 /tmp/worker.pid
WORKER_PID=$(cat /tmp/worker.pid 2>/dev/null || echo "")
if [ -n "$WORKER_PID" ]; then
    echo $WORKER_PID | sudo tee /sys/fs/cgroup/malcolm-worker/cgroup.procs
    taskset -cp $ALLOWED_CORES $WORKER_PID
fi

echo "[Heterogeneity] Node configured as $NODE_ROLE (cores: $ALLOWED_CORES)"
```

### 2.3 部署命令

```bash
# 从 Node 0 (控制节点) 批量配置
# Fast Workers: Node 3, 4
ssh 10.10.1.4 "bash /path/to/heterogeneity_setup.sh fast"
ssh 10.10.1.5 "bash /path/to/heterogeneity_setup.sh fast"

# Slow Workers: Node 5, 6, 7
ssh 10.10.1.6 "bash /path/to/heterogeneity_setup.sh slow"
ssh 10.10.1.7 "bash /path/to/heterogeneity_setup.sh slow"
ssh 10.10.1.8 "bash /path/to/heterogeneity_setup.sh slow"
```

### 2.4 替代方案：eRPC Worker 线程数控制

如果不想使用 cgroups，可直接在 Worker 代码中控制 eRPC RPC 线程数：

```cpp
// worker_config.h
struct WorkerConfig {
    size_t num_rpc_threads;  // Fast: 8, Slow: 2
    size_t max_concurrent_reqs;  // Fast: 64, Slow: 16
};

// 启动时传入配置
// ./worker --threads=8 --mode=fast   (Node 3, 4)
// ./worker --threads=2 --mode=slow   (Node 5, 6, 7)
```

---

## 3. 软件技术栈 (Low-Latency Stack)

### 3.1 通信层：eRPC

**选择 eRPC 而非 DPDK**：eRPC 封装了 RDMA/DPDK 底层细节，提供友好的 RPC 语义，更适合应用层调度研究。

#### CMakeLists.txt 依赖配置

```cmake
cmake_minimum_required(VERSION 3.16)
project(MalcolmStrict CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -DNDEBUG")

# ==================== eRPC ====================
# 假设 eRPC 已编译并安装在 /opt/erpc
set(ERPC_ROOT "/opt/erpc")
include_directories(${ERPC_ROOT}/src)
link_directories(${ERPC_ROOT}/build)

# eRPC 传输后端选择 (DPDK 或 RDMA)
option(USE_DPDK "Use DPDK transport" OFF)
option(USE_RDMA "Use RDMA transport" ON)

if(USE_RDMA)
    add_definitions(-DERPC_INFINIBAND=true)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IBVERBS REQUIRED libibverbs)
    set(TRANSPORT_LIBS ibverbs pthread numa dl)
endif()

# ==================== LibTorch (推理引擎) ====================
set(CMAKE_PREFIX_PATH "/opt/libtorch")
find_package(Torch REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")

# ==================== HdrHistogram (指标收集) ====================
find_library(HDR_HISTOGRAM_LIB hdr_histogram PATHS /usr/local/lib)

# ==================== 构建目标 ====================
add_executable(client src/client.cpp)
target_link_libraries(client erpc ${TRANSPORT_LIBS})

add_executable(load_balancer 
    src/load_balancer.cpp
    src/scheduler/po2.cpp
    src/scheduler/malcolm.cpp
    src/scheduler/malcolm_strict.cpp
    src/inference/iqn_engine.cpp
)
target_link_libraries(load_balancer erpc ${TRANSPORT_LIBS} ${TORCH_LIBRARIES} ${HDR_HISTOGRAM_LIB})

add_executable(worker src/worker.cpp)
target_link_libraries(worker erpc ${TRANSPORT_LIBS} ${HDR_HISTOGRAM_LIB})
```

### 3.2 推理引擎：LibTorch vs ONNX Runtime

| 维度 | LibTorch | ONNX Runtime |
|------|----------|--------------|
| 推理延迟 | ~50-100μs (需预热) | **~10-30μs** (优化更激进) |
| 模型转换 | 无需转换 | 需 `torch.onnx.export()` |
| C++ 集成 | 原生支持 | 轻量级，无 Python 依赖 |
| 动态 Batch | 支持 | 支持但配置复杂 |

**推荐**：**ONNX Runtime** 用于最终延迟敏感实验，**LibTorch** 用于开发阶段快速迭代。

#### LibTorch 推理示例

```cpp
// src/inference/iqn_engine.h
#pragma once
#include <torch/script.h>
#include <vector>

class IQNInferenceEngine {
public:
    explicit IQNInferenceEngine(const std::string& model_path) {
        model_ = torch::jit::load(model_path);
        model_.eval();
        model_.to(torch::kCUDA);  // 如有 GPU；否则用 kCPU
        
        // 预热：消除首次推理的 JIT 编译开销
        warmup();
    }
    
    // 输入: 松弛时间直方图 (slack_histogram) + 节点状态
    // 输出: 各 Worker 的 CVaR 风险估计
    std::vector<float> infer(const std::vector<float>& state) {
        torch::NoGradGuard no_grad;
        
        auto input = torch::from_blob(
            const_cast<float*>(state.data()),
            {1, static_cast<long>(state.size())},
            torch::kFloat32
        ).to(device_);
        
        auto output = model_.forward({input}).toTensor();
        
        std::vector<float> result(output.size(1));
        std::memcpy(result.data(), output.cpu().data_ptr<float>(), 
                    result.size() * sizeof(float));
        return result;
    }
    
private:
    void warmup() {
        std::vector<float> dummy(state_dim_, 0.0f);
        for (int i = 0; i < 100; ++i) {
            infer(dummy);
        }
    }
    
    torch::jit::script::Module model_;
    torch::Device device_{torch::kCPU};
    size_t state_dim_ = 64;  // 根据实际状态维度调整
};
```

### 3.3 节点内调度：用户态 EDF 优先队列

**关键洞察**：避免内核锁竞争，使用 **无锁数据结构** + **分层时间轮 (Hierarchical Timing Wheel)**。

#### EDF 优先队列实现

```cpp
// src/scheduler/edf_queue.h
#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>

using Timestamp = uint64_t;  // 纳秒级时间戳

struct Task {
    uint64_t request_id;
    Timestamp deadline;      // 绝对截止时间
    Timestamp arrival_time;
    size_t payload_size;
    void* payload;
    
    // 用于优先队列比较：截止时间越早优先级越高
    bool operator>(const Task& other) const {
        return deadline > other.deadline;
    }
};

// 方案 A: 简单的锁保护堆 (适用于中等负载)
class EDFQueueLocked {
public:
    void push(Task&& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push(std::move(task));
    }
    
    bool try_pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heap_.empty()) return false;
        task = std::move(const_cast<Task&>(heap_.top()));
        heap_.pop();
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_.size();
    }
    
private:
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> heap_;
    mutable std::mutex mutex_;
};

// 方案 B: 分层时间轮 (适用于超高吞吐场景)
// 将 deadline 分桶，每个桶内 FIFO
class HierarchicalTimingWheel {
public:
    static constexpr size_t NUM_BUCKETS = 1024;       // 2^10 个桶
    static constexpr Timestamp BUCKET_WIDTH_NS = 1000; // 1μs 每桶
    
    void insert(Task&& task) {
        size_t bucket_idx = (task.deadline / BUCKET_WIDTH_NS) % NUM_BUCKETS;
        auto& bucket = buckets_[bucket_idx];
        std::lock_guard<std::mutex> lock(bucket.mutex);
        bucket.tasks.push_back(std::move(task));
    }
    
    bool try_get_expired(Timestamp now, Task& out_task) {
        size_t current_bucket = (now / BUCKET_WIDTH_NS) % NUM_BUCKETS;
        
        // 扫描当前及之前的桶
        for (size_t i = 0; i < NUM_BUCKETS / 4; ++i) {
            size_t idx = (current_bucket - i + NUM_BUCKETS) % NUM_BUCKETS;
            auto& bucket = buckets_[idx];
            
            std::lock_guard<std::mutex> lock(bucket.mutex);
            if (!bucket.tasks.empty()) {
                // 找到 deadline 最早的任务
                auto it = std::min_element(bucket.tasks.begin(), bucket.tasks.end(),
                    [](const Task& a, const Task& b) { return a.deadline < b.deadline; });
                if (it->deadline <= now) {
                    out_task = std::move(*it);
                    bucket.tasks.erase(it);
                    return true;
                }
            }
        }
        return false;
    }
    
private:
    struct Bucket {
        std::mutex mutex;
        std::vector<Task> tasks;
    };
    std::array<Bucket, NUM_BUCKETS> buckets_;
};

// 方案 C: 无锁跳表 (最高性能，复杂度高)
// 推荐使用 folly::ConcurrentSkipList 或 Intel TBB concurrent_priority_queue
```

#### Baseline (FCFS) 实现对比

```cpp
// FCFS: 简单的无锁队列
#include <folly/ProducerConsumerQueue.h>

class FCFSQueue {
public:
    explicit FCFSQueue(size_t capacity) : queue_(capacity) {}
    
    bool push(Task&& task) {
        return queue_.write(std::move(task));
    }
    
    bool pop(Task& task) {
        return queue_.read(task);
    }
    
private:
    folly::ProducerConsumerQueue<Task> queue_;
};
```

---

## 4. 核心实验流程 (Orchestration Script)

### 4.1 目录结构

```
/users/Mingyang/microSec/
├── scripts/
│   ├── orchestrate.sh          # 主控脚本
│   ├── deploy_workers.sh       # Worker 部署
│   ├── run_experiment.sh       # 单次实验运行
│   └── collect_metrics.sh      # 数据收集
├── configs/
│   ├── nodes.yaml              # 节点配置
│   ├── workload_heavy_tail.yaml
│   └── workload_uniform.yaml
├── results/
│   ├── exp_a_po2/
│   ├── exp_b_malcolm/
│   └── exp_c_malcolm_strict/
└── src/
    └── ...
```

### 4.2 主控脚本 `orchestrate.sh`

```bash
#!/bin/bash
# orchestrate.sh - Malcolm-Strict 实验主控脚本
set -euo pipefail

# ======================== 配置 ========================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$PROJECT_ROOT/results"
LOG_DIR="$PROJECT_ROOT/logs"

# 节点 IP (从 ip.txt 解析)
declare -A NODES=(
    ["client0"]="10.10.1.1"
    ["client1"]="10.10.1.2"
    ["lb"]="10.10.1.3"
    ["worker0_fast"]="10.10.1.4"
    ["worker1_fast"]="10.10.1.5"
    ["worker2_slow"]="10.10.1.6"
    ["worker3_slow"]="10.10.1.7"
    ["worker4_slow"]="10.10.1.8"
)

# 实验参数
DURATION_SEC=120        # 每轮实验持续时间
WARMUP_SEC=30           # 预热时间
TARGET_RPS=500000       # 目标 RPS
DEADLINE_PERCENTILE=99  # SLO 基于 P99 定义

# 负载分布参数 (Pareto)
PARETO_ALPHA=1.2        # 重尾参数 (1 < α < 2 触发方差陷阱)
SERVICE_TIME_MIN_US=10  # 最小服务时间 (μs)

# ======================== 函数定义 ========================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

ssh_run() {
    local host=$1
    shift
    ssh -o StrictHostKeyChecking=no "$host" "$@"
}

# 启动所有 Workers
start_workers() {
    local scheduler=$1  # "fcfs" 或 "edf"
    log "Starting workers with scheduler: $scheduler"
    
    # Fast Workers
    for node in worker0_fast worker1_fast; do
        ssh_run "${NODES[$node]}" "cd $PROJECT_ROOT && \
            ./build/worker --mode=fast --scheduler=$scheduler \
            --threads=8 --port=31850 &> $LOG_DIR/${node}.log &"
    done
    
    # Slow Workers
    for node in worker2_slow worker3_slow worker4_slow; do
        ssh_run "${NODES[$node]}" "cd $PROJECT_ROOT && \
            ./build/worker --mode=slow --scheduler=$scheduler \
            --threads=2 --port=31850 &> $LOG_DIR/${node}.log &"
    done
    
    sleep 3  # 等待 Workers 就绪
}

# 启动 Load Balancer
start_lb() {
    local algorithm=$1  # "po2", "malcolm", "malcolm_strict"
    local model_path=$2
    log "Starting Load Balancer with algorithm: $algorithm"
    
    ssh_run "${NODES[lb]}" "cd $PROJECT_ROOT && \
        ./build/load_balancer \
        --algorithm=$algorithm \
        --model=$model_path \
        --workers=${NODES[worker0_fast]},${NODES[worker1_fast]},${NODES[worker2_slow]},${NODES[worker3_slow]},${NODES[worker4_slow]} \
        --port=31850 &> $LOG_DIR/lb.log &"
    
    sleep 2
}

# 启动 Clients
start_clients() {
    local output_dir=$1
    log "Starting clients, output: $output_dir"
    
    for i in 0 1; do
        ssh_run "${NODES[client$i]}" "cd $PROJECT_ROOT && \
            ./build/client \
            --lb=${NODES[lb]}:31850 \
            --threads=8 \
            --target_rps=$((TARGET_RPS / 2)) \
            --duration=$DURATION_SEC \
            --warmup=$WARMUP_SEC \
            --pareto_alpha=$PARETO_ALPHA \
            --service_min_us=$SERVICE_TIME_MIN_US \
            --output=$output_dir/client${i}_latency.hdr &> $LOG_DIR/client${i}.log &"
    done
}

# 停止所有进程
stop_all() {
    log "Stopping all processes..."
    for node in "${!NODES[@]}"; do
        ssh_run "${NODES[$node]}" "pkill -f 'worker|load_balancer|client' || true" 2>/dev/null
    done
    sleep 2
}

# 收集结果
collect_results() {
    local exp_name=$1
    local output_dir="$RESULTS_DIR/$exp_name"
    mkdir -p "$output_dir"
    
    log "Collecting results for $exp_name..."
    
    # 收集 HDR 直方图文件
    for i in 0 1; do
        scp "${NODES[client$i]}:$output_dir/client${i}_latency.hdr" "$output_dir/" 2>/dev/null || true
    done
    
    # 收集 Worker 负载日志
    for node in worker0_fast worker1_fast worker2_slow worker3_slow worker4_slow; do
        scp "${NODES[$node]}:$LOG_DIR/${node}.log" "$output_dir/" 2>/dev/null || true
    done
    
    # 合并并计算分布式百分位
    python3 "$SCRIPT_DIR/merge_histograms.py" \
        --inputs "$output_dir/client*_latency.hdr" \
        --output "$output_dir/combined_latency.csv"
}

# 运行单次实验
run_experiment() {
    local exp_name=$1
    local algorithm=$2
    local scheduler=$3
    local model_path=${4:-""}
    
    log "========== Running Experiment: $exp_name =========="
    
    stop_all
    
    # 配置异构性
    "$SCRIPT_DIR/setup_heterogeneity.sh"
    
    start_workers "$scheduler"
    start_lb "$algorithm" "$model_path"
    
    local output_dir="$RESULTS_DIR/$exp_name"
    mkdir -p "$output_dir"
    start_clients "$output_dir"
    
    # 等待实验完成
    log "Experiment running for $((WARMUP_SEC + DURATION_SEC)) seconds..."
    sleep $((WARMUP_SEC + DURATION_SEC + 10))
    
    stop_all
    collect_results "$exp_name"
    
    log "Experiment $exp_name completed!"
}

# ======================== 主流程 ========================

main() {
    mkdir -p "$LOG_DIR" "$RESULTS_DIR"
    
    log "==============================================="
    log "Malcolm-Strict Experiment Suite"
    log "==============================================="
    
    # Exp A: Sanity Check - Power-of-2
    # 预期: 在异构设置下 P99 延迟飙升
    run_experiment "exp_a_po2" "po2" "fcfs"
    
    # Exp B: The "Variance Trap" - Original Malcolm
    # 预期: 负载方差小，但 P99.9 仍然高
    run_experiment "exp_b_malcolm" "malcolm" "fcfs" \
        "$PROJECT_ROOT/models/malcolm_nash.pt"
    
    # Exp C: Strict Guarantee - Malcolm-Strict
    # 预期: P99.9 显著降低，Deadline Miss Rate 最小
    run_experiment "exp_c_malcolm_strict" "malcolm_strict" "edf" \
        "$PROJECT_ROOT/models/malcolm_strict_iqn.pt"
    
    # 生成对比报告
    log "Generating comparison report..."
    python3 "$SCRIPT_DIR/generate_report.py" \
        --results_dir "$RESULTS_DIR" \
        --output "$RESULTS_DIR/comparison_report.pdf"
    
    log "==============================================="
    log "All experiments completed!"
    log "Results: $RESULTS_DIR"
    log "==============================================="
}

main "$@"
```

### 4.3 异构性配置脚本

```bash
#!/bin/bash
# setup_heterogeneity.sh

log() { echo "[Hetero] $1"; }

# Fast nodes: 不限制
for ip in 10.10.1.4 10.10.1.5; do
    log "Configuring $ip as FAST node"
    ssh $ip "sudo cgcreate -g cpu:malcolm || true && \
             sudo cgset -r cpu.cfs_quota_us=-1 malcolm"
done

# Slow nodes: 限制为 20% CPU
for ip in 10.10.1.6 10.10.1.7 10.10.1.8; do
    log "Configuring $ip as SLOW node (20% CPU)"
    ssh $ip "sudo cgcreate -g cpu:malcolm || true && \
             sudo cgset -r cpu.cfs_quota_us=20000 -r cpu.cfs_period_us=100000 malcolm"
done

log "Heterogeneity configured!"
```

---

## 5. 数据埋点与指标计算

### 5.1 数据埋点位置

```
┌─────────────────────────────────────────────────────────────────┐
│                     请求生命周期与埋点                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Client                  Load Balancer              Worker       │
│    │                          │                        │         │
│    │  ─────────────────────►  │                        │         │
│    │     T1: send_time        │                        │         │
│    │                          │                        │         │
│    │                     T2: lb_receive               │         │
│    │                     ┌────┴────┐                   │         │
│    │                     │ 调度决策 │                   │         │
│    │                     │ (推理)   │                   │         │
│    │                     └────┬────┘                   │         │
│    │                     T3: lb_dispatch               │         │
│    │                          │  ───────────────────►  │         │
│    │                          │                   T4: worker_recv│
│    │                          │                   ┌────┴────┐    │
│    │                          │                   │  执行    │    │
│    │                          │                   └────┬────┘    │
│    │                          │                   T5: worker_done│
│    │                          │  ◄───────────────────  │         │
│    │                     T6: lb_response               │         │
│    │  ◄─────────────────────  │                        │         │
│    │     T7: client_recv       │                        │         │
│    │                          │                        │         │
│    ▼                          ▼                        ▼         │
│                                                                  │
│  关键指标:                                                       │
│  • E2E Latency = T7 - T1                                        │
│  • LB Overhead = T3 - T2 (含推理时间)                            │
│  • Queue Wait  = T5 - T4 - service_time                         │
│  • Slack Time  = deadline - T7 (负值 = 违约)                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 C++ 埋点代码

```cpp
// src/common/metrics.h
#pragma once
#include <hdr/hdr_histogram.h>
#include <atomic>
#include <chrono>
#include <string>
#include <fstream>

// 高精度时间戳
inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// 请求追踪结构
struct RequestTrace {
    uint64_t request_id;
    uint64_t deadline_ns;       // 绝对截止时间
    
    // 时间戳 (纳秒)
    uint64_t t1_client_send;
    uint64_t t2_lb_receive;
    uint64_t t3_lb_dispatch;
    uint64_t t4_worker_recv;
    uint64_t t5_worker_done;
    uint64_t t6_lb_response;
    uint64_t t7_client_recv;
    
    // 派发目标
    uint8_t target_worker_id;
    
    // 计算指标
    uint64_t e2e_latency_ns() const { return t7_client_recv - t1_client_send; }
    int64_t slack_time_ns() const { return static_cast<int64_t>(deadline_ns - t7_client_recv); }
    bool is_deadline_miss() const { return slack_time_ns() < 0; }
};

// HdrHistogram 封装
class LatencyHistogram {
public:
    LatencyHistogram(int64_t lowest_trackable = 1,         // 1 ns
                     int64_t highest_trackable = 10'000'000'000,  // 10s
                     int significant_figures = 3) {
        hdr_init(lowest_trackable, highest_trackable, significant_figures, &hist_);
    }
    
    ~LatencyHistogram() {
        hdr_close(hist_);
    }
    
    void record(int64_t value_ns) {
        hdr_record_value(hist_, value_ns);
    }
    
    // 获取百分位 (返回纳秒)
    int64_t percentile(double p) const {
        return hdr_value_at_percentile(hist_, p);
    }
    
    // P50, P99, P99.9, P99.99
    void print_summary(const std::string& name) const {
        printf("[%s] P50=%.2fus P99=%.2fus P99.9=%.2fus P99.99=%.2fus\n",
               name.c_str(),
               percentile(50.0) / 1000.0,
               percentile(99.0) / 1000.0,
               percentile(99.9) / 1000.0,
               percentile(99.99) / 1000.0);
    }
    
    // 导出为 HDR 格式 (可用于后续合并)
    void export_hdr(const std::string& path) const {
        FILE* fp = fopen(path.c_str(), "w");
        if (fp) {
            hdr_percentiles_print(hist_, fp, 5, 1.0, CLASSIC);
            fclose(fp);
        }
    }
    
    // 导出原始数据用于 CDF 绘图
    void export_cdf(const std::string& path, int num_points = 10000) const {
        std::ofstream out(path);
        out << "percentile,latency_ns\n";
        for (int i = 0; i <= num_points; ++i) {
            double p = 100.0 * i / num_points;
            out << p << "," << percentile(p) << "\n";
        }
    }
    
private:
    hdr_histogram* hist_;
};

// 全局指标收集器
class MetricsCollector {
public:
    static MetricsCollector& instance() {
        static MetricsCollector inst;
        return inst;
    }
    
    void record_request(const RequestTrace& trace) {
        e2e_latency_.record(trace.e2e_latency_ns());
        
        if (trace.is_deadline_miss()) {
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        }
        total_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    
    double deadline_miss_rate() const {
        uint64_t total = total_requests_.load();
        if (total == 0) return 0.0;
        return static_cast<double>(deadline_misses_.load()) / total;
    }
    
    const LatencyHistogram& e2e_latency() const { return e2e_latency_; }
    
    void export_all(const std::string& dir) {
        e2e_latency_.export_hdr(dir + "/e2e_latency.hdr");
        e2e_latency_.export_cdf(dir + "/e2e_latency_cdf.csv");
        
        std::ofstream summary(dir + "/summary.txt");
        summary << "Total Requests: " << total_requests_.load() << "\n";
        summary << "Deadline Misses: " << deadline_misses_.load() << "\n";
        summary << "Deadline Miss Rate: " << deadline_miss_rate() * 100 << "%\n";
        summary << "P99 Latency: " << e2e_latency_.percentile(99.0) / 1000.0 << " us\n";
        summary << "P99.9 Latency: " << e2e_latency_.percentile(99.9) / 1000.0 << " us\n";
    }
    
private:
    LatencyHistogram e2e_latency_;
    std::atomic<uint64_t> deadline_misses_{0};
    std::atomic<uint64_t> total_requests_{0};
};
```

### 5.3 分布式直方图合并

由于多个 Client 并行收集，需要在最终分析时合并：

```python
#!/usr/bin/env python3
# scripts/merge_histograms.py
"""合并多个 HdrHistogram 文件并计算全局百分位"""

import argparse
import numpy as np
from pathlib import Path
import glob

def parse_hdr_file(path):
    """解析 HdrHistogram 导出的 CLASSIC 格式"""
    latencies = []
    with open(path, 'r') as f:
        for line in f:
            if line.startswith('#') or line.startswith('Value'):
                continue
            parts = line.strip().split()
            if len(parts) >= 4:
                value = float(parts[0])
                count = int(float(parts[3]))  # TotalCount
                latencies.extend([value] * max(1, count // 1000))  # 下采样
    return np.array(latencies)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inputs', type=str, required=True, help='Glob pattern for input files')
    parser.add_argument('--output', type=str, required=True, help='Output CSV path')
    args = parser.parse_args()
    
    all_latencies = []
    for path in glob.glob(args.inputs):
        print(f"Loading {path}...")
        all_latencies.append(parse_hdr_file(path))
    
    if not all_latencies:
        print("No files found!")
        return
    
    combined = np.concatenate(all_latencies)
    combined.sort()
    
    # 计算百分位
    percentiles = [50, 90, 95, 99, 99.5, 99.9, 99.99]
    print("\n=== Combined Latency Statistics ===")
    for p in percentiles:
        val = np.percentile(combined, p) / 1000  # ns -> us
        print(f"P{p}: {val:.2f} us")
    
    # 导出 CDF
    with open(args.output, 'w') as f:
        f.write("percentile,latency_us\n")
        for i in range(10001):
            p = i / 100
            val = np.percentile(combined, p) / 1000
            f.write(f"{p},{val:.3f}\n")
    
    print(f"\nCDF exported to {args.output}")

if __name__ == '__main__':
    main()
```

### 5.4 指标收集建议

| 指标 | 推荐方案 | 原因 |
|------|----------|------|
| **延迟分布 (CDF/百分位)** | **HdrHistogram** | 对数压缩，内存占用小 (~1KB)，支持合并 |
| **Deadline Miss Rate** | 原子计数器 | 轻量级，无锁 |
| **Worker 负载 (Load)** | 指数移动平均 (EMA) | 平滑抖动，每 100μs 采样 |
| **推理延迟** | 独立 Histogram | 隔离 LB 开销 |
| **吞吐量 (RPS)** | 滑动窗口计数 | 1 秒窗口 |

---

## 6. 预期实验结果模式

### 6.1 Exp A: Power-of-2 (Sanity Check)

```
预期现象:
- P99 延迟: ~5-10x P50 (长尾明显)
- 在异构设置下, Slow Workers 排队严重
- 热点效应: 2 个 Fast Workers 处理 ~60% 流量

验证成功标志:
- CDF 曲线在 P99+ 区域急剧上升
- 各 Worker 负载方差大
```

### 6.2 Exp B: Original Malcolm (Variance Trap)

```
预期现象:
- 各节点负载非常均衡 (Var(load) → 0)
- 但 P99.9 仍然高! (证明纳什均衡 ≠ 低延迟)
- Slow Workers 被分配了 "相同" 的负载, 但服务时间更长

验证成功标志:
- 负载方差比 Exp A 低 50%+
- P99.9 延迟仅比 Exp A 低 10-20%
- 这就是 "方差陷阱"!
```

### 6.3 Exp C: Malcolm-Strict (Strict Guarantee)

```
预期现象:
- P99.9 延迟显著降低 (比 Exp B 低 40-60%)
- Deadline Miss Rate 最低
- 负载分布不均匀 (CVaR 优化会保守地避开 Slow Workers)

验证成功标志:
- CDF 曲线在尾部区域明显压低
- Deadline Miss Rate < 1%
- 这证明了 IQN + EDF + Barrier Reward 的有效性!
```

---

## 7. 快速启动检查清单

```bash
# 1. 在所有节点安装依赖
for ip in 10.10.1.{1..8}; do
    ssh $ip "sudo apt update && sudo apt install -y \
        build-essential cmake libhdr-histogram-dev \
        libnuma-dev libhugetlbfs-dev cgroup-tools"
done

# 2. 编译 eRPC
cd /opt && git clone https://github.com/erpc-io/eRPC.git
cd eRPC && cmake -DPERF=ON -DTRANSPORT=infiniband . && make -j

# 3. 下载 LibTorch
cd /opt && wget https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.0.0%2Bcpu.zip
unzip libtorch-*.zip

# 4. 编译 Malcolm-Strict
cd /users/Mingyang/microSec
mkdir build && cd build
cmake -DCMAKE_PREFIX_PATH=/opt/libtorch -DERPC_ROOT=/opt/eRPC ..
make -j

# 5. 运行实验
cd /users/Mingyang/microSec/scripts
chmod +x *.sh
./orchestrate.sh
```

---

> **下一步建议**：
> 1. 创建 `src/` 目录结构并实现核心组件
> 2. 训练 IQN 模型并导出为 TorchScript
> 3. 运行 Sanity Check (Exp A) 验证测试床正常工作
