#pragma once

/**
 * eRPC 消息类型和协议定义
 * 
 * 定义 Client <-> LB <-> Worker 之间的 RPC 消息格式
 */

#include "types.h"
#include <cstdint>
#include <cstring>

namespace malcolm {

// ==================== RPC 类型 ID ====================
enum class RpcType : uint8_t {
    // Client -> LB
    kClientRequest = 1,
    
    // LB -> Worker  
    kWorkerRequest = 2,
    
    // Worker -> LB (响应)
    kWorkerResponse = 3,
    
    // LB -> Client (响应)
    kClientResponse = 4,
    
    // 控制消息
    kHeartbeat = 10,
    kStateUpdate = 11,
};

// ==================== Client -> LB 请求 ====================
struct RpcClientRequest {
    uint64_t request_id;          // 请求唯一ID
    uint64_t client_send_time;    // 客户端发送时间戳 (ns)
    uint64_t deadline;            // 绝对截止时间 (ns)
    uint32_t service_time_hint;   // 预期服务时间 (us)
    uint8_t  client_id;           // 客户端ID
    uint8_t  request_type;        // 请求类型
    uint16_t payload_size;        // 有效载荷大小
    
    // 可变长度 payload 跟在后面
} __attribute__((packed));

// ==================== LB -> Worker 请求 ====================
struct RpcWorkerRequest {
    uint64_t request_id;          // 原始请求ID
    uint64_t client_send_time;    // 客户端原始发送时间
    uint64_t deadline;            // 截止时间
    uint64_t lb_forward_time;     // LB 转发时间
    uint32_t service_time_hint;   // 服务时间提示
    uint8_t  worker_id;           // 目标 Worker ID
    uint8_t  request_type;        // 请求类型
    uint16_t payload_size;        // 载荷大小
} __attribute__((packed));

// ==================== Worker -> LB 响应 ====================
struct RpcWorkerResponse {
    uint64_t request_id;          // 请求ID
    uint64_t worker_recv_time;    // Worker 接收时间
    uint64_t worker_done_time;    // Worker 完成时间
    uint64_t queue_time_ns;       // 排队时间
    uint32_t service_time_us;     // 实际服务时间
    uint16_t queue_length;        // 当前队列长度
    uint8_t  worker_id;           // Worker ID
    uint8_t  success;             // 是否成功
} __attribute__((packed));

// ==================== LB -> Client 响应 ====================
struct RpcClientResponse {
    uint64_t request_id;          // 请求ID
    uint64_t client_send_time;    // 原始发送时间 (用于计算 RTT)
    uint64_t e2e_latency_ns;      // 端到端延迟
    uint32_t service_time_us;     // 服务时间
    uint8_t  worker_id;           // 处理的 Worker ID
    uint8_t  deadline_met;        // 是否满足 deadline
    uint8_t  success;             // 是否成功
    uint8_t  _padding;
} __attribute__((packed));

// ==================== Worker 状态更新 (LB 轮询) ====================
struct RpcStateUpdate {
    uint16_t queue_length;        // 当前队列长度
    uint16_t active_requests;     // 正在处理的请求数
    uint32_t completed_requests;  // 已完成请求数
    float    load_ema;            // 负载 EMA
    uint8_t  worker_id;
    uint8_t  is_healthy;
    uint8_t  _padding[2];
    
    // 松弛时间直方图 (用于 Malcolm-Strict)
    uint32_t slack_histogram[constants::kSlackHistogramBins];
} __attribute__((packed));

// ==================== 消息大小限制 ====================
constexpr size_t kMaxPayloadSize = 4096;
constexpr size_t kMaxRequestSize = sizeof(RpcClientRequest) + kMaxPayloadSize;
constexpr size_t kMaxResponseSize = sizeof(RpcClientResponse);

// ==================== eRPC 会话配置 ====================
constexpr uint8_t kReqClientToLB = 1;      // Client->LB 请求类型
constexpr uint8_t kReqLBToWorker = 2;      // LB->Worker 请求类型
constexpr uint8_t kReqStateUpdate = 3;     // 状态更新请求类型

}  // namespace malcolm
