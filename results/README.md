# 实验结果分析

## 结果目录结构

```
results/
├── exp_a_po2/          # Exp A: Power-of-2 调度
│   └── [待生成: 延迟直方图、统计数据]
├── exp_b_malcolm/      # Exp B: Malcolm 调度
│   └── [待生成: 延迟直方图、统计数据]
└── exp_c_malcolm_strict/  # Exp C: Malcolm-Strict 调度
    └── [待生成: 延迟直方图、统计数据]
```

## 实验成果汇总

### Exp A: Power-of-2 Scheduling (Baseline 1)

**预期结果**:
- ❌ 尾部延迟严重 (P99.9 > 50ms)
- ❌ Slow Workers 严重积压
- ❌ 方差不平衡

**实际结果**: 
- 目录: `exp_a_po2/`
- 状态: 初步测试完成
- 详细数据: [待分析直方图]

---

### Exp B: Malcolm (Original) (Baseline 2)

**预期结果**:
- ✅ 负载方差最小化
- ✅ 纳什均衡达成
- ❌ P99.9 仍然高 (方差陷阱)

**实际结果**:
- 目录: `exp_b_malcolm/`
- 状态: 运行完成
- 详细数据: [待分析直方图]

---

### Exp C: Malcolm-Strict (Proposed Method)

**预期结果**:
- ✅ P99.9 延迟大幅降低 (40-60% 改进)
- ✅ Deadline Miss Rate 最低
- ✅ 尾部延迟最优

**实际结果**:
- 目录: `exp_c_malcolm_strict/`
- 状态: 待运行 (等待性能问题修复)
- 详细数据: [待生成]

---

## 性能基准线 (1000 RPS, 全 Fast Workers)

| 指标 | 值 | 说明 |
|------|-----|------|
| P50 | 135 μs | 中位延迟 |
| P99 | 961 μs | 99% 的请求在此以下 |
| P99.9 | 20,000 μs | 99.9% 的请求在此以下 |
| **Deadline Miss** | **66.7%** | 恒定值，与参数无关 |

### 延迟分解

```
客户端端到端延迟 (e2e) = t7 - t1

时间轴:
t1: Client 发送请求 (client_send_time)
t2: LB 接收请求 (lb_recv_time)
t3: LB 分发请求 (lb_send_time)
t4: Worker 接收请求 (worker_recv_time)
t5: Worker 完成处理 (worker_done_time) ← LB 此处计算 deadline_met ❌
t6: LB 发送响应 (lb_response_time)
t7: Client 接收响应 (client_recv_time) ← 应该在此检查 deadline ✓

延迟分解:
- Client → LB: t2 - t1 ≈ 200-300 μs
- LB → Worker: t4 - t3 ≈ 200-300 μs
- Worker 处理: t5 - t4 ≈ 10 μs (min service time)
- LB → Client: t7 - t6 ≈ 200-300 μs
- 网络栈开销: ~200-300 μs
- 总计: ~600-900 μs (P50 135 μs 是异常低值，需要进一步调查)
```

## 性能瓶颈分析

### 根本原因 1: eRPC Modded Driver 缺失

**现象**:
```
eRPC warning: Modded driver unavailable. Performance will be low.
```

**影响**:
- 接收路径未优化 (性能损失 ~10x)
- P99.9 延迟膨胀到 20ms (应该 < 1ms)
- 系统无法达到理论性能

**解决方案**:
- 方案 A: 在 CloudLab 上安装 modded driver (高难度，权限限制)
- 方案 B: 寻找替代 eRPC 传输 (高风险，影响整个系统)
- 方案 C: 接受当前性能作为 baseline，关注相对改进

### 根本原因 2: Deadline 计算不一致

**问题代码**:

`src/load_balancer/lb_context_erpc.cpp` (~L150):
```cpp
// LB 在 t5 (worker_done) 时计算 deadline_met
cresp->deadline_met = (complete_time <= pending.deadline) ? 1 : 0;
```

`src/client/client_context_erpc.cpp`:
```cpp
// Client 在 t7 (client_recv) 时重新检查
bool deadline_met = (recv_time <= response->deadline);
if (!deadline_met) metrics.deadline_miss++;
```

**语义问题**:
- Worker 完成时的延迟 != Client 接收时的延迟
- 网络往返延迟 (200-300 μs) 被忽视
- 恒定 66.7% miss rate 的根本原因

**影响**:
- Malcolm-Strict DRL 无法学到正确的 deadline 约束
- 调度决策基于错误的反馈信号
- 性能评估数据可能无效

### 根本原因 3: 网络栈开销

**观察**:
- 单跳 RPC: ~200-300 μs
- 3 跳往返: ~600-900 μs

**来源**:
- eRPC event loop (每次迭代 ~50-100 μs)
- 缓冲分配/释放 (每次 ~20-50 μs)
- RDMA 网络延迟 (InfiniBand/RoCE ~100 μs)
- eRPC 内部序列化 (rope) (~50 μs)

**优化方向**:
- [ ] Event loop 优化 (批处理、减少迭代数)
- [ ] 缓冲池预分配 (减少 malloc)
- [ ] RoCE 网络调优 (MTU、拥塞控制)
- [ ] eRPC 配置参数 (kHeadroom、批大小)

## 对标测试

### Baseline 对比

| 算法 | 调度机制 | 队列类型 | P99 预期 | Deadline Miss 预期 |
|------|----------|----------|----------|--------------------|
| **Power-of-2** | 随机探针 | FCFS | ~5-10 ms | 60-70% |
| **Malcolm** | 纳什均衡 | FCFS | ~2-5 ms | 50-60% |
| **Malcolm-Strict** | IQN + CVaR | EDF | ~500-1000 μs | 20-40% |

### 期望改进

从 Power-of-2 到 Malcolm-Strict:
- P99.9 降低: **20 ms → < 2 ms** (10x 改进)
- Deadline Miss: **66.7% → 20-30%** (50% 相对改进)

## 后续实验步骤

### Phase 1: 修复已知问题
- [ ] 修复 deadline 计算不一致
- [ ] 尝试 eRPC 配置优化
- [ ] 验证日志记录的完整性

### Phase 2: 重新运行基准实验
```bash
./scripts/orchestrate.sh --exp=a --duration=120 --rps=1000
./scripts/orchestrate.sh --exp=b --duration=120 --rps=1000
```

### Phase 3: 运行 Malcolm-Strict
```bash
./scripts/orchestrate.sh --exp=c --duration=120 --rps=1000
```

### Phase 4: 数据分析
```bash
python3 scripts/merge_histograms.py
python3 scripts/generate_report.py
```

## 相关文件

- 日志分析: `logs/README.md`
- 设计文档: `Malcolm-Strict.md`
- 实验设计: `docs/experiment-design.md`
- 源代码: `src/`
- 脚本: `scripts/`
