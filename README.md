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

## 预期实验结果

### Exp A: Power-of-2
- ❌ P99.9 延迟飙升 (长尾效应)
- ❌ Slow Workers 严重积压

### Exp B: Original Malcolm
- ✅ 负载方差小 (纳什均衡)
- ❌ P99.9 仍然高 (**方差陷阱**)

### Exp C: Malcolm-Strict
- ✅ P99.9 显著降低 (40-60% 改进)
- ✅ Deadline Miss Rate 最低
- 📊 证明了 IQN + EDF + Barrier Reward 的有效性

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

## 当前性能问题与分析

### 观测到的性能指标

在全 Fast Workers 配置下（1000 RPS）：

```
P50 Latency:    135 μs  ✓ (acceptable)
P99 Latency:    961 μs  ⚠ (should be <500 μs)
P99.9 Latency:  20 ms   ✗ (severe tail latency)
Deadline Miss:  66.7%   ✗ (constant, independent of parameters)
```

### 已识别的问题

#### 问题 1: eRPC Modded Driver 缺失
```
Warning: "Modded driver unavailable. Performance will be low."
```
- eRPC 期望自定义内核模块以优化接收路径
- 当前使用标准 libibverbs，性能大幅下降
- 估计影响：10x tail latency 增加

#### 问题 2: Deadline 计算方式错误
- 当前在 LB 端检查 deadline（t5_worker_done ≤ deadline）
- 但 Client 端的实际时间是 t7_client_recv
- 导致 deadline miss 率固定在 66% 左右，与参数无关

#### 问题 3: 尾部延迟爆炸
- 5000 RPS: P99 = 9.4ms, P99.9 = 69ms
- 1000 RPS: P99 = 961μs, P99.9 = 20ms
- 原因：缺少驱动优化 + RoCE 网络栈开销

### 已尝试但无效的优化

| 优化方向 | 结果 | 分析 |
|---------|------|------|
| 增加事件循环次数 (100→200) | 无改善 | 根本问题不在事件处理 |
| 改进缓冲区管理 | 无改善 | 缓冲区策略正确 |
| 提高 deadline_multiplier (5→20) | **恶化** | 反向作用，P99 增加 24 倍 |
| 降低 target_rps (5000→1000) | 部分改善 | P99 改善 11 倍，但仍不理想 |

### 请求专业分析的关键问题

1. **eRPC 集成是否正确**？
   - 是否应该使用不同的 eRPC 配置？
   - 200 次 run_event_loop_once() 调用是否合理？

2. **Deadline 语义应该如何定义**？
   - 是否应该改为基于 t7 (Client 收到) 检查？
   - 还是应该重新定义 deadline 的起点？

3. **RoCE 配置是否可优化**？
   - MTU、拥塞控制、优先级映射等设置
   - 是否有其他 Mellanox 特定的优化？

4. **系统架构是否合理**？
   - 缓存行对齐、NUMA 感知等
   - Worker 线程池设置（当前 8 个线程）

## 项目用途说明

本仓库用于：
✅ 分布式系统实验与研究
✅ 深度强化学习在尾延迟优化中的应用
✅ 调度算法对比与分析
✅ **寻求专业代码审查与性能优化建议**

---
