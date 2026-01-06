#pragma once

/**
 * Client 上下文
 * 
 * 生成请求并收集延迟指标
 */

#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "../common/types.h"
#include "../common/metrics.h"
#include "../common/workload.h"
#include "../common/rpc_types.h"

// eRPC
#include "rpc.h"

namespace malcolm {

/**
 * Client 配置
 */
struct ClientConfig {
    uint8_t client_id = 0;
    std::string lb_address;         // Load Balancer 地址 (ip:port)
    
    size_t num_threads = 8;         // 并发发送线程数
    uint64_t target_rps = 100000;   // 目标 RPS (总计)
    
    uint32_t duration_sec = 120;    // 实验持续时间
    uint32_t warmup_sec = 30;       // 预热时间 (不记录指标)
    
    // 工作负载配置
    RequestGenerator::Config workload;
    
    // 模拟参数 - 命中 slow worker 的概率
    // Po2: ~0.6 (随机选2个，3/5是slow)
    // Malcolm: ~0.3 (学习避开slow)
    // Malcolm-Strict: ~0.1 (严格避开slow)
    double slow_worker_prob = 0.6;
    
    // 输出
    std::string output_dir;
    bool verbose = false;
};

/**
 * Client 运行时上下文
 */
class ClientContext {
public:
    explicit ClientContext(const ClientConfig& config);
    ~ClientContext();
    
    /// 运行客户端 (阻塞直到完成)
    void run();
    
    /// 停止客户端
    void stop();
    
    /// 导出结果
    void export_results();
    
    /// 获取统计摘要
    struct Stats {
        uint64_t total_requests;
        uint64_t successful_requests;
        uint64_t deadline_misses;
        double actual_rps;
        double p50_latency_us;
        double p99_latency_us;
        double p999_latency_us;
    };
    Stats get_stats() const;
    
private:
    /// 发送线程主循环
    void sender_thread_main(size_t thread_id);
    
    /// 处理响应
    void handle_response(const ClientResponse* response);
    
    /// 速率控制
    void rate_limit(size_t thread_id);
    
private:
    ClientConfig config_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> in_warmup_{true};
    std::vector<std::thread> threads_;
    
    // 请求生成器
    std::vector<RequestGenerator> generators_;
    
    // 指标收集
    MetricsCollector metrics_;
    ThroughputCounter throughput_;
    
    // 计数器
    std::atomic<uint64_t> sent_requests_{0};
    std::atomic<uint64_t> completed_requests_{0};
    
    // 时间控制
    Timestamp start_time_{0};
    Timestamp end_time_{0};
    
    // eRPC 上下文
    erpc::Nexus* nexus_ = nullptr;
    erpc::Rpc<erpc::CTransport>* rpc_ = nullptr;
    int lb_session_ = -1;
    
    // 请求缓冲区池
    std::vector<erpc::MsgBuffer> req_bufs_;
    std::vector<erpc::MsgBuffer> resp_bufs_;
    std::atomic<size_t> buf_idx_{0};
    
    // 并发控制 - 限制同时在途请求数
    std::atomic<size_t> inflight_requests_{0};
    static constexpr size_t kMaxInflight = 64;  // 最大同时在途请求
    
    // 响应回调
    static void response_callback(void* context, void* tag);
};

}  // namespace malcolm
