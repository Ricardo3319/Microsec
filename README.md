# Malcolm-Strict 实验快速参考

## 项目结构概览

```
microSec/
├── CMakeLists.txt              # 项目构建配置
├── ip.txt                      # CloudLab 节点 IP 配置
├── Malcolm-Strict.md           # 技术设计文档
│
├── docs/
│   └── experiment-design.md    # 详细实验设计方案
│
├── scripts/
│   ├── orchestrate.sh          # 实验主控脚本 ★
│   ├── quick_setup.sh          # 快速环境设置
│   ├── merge_histograms.py     # 合并延迟直方图
│   └── generate_report.py      # 生成对比报告
│
├── src/
│   ├── common/
│   │   ├── types.h             # 核心类型定义
│   │   ├── metrics.h/cpp       # HdrHistogram 指标收集
│   │   ├── workload.h/cpp      # Pareto/重尾负载生成
│   │   └── config.h/cpp        # 配置管理
│   │
│   ├── scheduler/
│   │   ├── scheduler.h         # 调度器接口
│   │   ├── po2_scheduler.h     # Baseline 1: Power-of-2
│   │   ├── malcolm_scheduler.h # Baseline 2: 纳什均衡
│   │   ├── malcolm_strict_scheduler.h  # 本方法: IQN + CVaR
│   │   ├── edf_queue.h/cpp     # EDF 优先队列
│   │   └── fcfs_queue.h/cpp    # FCFS 队列
│   │
│   ├── load_balancer/
│   │   ├── lb_context.h/cpp    # LB 运行时上下文
│   │   └── main.cpp            # LB 入口点
│   │
│   ├── worker/
│   │   ├── worker_context.h/cpp # Worker 运行时
│   │   └── main.cpp            # Worker 入口点
│   │
│   └── client/
│       ├── client_context.h/cpp # 客户端上下文
│       ├── request_generator.cpp
│       └── main.cpp            # Client 入口点
│
├── models/                     # (待创建) 训练好的模型
│   ├── malcolm_nash.pt
│   └── malcolm_strict_iqn.pt
│
├── results/                    # (运行时生成) 实验结果
└── logs/                       # (运行时生成) 日志文件
```

## 快速开始

### 1. 环境设置 (在 node0 上运行)

```bash
cd /users/Mingyang/microSec
./scripts/quick_setup.sh
```

### 2. 手动编译 (如果需要)

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DERPC_ROOT=/opt/erpc \
      -DCMAKE_PREFIX_PATH=/opt/libtorch \
      -DUSE_RDMA=ON ..
make -j$(nproc)
```

### 3. 运行全部实验

```bash
./scripts/orchestrate.sh --exp=all --duration=120
```

### 4. 运行单个实验

```bash
# Exp A: Power-of-2 (Baseline 1)
./scripts/orchestrate.sh --exp=a

# Exp B: Original Malcolm (Baseline 2)
./scripts/orchestrate.sh --exp=b

