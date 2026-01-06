#pragma once

/**
 * Malcolm-Strict 公共类型定义
 * 
 * 定义了请求追踪、消息协议和核心常量
 */

#include <cstdint>
#include <chrono>
#include <string>

namespace malcolm {

// ==================== 时间工具 ====================

using Timestamp = uint64_t;  // 纳秒级时间戳
using Duration = int64_t;    // 纳秒级时间间隔 (可为负)

/// 获取当前高精度时间戳 (纳秒)
inline Timestamp now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

/// 微秒转纳秒
constexpr Timestamp us_to_ns(uint64_t us) { return us * 1000; }
/// 毫秒转纳秒
constexpr Timestamp ms_to_ns(uint64_t ms) { return ms * 1'000'000; }
/// 纳秒转微秒
constexpr double ns_to_us(Timestamp ns) { return static_cast<double>(ns) / 1000.0; }
/// 纳秒转毫秒
constexpr double ns_to_ms(Timestamp ns) { return static_cast<double>(ns) / 1'000'000.0; }

// ==================== 常量定义 ====================

namespace constants {
    // eRPC 配置
    constexpr uint16_t kDefaultPort = 31850;
    constexpr size_t kMaxPayloadSize = 4096;      // 最大请求负载大小
    constexpr size_t kMaxWorkers = 16;            // 最大 Worker 数量
    
    // 调度参数
    constexpr size_t kSlackHistogramBins = 32;    // 松弛时间直方图桶数
    constexpr Timestamp kSlackBinWidth = us_to_ns(100);  // 每桶 100μs
    
    // 默认 SLO
    constexpr Timestamp kDefaultDeadline = ms_to_ns(10); // 10ms 默认截止时间
    
    // 指标采集
    constexpr size_t kMetricsSampleRate = 100;    // 每 100 个请求采样一次详细追踪
}

// ==================== 请求类型 ====================

/// RPC 请求类型
enum class RequestType : uint8_t {
    kGetRequest = 0,    // 读请求 (轻量)
    kPutRequest = 1,    // 写请求 (中等)
    kScanRequest = 2,   // 扫描请求 (重量)
    kCompute = 3,       // 计算密集型请求
};

// ==================== 消息协议 ====================

/// 客户端 -> Load Balancer 请求头
struct alignas(64) ClientRequest {
    uint64_t request_id;           // 全局唯一请求 ID
    Timestamp client_send_time;    // 客户端发送时间戳
    Timestamp deadline;            // 绝对截止时间
    RequestType type;              // 请求类型
    uint32_t payload_size;         // 负载大小
    uint32_t expected_service_us;  // 期望服务时间 (μs)
    // 后续跟随 payload 数据
};

/// Load Balancer -> Worker 请求头
struct alignas(64) WorkerRequest {
    uint64_t request_id;
    Timestamp deadline;
    Timestamp lb_dispatch_time;    // LB 派发时间
    RequestType type;
    uint32_t payload_size;
    uint8_t source_client_id;      // 来源客户端 ID
    // 后续跟随 payload 数据
};

/// Worker -> Load Balancer 响应头
struct alignas(64) WorkerResponse {
    uint64_t request_id;
    Timestamp worker_recv_time;    // Worker 接收时间
    Timestamp worker_done_time;    // Worker 完成时间
    uint32_t response_size;        // 响应数据大小
    uint8_t worker_id;             // 处理该请求的 Worker ID
    bool success;                  // 是否成功
    // 后续跟随响应数据
};

/// Load Balancer -> Client 响应头
struct alignas(64) ClientResponse {
    uint64_t request_id;
    Timestamp client_send_time;    // 回传原始发送时间
    Timestamp e2e_complete_time;   // E2E 完成时间
    uint32_t response_size;
    uint8_t worker_id;             // 实际处理的 Worker
    bool deadline_met;             // 是否满足截止时间
    // 后续跟随响应数据
};

// ==================== 请求追踪 (用于指标收集) ====================

struct RequestTrace {
    uint64_t request_id;
    Timestamp deadline;
    
    // 时间戳链
    Timestamp t1_client_send;
    Timestamp t2_lb_receive;
    Timestamp t3_lb_dispatch;
    Timestamp t4_worker_recv;
    Timestamp t5_worker_done;
    Timestamp t6_lb_response;
    Timestamp t7_client_recv;
    
    uint8_t target_worker_id;
    RequestType type;
    
    // 计算端到端延迟
    Timestamp e2e_latency_ns() const { 
        return t7_client_recv - t1_client_send; 
    }
    
    // 计算松弛时间 (正值表示提前完成)
    Duration slack_time_ns() const { 
        return static_cast<Duration>(deadline - t7_client_recv); 
    }
    
    // 是否违约
    bool is_deadline_miss() const { 
        return slack_time_ns() < 0; 
    }
    
    // LB 调度开销
    Timestamp lb_overhead_ns() const {
        return t3_lb_dispatch - t2_lb_receive;
    }
    
    // Worker 排队时间 (不含服务时间)
    Timestamp queue_wait_ns() const {
        // 近似: worker处理时间 - 预期服务时间
        return t5_worker_done - t4_worker_recv;
    }
};

// ==================== Worker 状态 (LB 侧维护) ====================

struct WorkerState {
    uint8_t worker_id;
    std::string address;          // IP:Port
    
    // 负载指标
    uint32_t queue_length;        // 当前队列长度
    uint32_t active_requests;     // 正在处理的请求数
    double load_ema;              // 负载指数移动平均
    
    // 松弛时间直方图 (用于 Malcolm-Strict)
    uint32_t slack_histogram[constants::kSlackHistogramBins];
    
    // 性能指标
    Timestamp avg_service_time;   // 平均服务时间
    Timestamp p99_latency;        // 最近 P99 延迟
    double deadline_miss_rate;    // 违约率
    
    // Worker 能力 (用于异构感知)
    double capacity_factor;       // 相对处理能力 (1.0 = 基准)
    
    // 健康状态
    bool is_healthy;
    Timestamp last_heartbeat;
    
    // 更新负载 EMA
    void update_load_ema(double new_load, double alpha = 0.1) {
        load_ema = alpha * new_load + (1.0 - alpha) * load_ema;
    }
};

// ==================== 调度算法类型 ====================

enum class SchedulerType {
    kPowerOf2,        // Baseline 1: 随机探针
    kMalcolm,         // Baseline 2: 原版纳什均衡
    kMalcolmStrict,   // 本方法: 分布 RL + EDF
};

inline const char* scheduler_type_name(SchedulerType type) {
    switch (type) {
        case SchedulerType::kPowerOf2: return "Power-of-2";
        case SchedulerType::kMalcolm: return "Malcolm";
        case SchedulerType::kMalcolmStrict: return "Malcolm-Strict";
        default: return "Unknown";
    }
}

// ==================== 节点内调度策略 ====================

enum class LocalSchedulerType {
    kFCFS,  // 先来先服务 (Baseline)
    kEDF,   // 最早截止时间优先 (Malcolm-Strict)
};

}  // namespace malcolm
