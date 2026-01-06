#pragma once

/**
 * Malcolm-Strict 调度器
 * 
 * 本方法: 基于分布强化学习 (IQN) + CVaR 优化
 * 
 * 核心创新:
 * 1. 状态空间: 松弛时间直方图 (Slack Time Histogram)
 * 2. 决策: IQN 对延迟分布尾部建模，优化 CVaR
 * 3. 节点内调度: 配合 EDF 使用
 * 
 * 预期表现:
 * - P99.9 显著优于原版 Malcolm
 * - Deadline Miss Rate 最低
 */

#include "scheduler.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>

#ifdef USE_LIBTORCH
#include <torch/script.h>
#endif

namespace malcolm {

/**
 * CVaR (Conditional Value at Risk) 计算
 * 
 * CVaR_α = E[X | X >= VaR_α]
 * 表示在最差 (1-α) 情况下的期望损失
 */
struct CVaREstimate {
    double var;    // Value at Risk
    double cvar;   // Conditional VaR
    double mean;   // 期望值
};

/**
 * Malcolm-Strict 调度器
 * 
 * 使用 Implicit Quantile Network (IQN) 估计延迟分布
 * 基于 CVaR 进行风险感知调度
 */
class MalcolmStrictScheduler : public Scheduler {
public:
    static constexpr double kDefaultCVaRAlpha = 0.95;  // 关注最差 5%
    static constexpr size_t kNumQuantileSamples = 32;  // 分位数采样数
    
    /**
     * @param model_path IQN 模型路径
     * @param cvar_alpha CVaR 风险参数 (0.9 = 关注最差 10%)
     */
    explicit MalcolmStrictScheduler(
        const std::string& model_path = "",
        double cvar_alpha = kDefaultCVaRAlpha
    ) : cvar_alpha_(cvar_alpha) {
        
#ifdef USE_LIBTORCH
        if (!model_path.empty()) {
            try {
                model_ = torch::jit::load(model_path);
                model_.eval();
                model_loaded_ = true;
                
                // 生成固定的分位数采样点 (用于 IQN)
                generate_quantile_samples();
                
                warmup();
                
                printf("[Malcolm-Strict] Model loaded: %s, CVaR alpha=%.2f\n",
                       model_path.c_str(), cvar_alpha_);
            } catch (const c10::Error& e) {
                fprintf(stderr, "[Malcolm-Strict] Failed to load model: %s\n", e.what());
            }
        }
#else
        (void)model_path;
#endif
    }
    
    ScheduleDecision schedule(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states
    ) override {
        Timestamp start = now_ns();
        
        if (worker_states.empty()) {
            return {0, 0.0, now_ns() - start};
        }
        
        uint8_t target = 0;
        double confidence = 0.0;
        
        if (model_loaded_) {
            target = schedule_iqn(request, worker_states, confidence);
        } else {
            target = schedule_heuristic(request, worker_states, confidence);
        }
        
        return {target, confidence, now_ns() - start};
    }
    
    void on_request_complete(const RequestTrace& trace) override {
        // 可用于在线学习或统计收集
        // 当前版本使用离线训练的模型
        (void)trace;
    }
    
    std::string name() const override {
        return "Malcolm-Strict";
    }
    
    SchedulerType type() const override {
        return SchedulerType::kMalcolmStrict;
    }
    
private:
    /**
     * IQN 模型推理调度
     * 
     * 1. 构建状态向量 (含松弛时间直方图)
     * 2. 对每个 Worker 估计延迟分布
     * 3. 计算 CVaR，选择风险最小的 Worker
     */
    uint8_t schedule_iqn(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states,
        double& confidence
    ) {
#ifdef USE_LIBTORCH
        torch::NoGradGuard no_grad;
        
        // 构建状态向量
        std::vector<float> state = build_state_vector(request, worker_states);
        
        auto state_tensor = torch::from_blob(
            state.data(),
            {1, static_cast<long>(state.size())},
            torch::kFloat32
        ).clone();
        
        // 分位数采样张量
        auto tau_tensor = torch::from_blob(
            quantile_samples_.data(),
            {1, static_cast<long>(kNumQuantileSamples)},
            torch::kFloat32
        ).clone();
        
        // IQN 前向传播
        // 输入: (state, tau) -> 输出: 每个 Worker 在各分位数的延迟估计
        std::vector<torch::jit::IValue> inputs;
        inputs.push_back(state_tensor);
        inputs.push_back(tau_tensor);
        
        auto output = model_.forward(inputs).toTensor();
        // output 形状: [1, num_workers, num_quantiles]
        
        size_t num_workers = worker_states.size();
        uint8_t best_worker = 0;
        double min_cvar = std::numeric_limits<double>::max();
        
        for (size_t w = 0; w < num_workers; ++w) {
            if (!worker_states[w].is_healthy) continue;
            
            // 计算该 Worker 的 CVaR
            CVaREstimate cvar = compute_cvar_from_quantiles(output, w);
            
            // 考虑 deadline 约束
            Duration slack = static_cast<Duration>(request.deadline - now_ns());
            double deadline_penalty = compute_deadline_penalty(cvar, slack);
            
            double risk_score = cvar.cvar + deadline_penalty;
            
            if (risk_score < min_cvar) {
                min_cvar = risk_score;
                best_worker = static_cast<uint8_t>(w);
            }
        }
        
        confidence = 1.0 / (1.0 + min_cvar / 1e6);  // 归一化置信度
        return best_worker;
#else
        return schedule_heuristic(request, worker_states, confidence);
#endif
    }
    
