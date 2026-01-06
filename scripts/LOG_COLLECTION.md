# 日志自动收集机制

## 概述

Malcolm-Strict 项目已配置**零开销的日志自动收集机制**：

- ✅ **实验启动前**：清空所有远程节点的旧日志 (无性能影响)
- ✅ **实验运行中**：不进行任何日志操作 (保证微秒级精度)
- ✅ **实验结束后**：自动从所有节点并行收集日志

## 工作流程

### 1. 自动集成到实验脚本

修改后的 `orchestrate.sh` 已集成日志收集：

```bash
# 脚本自动执行以下步骤：

# 步骤 1: 清空旧日志 (实验前一次)
"Cleaning up old logs on all nodes..."
# 清除 node0-7 上的所有 *.log 文件
# 并行执行，总耗时 < 1 秒，不在实验期间

# 步骤 2: 运行实验 (不涉及日志操作)
# 120+ 秒实验期间零干扰

# 步骤 3: 收集日志 (实验后自动)
"Collecting logs from all remote nodes (after experiment)..."
# 从 node0-7 并行收集所有 .log 文件
# 保存位置：results/exp_a_po2/logs/
#          results/exp_b_malcolm/logs/
#          results/exp_c_malcolm_strict/logs/
```

### 2. 日志收集脚本

**文件**: `scripts/collect_logs_simple.sh`

**特点**:
- 最小化代码 (50 行，专注于核心功能)
- 无持续监控 (避免性能影响)
- 并行传输 (8 个节点同时收集)
- 超时保护 (10 秒 timeout，防止单个节点卡住)
- 按实验分类 (自动保存到对应实验目录)

**用法**:
```bash
# 手动运行 (如果需要重新收集日志)
bash scripts/collect_logs_simple.sh exp_a_po2

# 或使用老版本 (功能更完整但开销更大)
bash scripts/collect_logs.sh --monitor=0  # 一次性收集
```

## 日志存储位置

### 实验进行中

每个节点本地存储:
```
/users/Mingyang/microSec/logs/
├── client_0.log      (node0)
├── client_1.log      (node1)
├── lb.log            (node2)
├── worker_0.log      (node3)
├── worker_1.log      (node4)
├── worker_2.log      (node5)
├── worker_3.log      (node6)
└── worker_4.log      (node7)
```

### 实验结束后 (本地)

自动收集到实验结果目录:
```
results/
├── exp_a_po2/
│   └── logs/
│       ├── client_0.log
│       ├── client_1.log
│       ├── lb.log
│       └── worker_*.log
├── exp_b_malcolm/
│   └── logs/
│       └── [同上]
└── exp_c_malcolm_strict/
    └── logs/
        └── [同上]
```

## 性能考虑

### 零开销设计

| 阶段 | 操作 | 耗时 | 影响 |
|------|------|------|------|
| **实验前** | 清空日志 (并行) | < 1 秒 | ✓ 无 (在启动前) |
| **实验中** | (无任何操作) | — | ✓ 完全无干扰 |
| **实验后** | 收集日志 (并行) | 2-5 秒 | ✓ 无 (实验已结束) |

### 网络优化

- **并行传输**: 8 个节点同时传输 (充分利用网络)
- **传输协议**: 
  - 优先使用 rsync (增量传输，快速)
  - 降级到 scp (普通递归复制)
- **超时控制**: 单节点 10 秒 timeout，防止卡住

### 实验精度保证

- ✅ 没有后台进程在实验进行中
- ✅ 没有网络流量竞争 (实验中无日志操作)
- ✅ 没有磁盘 I/O 竞争 (实验中无日志操作)
- ✅ 微秒级测量精度不受影响

## 日志分析

### 查看实验日志

```bash
# 查看 Power-of-2 实验的负载均衡器日志
cat results/exp_a_po2/logs/lb.log

# 查看 Malcolm 实验的 Worker 0 日志
cat results/exp_b_malcolm/logs/worker_0.log

# 统计日志大小
du -sh results/*/logs/
```

### 常见问题排查

**问题**: 某个节点的日志未收集
```bash
# 手动检查远程节点
ssh 10.10.1.3 "ls -lah /users/Mingyang/microSec/logs/"

# 手动收集该节点
scp -r 10.10.1.3:/users/Mingyang/microSec/logs/* results/exp_a_po2/logs/
```

**问题**: 日志为空或不完整
```bash
# 检查远程进程是否正常启动
ssh 10.10.1.3 "ps aux | grep worker"

# 查看进程的实时日志
ssh 10.10.1.3 "tail -f /users/Mingyang/microSec/logs/worker_0.log"
```

## 改进建议

### 如果需要更细粒度的日志收集

可以使用 `scripts/collect_logs.sh` 的连续监控模式 (但会增加开销):

```bash
# 在另一个终端运行 (实验启动后)
bash scripts/collect_logs.sh --monitor=5

# 每 5 秒收集一次日志，用于实时监控
# 警告: 可能影响实验性能，仅用于调试
```

### 如果需要更频繁的自动收集

修改 `orchestrate.sh` 中的日志清理频率:

```bash
# 当前: 实验前清一次
# 建议: 保持不变 (已是最优方案)

# 如需改变，编辑 orchestrate.sh:
# 修改这部分代码的清理逻辑
```

## 总结

✅ **日志收集已完全自动化**
- 实验启动时自动清空旧日志
- 实验运行中零干扰
- 实验结束后自动收集
- 按实验分类存储
- 支持后续手动重新收集

**立即运行**:
```bash
cd /users/Mingyang/microSec
./scripts/orchestrate.sh --exp=a --duration=120
# 日志会自动收集到 results/exp_a_po2/logs/
```
