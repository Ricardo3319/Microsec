# GitHub 推送完整指南

## 当前状态

✅ **本地 Git 仓库已初始化**
- 初始提交已创建: "Initial commit: Malcolm-Strict DRL-based load balancer"
- 32 个文件已暂存（包括完整源代码、文档、模型）
- 远程仓库已配置: `https://github.com/Ricardo3319/Microsec.git`

## 推送步骤

### 1. 执行推送命令

```bash
cd /users/Mingyang/microSec
git push -u origin main
```

### 2. 输入身份验证

当系统提示时，输入：
- **用户名**: `Ricardo3319`
- **密码或 Token**: 你的 GitHub Personal Access Token

> **推荐**: 使用 Personal Access Token 而不是密码
> - 访问: https://github.com/settings/tokens
> - 创建新 token，勾选 `repo` 权限
> - 保存 token（仅显示一次）

### 3. 等待推送完成

```
Enumerating objects: 32, done.
Counting objects: 100% (32/32), done.
...
To https://github.com/Ricardo3319/Microsec.git
 * [new branch]      main -> main
Branch 'main' set to track remote branch 'main' from 'origin'.
```

## 推送内容清单

### 源代码
- ✅ `src/client/` - Client 实现（sender、request_generator）
- ✅ `src/load_balancer/` - Load Balancer 实现（dispatcher、routing）
- ✅ `src/worker/` - Worker 实现（service、delay simulation）
- ✅ `src/scheduler/` - 所有调度算法（Po2、Malcolm、Malcolm-Strict）
- ✅ `src/common/` - 共享库（types、metrics、workload、config）

### 配置与构建
- ✅ `CMakeLists.txt` - CMake 构建配置
- ✅ `src/common/config.h` - eRPC 配置（RoCE 模式）
- ✅ `ip.txt` - CloudLab 节点 IP 映射

### 文档
- ✅ `README.md` - **项目概览 + 性能问题分析**（新增专业审查请求）
- ✅ `Malcolm-Strict.md` - 技术设计文档
- ✅ `docs/experiment-design.md` - 详细实验设计
- ✅ `.gitignore` - 忽略规则（build/, logs/, results/ 等）

### 脚本
- ✅ `scripts/orchestrate.sh` - 实验主控脚本
- ✅ `scripts/run_experiment.sh` - 单次实验脚本
- ✅ `scripts/quick_setup.sh` - 环境快速设置
- ✅ `scripts/generate_report.py` - 报告生成
- ✅ `scripts/merge_histograms.py` - 直方图合并

### 模型
- ✅ `models/malcolm_nash.pt` - Malcolm 基线模型
- ✅ `models/malcolm_strict_iqn.pt` - Malcolm-Strict IQN 模型

## 推送后的行动

### 1. 访问 GitHub 仓库

打开浏览器访问：
```
https://github.com/Ricardo3319/Microsec
```

### 2. 创建 GitHub Issues 用于专业审查

在仓库的 Issues 标签中创建以下问题：

**Issue 1: 性能问题诊断**
```
Title: Performance Issues Analysis - eRPC Modded Driver & Deadline Calculation

问题描述:
系统 P99 延迟为 961μs，P99.9 为 20ms（1000 RPS）
相同配置下 deadline miss 率固定 66.7%（与参数无关）

已识别问题:
1. eRPC modded driver 缺失
   - Warning: "Modded driver unavailable"
   - 使用标准 libibverbs，性能下降

2. Deadline 计算不一致
   - LB 检查 t5_worker_done ≤ deadline
   - 但实际延迟应该是 t7_client_recv

3. 尾部延迟在高负载下爆炸
   - 5000 RPS: P99 = 9.4ms
   - 1000 RPS: P99 = 961μs

寻求建议:
- 这些是系统性的限制还是可优化的问题？
- deadline 的正确定义应该是什么？
```

**Issue 2: eRPC 集成审查**
```
Title: Code Review - eRPC Integration & Event Loop Optimization

问题描述:
- Client 在每个循环中调用 200 次 run_event_loop_once()
- Buffer 管理使用 8000 个循环 buffer
- 是否有更优的配置方式？

寻求建议:
- eRPC API 使用是否正确？
- 事件循环设计是否符合 eRPC 最佳实践？
- 是否有同步问题（TLS assertions 之前出现过）？
```

**Issue 3: RoCE 网络优化**
```
Title: RoCE Network Configuration - Optimization Opportunities

问题描述:
当前使用:
- Mellanox ConnectX-4 Lx
- RoCE 模式 (kIsRoCE=1, kHeadroom=40)
- 25 Gbps 网络

观察到的问题:
- 网络往返延迟约 600-800μs
- P99.9 延迟为 20ms（可能与网络拥塞相关）

寻求建议:
- RoCE MTU、拥塞控制等是否需要调优？
- 是否有其他 Mellanox OFED 配置可以帮助？
```

### 3. 分享给审查人员

发送以下链接给需要审查代码的人员：

1. **仓库主页**
   - https://github.com/Ricardo3319/Microsec

2. **README（含问题分析）**
   - https://github.com/Ricardo3319/Microsec/blob/main/README.md

3. **核心源代码**
   - https://github.com/Ricardo3319/Microsec/tree/main/src

4. **性能问题讨论** (创建 Issues 后)
   - https://github.com/Ricardo3319/Microsec/issues

## 常见问题

### Q: 推送时出现 "Permission denied (publickey)"
**A**: 这是 SSH 密钥问题。我已经配置为 HTTPS，请使用 Personal Access Token。

### Q: 如何生成 Personal Access Token?
**A**: 
1. 访问 https://github.com/settings/tokens
2. 点击 "Generate new token"
3. 勾选 `repo` 权限
4. 复制 token 并保存

### Q: 推送后如何更新代码?
**A**:
```bash
# 本地修改后
git add .
git commit -m "Fix: description"
git push origin main
```

### Q: 远程仓库不存在怎么办?
**A**: 确保在 GitHub 上创建了仓库 "Ricardo3319/Microsec"

## 文件大小估计

总大小: ~30-50 MB（包含预训练模型）

可以通过以下方式减小:
```bash
# 移除模型，仅保留源代码
git rm models/
git commit -m "Remove large model files"
```

## 推送验证

推送成功后，验证:
```bash
# 查看远程分支
git branch -r

# 查看远程 URL
git remote -v

# 查看最新的远程提交
git log origin/main --oneline -1
```

---

**需要帮助？** 检查 GitHub 仓库的 Issues 标签或联系项目维护者。
