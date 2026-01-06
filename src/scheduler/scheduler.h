#pragma once

/**
 * 调度器基类接口
 * 
 * 定义 Load Balancer 调度策略的统一接口
 */

#include <vector>
#include <memory>
#include <string>
#include "../common/types.h"

namespace malcolm {

/**
 * 调度决策结果
 */
struct ScheduleDecision {
    uint8_t target_worker_id;     // 目标 Worker ID
    double confidence;            // 决策置信度 (用于调试)
    Timestamp decision_time;      // 决策耗时
};

/**
 * 调度器基类
 * 
 * 所有调度算法都需要实现这个接口
 */
class Scheduler {
public:
    virtual ~Scheduler() = default;
    
    /**
     * 为请求选择目标 Worker
     * 
     * @param request 请求头信息
     * @param worker_states 所有 Worker 的状态
     * @return 调度决策
     */
    virtual ScheduleDecision schedule(
        const ClientRequest& request,
        const std::vector<WorkerState>& worker_states
    ) = 0;
    
    /**
     * 更新 Worker 状态 (可选，用于学习型调度器)
     * 
     * @param worker_id Worker ID
     * @param new_state 新状态
     */
    virtual void update_worker_state(uint8_t worker_id, const WorkerState& new_state) {
        (void)worker_id;
        (void)new_state;
    }
    
    /**
     * 接收请求完成反馈 (可选，用于学习型调度器)
     * 
     * @param trace 请求追踪信息
     */
    virtual void on_request_complete(const RequestTrace& trace) {
        (void)trace;
    }
    
    /**
     * 获取调度器名称
     */
    virtual std::string name() const = 0;
    
    /**
     * 获取调度器类型
     */
    virtual SchedulerType type() const = 0;
};

/**
 * 调度器工厂
 */
class SchedulerFactory {
public:
    static std::unique_ptr<Scheduler> create(
        SchedulerType type,
        const std::string& model_path = ""
    );
};

}  // namespace malcolm