# Exp C: Malcolm-Strict (本方法)
./scripts/orchestrate.sh --exp=c
```

## 节点角色分配

| 节点 | IP | 角色 | 配置 |
|------|-----|------|------|
| Node 0 | 10.10.1.1 | Client 0 | 8 线程, 250K RPS |
| Node 1 | 10.10.1.2 | Client 1 | 8 线程, 250K RPS |
| Node 2 | 10.10.1.3 | Load Balancer | DRL 推理 + 路由 |
| Node 3 | 10.10.1.4 | Worker 0 (Fast) | 100% CPU, 8 线程 |
| Node 4 | 10.10.1.5 | Worker 1 (Fast) | 100% CPU, 8 线程 |
| Node 5 | 10.10.1.6 | Worker 2 (Slow) | 20% CPU, 2 线程 |
| Node 6 | 10.10.1.7 | Worker 3 (Slow) | 20% CPU, 2 线程 |
| Node 7 | 10.10.1.8 | Worker 4 (Slow) | 20% CPU, 2 线程 |

## 关键实验参数

```bash
# orchestrate.sh 中的默认参数
DURATION_SEC=120        # 实验持续时间
WARMUP_SEC=30           # 预热时间
TARGET_RPS=500000       # 目标总 RPS
PARETO_ALPHA=1.2        # 重尾分布参数 (触发方差陷阱)
SERVICE_TIME_MIN_US=10  # 最小服务时间
```

## 调度器对比

| 算法 | 调度策略 | 节点内队列 | 目标函数 |
|------|----------|------------|----------|
| Power-of-2 | 随机探针 | FCFS | min(random probe load) |
| Malcolm | 纳什均衡 | FCFS | min(load variance) |
| **Malcolm-Strict** | **IQN + CVaR** | **EDF** | **max(deadline satisfaction)** |

## 预期实验结果 (2026.01 实验状态)

### ✅ 已完成实验

**Exp A: Power-of-2 (异构节点)**
- ✅ 吞吐量：**6,966 RPS**
- ✅ P99 延迟：**24,084.48 μs** (24.08 ms)
- ✅ 违约率：**60.1337%**
- 结论：确实存在方差陷阱

### ❌ 未完成实验

| 实验 | 配置 | 模型状态 | 预期性能 | 备注 |
|------|------|---------|--------|------|
| **Exp B** | 异构 + Malcolm | 空 (0 字节) | ~15-20K RPS | 需训练 malcolm_nash.pt |
| **Exp C** | 异构 + Malcolm-Strict | 空 (0 字节) | ~25-35K RPS | 需训练 malcolm_strict_iqn.pt |

## 指标收集位置

```cpp
// 关键埋点
T1: client_send_time    // Client 发送
T2: lb_receive_time     // LB 接收
T3: lb_dispatch_time    // LB 派发 (含推理时间)
T4: worker_recv_time    // Worker 接收
T5: worker_done_time    // Worker 完成
T6: lb_response_time    // LB 响应
T7: client_recv_time    // Client 接收

// 核心指标
E2E Latency = T7 - T1
LB Overhead = T3 - T2
Slack Time = deadline - T7
Deadline Miss = (Slack Time < 0)
```

## 模型训练 (单独进行)

1. 训练 Malcolm-Strict IQN 模型 (Python/PyTorch)
2. 导出为 TorchScript:
   ```python
   traced = torch.jit.trace(model, example_input)
   traced.save("models/malcolm_strict_iqn.pt")
   ```
3. 复制到 `models/` 目录

## 结果分析

```bash
# 合并多个客户端的延迟直方图
python3 scripts/merge_histograms.py \
    --inputs "results/exp_c_malcolm_strict/client_*/*.hdr" \
    --output "results/exp_c_malcolm_strict/combined_latency.csv"

# 生成对比报告
python3 scripts/generate_report.py \
    --results_dir results/ \
    --output results/comparison_report.pdf
```

## 故障排查

### eRPC 连接问题
```bash
# 检查 RDMA 设备
ibv_devinfo

# 检查端口占用
netstat -tulpn | grep 31850
```

### 进程清理
```bash
# 清理所有节点上的残留进程
for ip in 10.10.1.{1..8}; do
    ssh $ip "pkill -9 -f 'worker|load_balancer|client'" &
done
wait
```

### 日志查看
```bash
# 查看 LB 日志
tail -f logs/lb.log

# 查看 Worker 日志
tail -f logs/worker_*.log
```

---

**Contact**: 查看 [Malcolm-Strict.md](Malcolm-Strict.md) 获取完整技术设计

---

## 系统架构创新 (2026.01 更新)

### 核心改进通过

在基础 Malcolm-Strict 架构上，我们实施了 3 项关键工程创新，以确保系统的正确性和性能：

#### **创新 1: Worker I/O 卸载架构**
**问题**: eRPC 库的线程本地存储(TLS)初始化要求，compute threads 直接调用 eRPC 方法导致 segfault。

**解决方案**:
- 分离 I/O 线程（主线程处理 eRPC）和 compute 线程池
- Compute threads 只负责计算，结果推送到 `ThreadSafeTaskQueue<completion_queue_>`
- 主线程轮询 completion_queue，调用 eRPC `enqueue_response()`
- 零额外开销：避免锁争用，保证线程安全

**代码架构** ([src/worker/worker_context.h](src/worker/worker_context.h#L1)):
```cpp
class WorkerContext {
  ThreadSafeTaskQueue<Task> task_queue_;       // 任务入队
  ThreadSafeTaskQueue<Task> completion_queue_; // 完成出队
  std::vector<std::thread> compute_threads_;
  
