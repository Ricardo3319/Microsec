# 实验日志和分析

## 日志内容概览

本目录包含 Malcolm-Strict 系统在 CloudLab 8 节点集群上的运行日志。

### 日志文件说明

| 文件 | 节点 | 角色 | 说明 |
|------|------|------|------|
| `lb.log` | node2 | Load Balancer | 负载均衡器启动和运行日志 |
| `worker_0.log` | node3 | Worker (Fast) | 快速 Worker 0 日志 |
| `worker_1.log` | node4 | Worker (Fast) | 快速 Worker 1 日志 |
| `worker_2.log` | node5 | Worker (Slow) | 慢速 Worker 2 日志 |
| `worker_3.log` | node6 | Worker (Slow) | 慢速 Worker 3 日志 |
| `worker_4.log` | node7 | Worker (Slow) | 慢速 Worker 4 日志 |
| `client_0.log` | node0 | Client | 客户端 0 日志 |
| `client_1.log` | node1 | Client | 客户端 1 日志 |

## 当前实验结果汇总

### Exp A: Power-of-2 (Baseline 1)
- 状态: 部分运行
- 运行时长: ~120 秒
- 总请求数: 初步测试 (具体数据见 results/exp_a_po2/)
- 性能指标: 待分析

### Exp B: Malcolm (Baseline 2)
- 状态: 运行完成
- 运行时长: ~120 秒
- 总请求数: 初步测试
- 性能指标: 待分析

### Exp C: Malcolm-Strict (本方法)
- 状态: 未运行
- 原因: 等待性能问题修复

## 已知问题

### 1. eRPC 驱动问题
**症状**: 早期运行出现 "Invalid link layer" 错误
```
terminate called after throwing an instance of 'std::runtime_error'
  what():  Invalid link layer. Port link layer is [Ethernet]
```

**原因**: eRPC 初始化时检查到 InfiniBand 端口上层是 Ethernet，而非 RoCE

**解决**: 
- 更新 eRPC 配置 `config.h`: `kIsRoCE=1`
- 设置正确的物理端口: `phy_port=1`
- 验证 RDMA 设备正确识别

### 2. Deadline 计算不一致
**问题**: Deadline miss 率恒定为 66.7%（与参数无关）

**根本原因**:
- LB 在 t5 (worker_done) 时计算 `deadline_met`
- 但实际约束应该在 t7 (client_recv) 时评估
- 导致语义不匹配

**相关代码**:
- `src/load_balancer/lb_context_erpc.cpp`: L~150, `cresp->deadline_met = ...`
- `src/client/client_context_erpc.cpp`: response_callback 中重新检查

**影响**:
- Malcolm-Strict 调度器无法正确优化 deadline 满足率
- 性能评估数据可能无法准确反映算法有效性

### 3. eRPC Modded Driver 缺失
**症状**: P99.9 延迟为 20ms（应该 <1ms）

**原因**: 
- 使用标准 libibverbs，缺少 Mellanox 优化的接收路径
- eRPC 启动时警告: "Modded driver unavailable. Performance will be low."
- 预期性能损失: **10x**

**解决方案**: 
- 安装 eRPC modded driver (高难度，CloudLab 权限限制)
- 或接受当前性能作为 baseline

## 性能基准线

### 成功运行的实验数据 (1000 RPS, 全 Fast Workers)
```
P50:    135 μs
P99:    961 μs
P99.9:  20,000 μs (20 ms)
Deadline Miss Rate: 66.7% (constant)
```

### 诊断数据
- 单跳 RPC 延迟: ~200-300 μs (client → LB 或 LB → worker)
- 3 跳往返 (C→LB→W→LB→C): ~600-900 μs
- 网络栈开销: 主要来自 eRPC event loop 和缓冲管理

## 实验重启指南

### 快速重启
```bash
cd /users/Mingyang/microSec
./scripts/orchestrate.sh --exp=a --duration=120
```

### 完整清理 + 重启
```bash
# 清理日志和结果
rm -rf logs/*.log results/*/

# 重新编译
./scripts/quick_setup.sh

# 运行实验
./scripts/orchestrate.sh --exp=all --duration=120
```

### 数据收集
```bash
# 汇总所有节点的直方图
python3 scripts/merge_histograms.py

# 生成对比报告
python3 scripts/generate_report.py
```

## 后续分析需要

1. **eRPC 集成审查**
   - RPC 调用模式是否符合 eRPC 最佳实践？
   - 缓冲管理策略是否最优？
   - Event loop 是否需要优化？

2. **Deadline 语义修复**
   - 应该在何处检查 deadline？
   - 如何正确传播 deadline 满足信息？
   - 对调度器的影响？

3. **性能分析**
   - RoCE 网络配置是否可进一步优化？
   - 系统架构是否存在其他瓶颈？
   - eRPC modded driver 的可行性？

## 相关文件

- 设计文档: `Malcolm-Strict.md`
- 实验设计: `docs/experiment-design.md`
- 源代码: `src/` 目录
- 实验脚本: `scripts/` 目录