    /**
     * 从分位数估计计算 CVaR
     */
    CVaREstimate compute_cvar_from_quantiles(
        [[maybe_unused]] const torch::Tensor& quantiles,
        [[maybe_unused]] size_t worker_idx
    ) {
        CVaREstimate result{0.0, 0.0, 0.0};
        
#ifdef USE_LIBTORCH
        auto worker_quantiles = quantiles[0][worker_idx].contiguous();
        auto q = worker_quantiles.accessor<float, 1>();
        
        // 排序分位数值
        std::vector<float> sorted_q;
        for (size_t i = 0; i < kNumQuantileSamples; ++i) {
            sorted_q.push_back(q[i]);
        }
        std::sort(sorted_q.begin(), sorted_q.end());
        
        // 计算均值
        result.mean = std::accumulate(sorted_q.begin(), sorted_q.end(), 0.0f) 
                     / kNumQuantileSamples;
        
        // VaR: alpha 分位数
        size_t var_idx = static_cast<size_t>(cvar_alpha_ * kNumQuantileSamples);
        result.var = sorted_q[var_idx];
        
        // CVaR: VaR 以上的平均值
        double cvar_sum = 0.0;
        size_t cvar_count = 0;
        for (size_t i = var_idx; i < kNumQuantileSamples; ++i) {
            cvar_sum += sorted_q[i];
            ++cvar_count;
        }
        result.cvar = cvar_count > 0 ? cvar_sum / cvar_count : result.var;
#endif
        
        return result;
    }
    
    /**
     * 计算 Deadline 违约惩罚 (Barrier Function)
     * 
     * 使用对数障碍函数:
     * penalty = -log(slack / expected_latency)
     * 当 slack -> 0 时，penalty -> +∞
     */
    double compute_deadline_penalty(const CVaREstimate& cvar, Duration slack) {
        if (slack <= 0) {
            return 1e9;  // 已经违约
        }
        
        double slack_ratio = static_cast<double>(slack) / (cvar.cvar + 1e-6);
        
        if (slack_ratio <= 1.0) {
            // 高风险区域: 使用陡峭的惩罚
            return -1e6 * std::log(slack_ratio + 1e-9);
        } else if (slack_ratio <= 2.0) {
            // 警戒区域
            return 1e3 * (2.0 - slack_ratio);
        } else {
            // 安全区域
            return 0.0;
        }
    }
    