  void start();                    // Main thread: eRPC loop
  void process_completions();      // Main: handle completion_queue_
  void compute_thread_main();      // Worker: consume task_queue_
};
```

#### **创新 2: 基于客户端时钟的 Deadline 判定**
**问题**: 原设计在 Worker 端检查 deadline (基于服务器时间)，但实际约束来自客户端接收时间 (T7)。不同时钟域导致 miss 率恒定 66.7%。

**解决方案**:
- 在 Client 端维护 `req_deadlines_[]` 数组（并行于缓冲池）
- 请求发送时记录: `req_deadlines_[idx] = creq.deadline`
- 将缓冲区索引作为标签传递: `reinterpret_cast<void*>(idx)`
- 响应回调中恢复索引，使用本地时钟检查: `recv_time <= req_deadlines_[idx]`

**关键改变** ([src/client/client_context_erpc.cpp](src/client/client_context_erpc.cpp#L1)):
```cpp
// 发送时
req_deadlines_[idx] = creq.deadline;
send_packet(..., reinterpret_cast<void*>(idx));

// 响应时
size_t idx = reinterpret_cast<size_t>(tag);
Timestamp original_deadline = req_deadlines_[idx];
bool deadline_met = (recv_time <= original_deadline);
```

**结果**: Deadline 判定从服务器时钟转为客户端时钟，精度改进。

#### **创新 3: 服务时间与 Deadline 解耦**
**问题**: 原逻辑将 service_time_hint 计算为 `(deadline - send_time) / 5`，混淆了截止时间宽度和实际服务时间。

**解决方案**:
- 使用 RequestGenerator 生成的原始期望服务时间: `creq.expected_service_us`
- 将其直接作为 service_time_hint 传递给调度器
- Worker 获得准确的服务时间估计，而非截止时间衍生值

**修改位置** (2 处 [src/client/client_context_erpc.cpp](src/client/client_context_erpc.cpp)):
```cpp
// 改为
rpc_req->service_time_hint = creq.expected_service_us;
```

### 部署验证

| 环境 | 状态 | 验证方式 |
|------|------|---------|
| 本地编译 (node0) | ✅ 通过 | 无警告编译，生成二进制 |
| node2 (LB) | ✅ 通过 | 远程编译成功 |
| node3-7 (Workers) | ✅ 通过 | 所有 8 节点编译成功 |
| node1 (Client) | ✅ 通过 | 二进制部署并验证新逻辑 |

**提交历史** (2026.01):
- c649a88: Fix service_time_hint to use expected_service_us
- db9f622: Implement client-side deadline judgment with req_deadlines_[]
- fdbefc8: Add debug logging with thread ID tracking
- 8b96ab4: Fix eRPC TLS violation with I/O offloading
- 63ccf17: Worker I/O architecture refactoring

---

## 实验评估进度 (2026.01)

### ✅ 已完成工作
- ✅ 系统架构设计 (3 项工程创新)
- ✅ 代码实现与编译
- ✅ 多节点部署准备
- ✅ **Exp A 运行完成** ← NEW
- ✅ 数据采集 (eRPC 日志)

### 实验进度

| 实验 | 配置 | 模型训练 | 系统部署 | 数据采集 |
|------|------|---------|---------|---------|
| Exp A | Po2 异构 | N/A | ✅ | ✅ |
| Exp B | Malcolm 异构 | ❌ | ✅ | ❌ |
| Exp C | Malcolm-Strict 异构 | ❌ | ✅ | ❌ |

### 待进行工作
- [ ] **模型训练**
  - 训练 Malcolm 纳什均衡模型 → malcolm_nash.pt
  - 训练 Malcolm-Strict IQN/CVaR 模型 → malcolm_strict_iqn.pt
  
- [ ] **Exp B/C 实验**
  - 部署模型到 Load Balancer
  - 运行异构集群上的完整对比实验
  - 采集延迟直方图和调度决策日志
  
- [ ] **数据分析与论文**
  - 对比三种调度器性能
  - 验证方差陷阱现象
  - 学术发表

---
