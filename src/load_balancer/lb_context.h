#pragma once

/**
 * Load Balancer 上下文
 * 
 * 实现请求路由和调度决策
 */

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../common/types.h"
#include "../common/metrics.h"
#include "../common/rpc_types.h"
#include "../scheduler/scheduler.h"

// eRPC
#include "rpc.h"

namespace malcolm {

/**
 * Load Balancer 配置
 */
struct LBConfig {
    std::string listen_uri;         // 监听地址 (如 "0.0.0.0:31850")
    uint16_t port = constants::kDefaultPort;
    
    std::vector<std::string> worker_addresses;  // Worker 地址列表
    
    SchedulerType algorithm = SchedulerType::kPowerOf2;
    std::string model_path;         // DRL 模型路径
    
    size_t num_rpc_threads = 8;     // eRPC 服务线程数
    
    // 状态更新间隔
    Timestamp state_update_interval_ns = us_to_ns(100);  // 100μs
    
    // 输出
    std::string metrics_output_dir;
};

/**
 * Load Balancer 运行时上下文
 */
class LBContext {
public:
    explicit LBContext(const LBConfig& config);
    ~LBContext();
    
    /// 启动 LB 服务
    void start();
    
    /// 停止 LB 服务
    void stop();
    
    /// 等待服务结束
    void wait();
    
    /// 导出指标
    void export_metrics();
    
private:
    /// 处理客户端请求
    void handle_client_request(void* req_handle, const ClientRequest* request);
    
    /// 处理 Worker 响应
    void handle_worker_response(const WorkerResponse* response);
    
    /// 状态更新线程
    void state_update_thread_main();
    
    /// 发送请求到 Worker
    void send_to_worker(uint8_t worker_id, const WorkerRequest& request);
    
    /// 更新 Worker 状态
    void update_worker_states();
    
private:
    LBConfig config_;
    
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
    std::thread state_thread_;
    
    // 调度器
    std::unique_ptr<Scheduler> scheduler_;
    
    // Worker 状态
    std::vector<WorkerState> worker_states_;
    mutable std::mutex state_mutex_;
    
    // 未完成请求追踪
    struct PendingRequest {
        uint64_t request_id;
        Timestamp send_time;
        Timestamp deadline;
        void* client_handle;
        uint8_t target_worker;
    };
    std::unordered_map<uint64_t, PendingRequest> pending_requests_;
    std::mutex pending_mutex_;
    
    // 指标收集
    MetricsCollector metrics_;
    LatencyHistogram scheduling_latency_;
    
    // eRPC 上下文
    erpc::Nexus* nexus_ = nullptr;
    erpc::Rpc<erpc::CTransport>* rpc_ = nullptr;
    std::vector<int> worker_sessions_;  // 到每个 Worker 的会话
    
    // RPC 回调
    static void client_request_handler(erpc::ReqHandle* req_handle, void* context);
    static void worker_response_callback(void* context, void* tag);
};

}  // namespace malcolm
