#pragma once

/**
 * Malcolm 调度器 (Original)
 * 
 * Baseline 2: 基于纳什均衡的 MARL 调度
 * 
 * 核心机制:
 * - 目标函数: 最小化节点间负载方差
 * - 达成纳什均衡状态
 * 
 * 关键痛点 (待验证):
 * - 在重尾分布下陷入"均值陷阱"
 * - 方差最小化 ≠ 低尾延迟
 */

#include "scheduler.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

#ifdef USE_LIBTORCH
#include <torch/script.h>
#endif

namespace malcolm {

/**
 * Malcolm 原版调度器
 * 
 * 这里提供两种模式:
 * 1. 模型推理模式: 加载训练好的 PyTorch 模型
 * 2. 启发式模式: 使用分析公式模拟纳什均衡策略
 */
class MalcolmScheduler : public Scheduler {
public:
    /**
     * @param model_path PyTorch 模型路径 (可选)
     * @param use_heuristic 是否使用启发式模式
     */
    explicit MalcolmScheduler(
        const std::string& model_path = "",
        bool use_heuristic = true
    ) : use_heuristic_(use_heuristic) {
        
#ifdef USE_LIBTORCH
        if (!model_path.empty() && !use_heuristic_) {
            try {
                model_ = torch::jit::load(model_path);
                model_.eval();
                model_loaded_ = true;
                
                // 预热
                warmup();
            } catch (const c10::Error& e) {
                fprintf(stderr, "[Malcolm] Failed to load model: %s\n", e.what());
                use_heuristic_ = true;
            }
        }
#else
        (void)model_path;
        use_heuristic_ = true;
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
        
        if (use_heuristic_) {
            target = schedule_heuristic(request, worker_states, confidence);
        } else {
            target = schedule_model(request, worker_states, confidence);
        }
        
        return {target, confidence, now_ns() - start};
    }
    
    std::string name() const override {
        return use_heuristic_ ? "Malcolm-Heuristic" : "Malcolm-Model";
    }
    
    SchedulerType type() const override {
        return SchedulerType::kMalcolm;
    }
    
private:
    /**
     * 启发式调度 - 模拟纳什均衡
     * 
     * 思路: 选择使得负载方差增加最小的 Worker
     * 
     * 对于每个 Worker i, 计算如果将请求派给它:
     *   新负载 = load[i] + 1
     *   新方差 = Var({load[0], ..., load[i]+1, ..., load[n-1]})
     * 选择使新方差最小的 Worker
     */
    uint8_t schedule_heuristic(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states,
        double& confidence
    ) {
        (void)request;
        
        size_t n = worker_states.size();
        
        // 收集当前负载
        std::vector<double> loads;
        loads.reserve(n);
        for (const auto& ws : worker_states) {
            loads.push_back(ws.load_ema);
        }
        
        // 计算当前均值
        double mean = std::accumulate(loads.begin(), loads.end(), 0.0) / n;
        
        // 模拟加入请求后的方差变化
        uint8_t best_worker = 0;
        double min_variance_increase = std::numeric_limits<double>::max();
        
        for (size_t i = 0; i < n; ++i) {
            if (!worker_states[i].is_healthy) continue;
            
            // 模拟新负载
            double old_load = loads[i];
            double new_load = old_load + 1.0;  // 简化: 每请求增加 1 单位负载
            
            // 计算新方差 (增量公式)
            // Var_new = Var_old + 2*(load[i] - mean)*(1/n) + (1/n - 1/n^2)
            // 简化: 使用 (new_load - mean)^2 的变化
            double delta = (new_load - mean) * (new_load - mean) 
                         - (old_load - mean) * (old_load - mean);
            
            if (delta < min_variance_increase) {
                min_variance_increase = delta;
                best_worker = static_cast<uint8_t>(i);
            }
        }
        
        // 置信度: 负载越均衡置信度越高
        double current_var = 0.0;
        for (double l : loads) {
            current_var += (l - mean) * (l - mean);
        }
        current_var /= n;
        confidence = std::exp(-current_var);  // 方差越小置信度越高
        
        return best_worker;
    }
    
    /**
     * 模型推理调度
     */
    uint8_t schedule_model(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states,
        double& confidence
    ) {
#ifdef USE_LIBTORCH
        if (!model_loaded_) {
            return schedule_heuristic(request, worker_states, confidence);
        }
        
        // 构建状态向量
        std::vector<float> state = build_state_vector(request, worker_states);
        
        torch::NoGradGuard no_grad;
        
        auto input = torch::from_blob(
            state.data(),
            {1, static_cast<long>(state.size())},
            torch::kFloat32
        ).clone();  // clone 确保内存安全
        
        auto output = model_.forward({input}).toTensor();
        
        // 输出是每个 Worker 的 Q 值
        auto q_values = output.accessor<float, 2>();
        
        uint8_t best_worker = 0;
        float best_q = q_values[0][0];
        
        for (size_t i = 1; i < worker_states.size(); ++i) {
            if (q_values[0][i] > best_q && worker_states[i].is_healthy) {
                best_q = q_values[0][i];
                best_worker = static_cast<uint8_t>(i);
            }
        }
        
        confidence = static_cast<double>(best_q);
        return best_worker;
#else
        return schedule_heuristic(request, worker_states, confidence);
#endif
    }
    
    /**
     * 构建状态向量 (用于模型输入)
     */
    std::vector<float> build_state_vector(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states
    ) {
        std::vector<float> state;
        
        // 请求特征
        state.push_back(static_cast<float>(request.type));
        state.push_back(static_cast<float>(request.payload_size) / 1000.0f);
        state.push_back(static_cast<float>(request.expected_service_us) / 100.0f);
        
        // 各 Worker 状态
        for (const auto& ws : worker_states) {
            state.push_back(static_cast<float>(ws.load_ema));
            state.push_back(static_cast<float>(ws.queue_length) / 100.0f);
            state.push_back(static_cast<float>(ws.capacity_factor));
            state.push_back(ws.is_healthy ? 1.0f : 0.0f);
        }
        
        return state;
    }
    
    void warmup() {
#ifdef USE_LIBTORCH
        std::vector<float> dummy(64, 0.0f);
        auto input = torch::from_blob(dummy.data(), {1, 64}, torch::kFloat32).clone();
        for (int i = 0; i < 100; ++i) {
            model_.forward({input});
        }
#endif
    }
    
private:
    bool use_heuristic_;
    bool model_loaded_ = false;
    
#ifdef USE_LIBTORCH
    torch::jit::script::Module model_;
#endif
};

}  // namespace malcolm