    /**
     * 启发式调度 (无模型时使用)
     * 
     * 结合多个因素:
     * 1. 松弛时间分布 (优先选择有更多"安全"任务的 Worker)
     * 2. 历史 P99 延迟
     * 3. 处理能力因子
     */
    uint8_t schedule_heuristic(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states,
        double& confidence
    ) {
        size_t n = worker_states.size();
        uint8_t best_worker = 0;
        double min_risk = std::numeric_limits<double>::max();
        
        for (size_t i = 0; i < n; ++i) {
            const auto& ws = worker_states[i];
            if (!ws.is_healthy) continue;
            
            // 风险评估
            double risk = 0.0;
            
            // 1. 队列长度风险
            risk += ws.queue_length * 100.0;
            
            // 2. 历史 P99 风险
            risk += static_cast<double>(ws.p99_latency) / 1000.0;
            
            // 3. 处理能力折扣 (capacity < 1 的 Worker 风险更高)
            risk *= (2.0 - ws.capacity_factor);
            
            // 4. 松弛时间直方图分析
            // 统计紧急任务 (前几个桶) 的数量
            size_t urgent_tasks = 0;
            for (size_t b = 0; b < 4; ++b) {
                urgent_tasks += ws.slack_histogram[b];
            }
            risk += urgent_tasks * 500.0;
            
            // 5. Deadline 约束
            Duration expected_latency = static_cast<Duration>(
                ws.avg_service_time * (1 + ws.queue_length)
            );
            Duration slack = static_cast<Duration>(request.deadline - now_ns()) 
                           - expected_latency;
            
            if (slack < 0) {
                risk += 1e6;  // 高违约风险
            } else if (slack < static_cast<Duration>(us_to_ns(100))) {
                risk += 1e4 * (1.0 - static_cast<double>(slack) / us_to_ns(100));
            }
            
            if (risk < min_risk) {
                min_risk = risk;
                best_worker = static_cast<uint8_t>(i);
            }
        }
        
        confidence = 1.0 / (1.0 + min_risk / 1e6);
        return best_worker;
    }
    
    /**
     * 构建状态向量
     * 
     * 包含:
     * - 请求特征
     * - 各 Worker 的松弛时间直方图
     * - 各 Worker 的基本状态
     */
    std::vector<float> build_state_vector(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states
    ) {
        std::vector<float> state;
        state.reserve(128);  // 预分配
        
        // 请求特征
        state.push_back(static_cast<float>(request.type));
        state.push_back(static_cast<float>(request.payload_size) / 1000.0f);
        state.push_back(static_cast<float>(request.expected_service_us) / 100.0f);
        
        // 计算到 deadline 的剩余时间
        Duration slack = static_cast<Duration>(request.deadline - now_ns());
        state.push_back(static_cast<float>(slack) / 1e6f);  // 归一化到毫秒
        
        // 各 Worker 状态
        for (const auto& ws : worker_states) {
            // 基本状态
            state.push_back(static_cast<float>(ws.load_ema));
            state.push_back(static_cast<float>(ws.queue_length) / 100.0f);
            state.push_back(static_cast<float>(ws.capacity_factor));
            state.push_back(static_cast<float>(ws.avg_service_time) / 1e6f);
            state.push_back(static_cast<float>(ws.p99_latency) / 1e6f);
            state.push_back(static_cast<float>(ws.deadline_miss_rate));
            state.push_back(ws.is_healthy ? 1.0f : 0.0f);
            
            // 松弛时间直方图 (关键特征!)
            for (size_t b = 0; b < constants::kSlackHistogramBins; ++b) {
                state.push_back(static_cast<float>(ws.slack_histogram[b]) / 100.0f);
            }
        }
        
        return state;
    }
    
    /**
     * 生成分位数采样点 (用于 IQN)
     */
    void generate_quantile_samples() {
        quantile_samples_.resize(kNumQuantileSamples);
        
        // 使用分层采样确保覆盖尾部
        for (size_t i = 0; i < kNumQuantileSamples; ++i) {
            // 线性分布 + 尾部加密
            double base = static_cast<double>(i + 1) / (kNumQuantileSamples + 1);
            
            // 对尾部区域 (> 0.9) 更密集采样
            if (i >= kNumQuantileSamples * 0.8) {
                base = 0.9 + 0.1 * (i - kNumQuantileSamples * 0.8) 
                          / (kNumQuantileSamples * 0.2);
            }
            
            quantile_samples_[i] = static_cast<float>(base);
        }
    }
    
    void warmup() {
#ifdef USE_LIBTORCH
        std::vector<float> dummy_state(256, 0.0f);
        std::vector<float> dummy_tau(kNumQuantileSamples, 0.5f);
        
        auto state_tensor = torch::from_blob(
            dummy_state.data(), {1, 256}, torch::kFloat32).clone();
        auto tau_tensor = torch::from_blob(
            dummy_tau.data(), {1, static_cast<long>(kNumQuantileSamples)}, 
            torch::kFloat32).clone();
        
        std::vector<torch::jit::IValue> inputs;
        inputs.push_back(state_tensor);
        inputs.push_back(tau_tensor);
        
        for (int i = 0; i < 100; ++i) {
            model_.forward(inputs);
        }
#endif
    }
    
private:
    double cvar_alpha_;
    bool model_loaded_ = false;
    std::vector<float> quantile_samples_;
    
#ifdef USE_LIBTORCH
    torch::jit::script::Module model_;
#endif
};

}  // namespace malcolm
