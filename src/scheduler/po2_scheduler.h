#pragma once

/**
 * Power-of-2 Choices 调度器
 * 
 * Baseline 1: 随机探针，贪婪选择
 * 
 * 算法:
 * 1. 随机选择 2 个候选 Worker
 * 2. 选择负载更低的那个
 * 
 * 预期表现:
 * - 在同构环境下表现良好
 * - 在异构环境下出现严重的长尾效应
 */

#include "scheduler.h"
#include <random>
#include <algorithm>

namespace malcolm {

class Po2Scheduler : public Scheduler {
public:
    explicit Po2Scheduler(size_t num_choices = 2)
        : num_choices_(num_choices),
          rng_(std::random_device{}()) {}
    
    ScheduleDecision schedule(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states
    ) override {
        (void)request;  // Po2 不关心请求内容
        
        Timestamp start = now_ns();
        
        if (worker_states.empty()) {
            return {0, 0.0, now_ns() - start};
        }
        
        size_t num_workers = worker_states.size();
        
        // 随机选择 num_choices_ 个候选
        std::vector<size_t> candidates;
        candidates.reserve(num_choices_);
        
        std::uniform_int_distribution<size_t> dist(0, num_workers - 1);
        
        for (size_t i = 0; i < num_choices_; ++i) {
            size_t idx = dist(rng_);
            // 避免重复选择同一个 Worker
            bool duplicate = false;
            for (size_t c : candidates) {
                if (c == idx) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate || candidates.size() < num_choices_) {
                candidates.push_back(idx);
            }
        }
        
        // 选择负载最低的候选
        size_t best_idx = candidates[0];
        double best_load = worker_states[candidates[0]].load_ema;
        
        for (size_t i = 1; i < candidates.size(); ++i) {
            size_t idx = candidates[i];
            const auto& state = worker_states[idx];
            
            // 跳过不健康的 Worker
            if (!state.is_healthy) continue;
            
            double load = state.load_ema;
            if (load < best_load) {
                best_load = load;
                best_idx = idx;
            }
        }
        
        ScheduleDecision decision;
        decision.target_worker_id = static_cast<uint8_t>(best_idx);
        decision.confidence = 1.0 - best_load;  // 负载越低置信度越高
        decision.decision_time = now_ns() - start;
        
        return decision;
    }
    
    std::string name() const override {
        return "Power-of-" + std::to_string(num_choices_);
    }
    
    SchedulerType type() const override {
        return SchedulerType::kPowerOf2;
    }
    
private:
    size_t num_choices_;
    std::mt19937 rng_;
};

}  // namespace malcolm
