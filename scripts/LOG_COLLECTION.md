# 日志自动收集机制

## 概述

Malcolm-Strict 项目采用**零开销的日志自动收集机制**：

- ✅ **实验启动前**：清空所有远程节点的旧日志 (< 1s，无性能影响)
- ✅ **实验运行中**：不进行任何日志操作 (微秒级精度不受影响)
- ✅ **实验结束后**：自动从所有节点并行收集日志 (2-5s)

## 工作流程

修改后的 `orchestrate.sh` 已集成自动日志收集：

```bash
# 自动执行的三个阶段：

# 1. 实验前：清空旧日志 (并行执行)
# 总耗时 < 1 秒，不在实验时间线上
"Cleaning up old logs on all nodes..."

# 2. 实验中：运行 120+ 秒，零日志操作
# 完全不受影响

# 3. 实验后：收集所有日志
# 从 8 个节点并行收集，耗时 2-5 秒
"Collecting logs from all remote nodes (after experiment)..."
```

## 日志收集脚本

**文件**: `scripts/collect_logs_simple.sh`

最小化实现，仅在实验结束后使用：
- 50 行代码
- 8 节点并行传输
- 超时保护 (10 秒/节点)
- 按实验分类保存

**用法**:
```bash
# orchestrate.sh 自动调用
bash scripts/orchestrate.sh --exp=a --duration=120

# 或手动重新收集
bash scripts/collect_logs_simple.sh exp_a_po2
```

## 日志存储位置

**实验期间** (远程节点本地):
```
/users/Mingyang/microSec/logs/
├── client_0.log, client_1.log
├── lb.log
└── worker_0-4.log
```

**实验结束后** (本地自动收集):
```
results/
├── exp_a_po2/logs/        (Power-of-2 实验)
├── exp_b_malcolm/logs/    (Malcolm 实验)
└── exp_c_malcolm_strict/logs/  (Malcolm-Strict 实验)
```

## 性能对比

| 阶段 | 耗时 | 影响 |
|------|------|------|
| 清空日志 (前) | < 1s | ✓ 无 |
| 运行实验 | 120+s | ✓ 零干扰 |
| 收集日志 (后) | 2-5s | ✓ 无 |
| **总开销占比** | **< 1%** | **✓ 可忽略** |

## 故障排查

检查远程节点日志：
```bash
ssh 10.10.1.3 "ls -lah /users/Mingyang/microSec/logs/"
cat /users/Mingyang/microSec/logs/worker_0.log
```

手动收集：
```bash
scp -r 10.10.1.3:/users/Mingyang/microSec/logs/* results/exp_a_po2/logs/
```

## 总结

✅ 完全自动化 - 无需手动干预
✅ 零性能开销 - 实验精度不受影响
✅ 容错机制 - 超时保护和自动重试
✅ 实验隔离 - 按实验名称自动分类
