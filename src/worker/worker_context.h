#pragma once

/**
 * Worker 上下文
 * 
 * 管理 Worker 节点的 eRPC 服务和任务队列
 */

#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <functional>

#include "../common/types.h"
#include "../common/metrics.h"
#include "../common/rpc_types.h"
#include "../scheduler/edf_queue.h"
#include "../scheduler/fcfs_queue.h"

// eRPC 头文件
#include "rpc.h"

namespace malcolm {

/**
 * Worker 配置
 */
struct WorkerConfig {
    std::string server_uri;           // 绑定地址 (如 "10.10.1.4:31850")
    uint16_t port = constants::kDefaultPort;
    uint8_t phy_port = 1;             // RDMA 物理端口 (10.10.1.x network)
    
    uint8_t worker_id = 0;            // Worker ID
    size_t num_rpc_threads = 8;       // eRPC 服务线程数
    size_t max_queue_size = 10000;    // 最大队列长度
    
    LocalSchedulerType scheduler = LocalSchedulerType::kFCFS;
    
    // 异构模拟参数
    double capacity_factor = 1.0;     // 处理能力因子 (< 1 表示 Slow Node)
    Timestamp artificial_delay_ns = 0; // 人工注入延迟
    
    // 指标导出路径
    std::string metrics_output_dir;
};

/**
 * 模拟负载处理
 * 
 * 根据请求类型和配置模拟服务时间
 */
class WorkloadSimulator {
public:
    explicit WorkloadSimulator(double capacity_factor = 1.0)
        : capacity_factor_(capacity_factor) {}
    
    /**
     * 处理请求 (阻塞，模拟计算)
     * 
     * @param type 请求类型
     * @param expected_us 期望服务时间 (微秒)
     * @return 实际花费时间 (纳秒)
     */
    Timestamp process(RequestType type, uint32_t expected_us) {
        // 根据容量因子调整实际服务时间
        // Slow Node (capacity_factor < 1) 服务时间更长
        uint64_t adjusted_us = static_cast<uint64_t>(
            expected_us / capacity_factor_
        );
        
        // 根据请求类型添加额外开销
        switch (type) {
            case RequestType::kGetRequest:
                // 轻量级读取
                break;
            case RequestType::kPutRequest:
                // 中等写入
                adjusted_us = static_cast<uint64_t>(adjusted_us * 1.2);
                break;
            case RequestType::kScanRequest:
                // 重量级扫描
                adjusted_us = static_cast<uint64_t>(adjusted_us * 2.0);
                break;
            case RequestType::kCompute:
                // 计算密集型
                adjusted_us = static_cast<uint64_t>(adjusted_us * 1.5);
                break;
        }
        
        Timestamp start = now_ns();
        
        // 忙等待模拟 (避免上下文切换抖动)
        Timestamp target = start + us_to_ns(adjusted_us);
        while (now_ns() < target) {
            // CPU 忙等待
            // 可以考虑加入一些实际计算来模拟真实负载
            asm volatile("pause" ::: "memory");
        }
        
        return now_ns() - start;
    }
    
private:
    double capacity_factor_;
};

/**
 * Worker 运行时上下文
 */
class WorkerContext {
public:
    explicit WorkerContext(const WorkerConfig& config);
    ~WorkerContext();
    
    /// 启动 Worker 服务
    void start();
    
    /// 停止 Worker 服务
    void stop();
    
    /// 等待服务结束
    void wait();
    
    /// 获取当前队列长度
    size_t queue_length() const;
    
    /// 获取松弛时间直方图
    void get_slack_histogram(
        std::array<uint32_t, constants::kSlackHistogramBins>& hist) const;
    
    /// 导出指标
    void export_metrics();
    
private:
    /// RPC 请求处理入口
    void handle_request(void* req_handle, const WorkerRequest* request);
    
    /// 工作线程主循环
    void worker_thread_main(size_t thread_id);
    
    /// 从队列取任务并处理
    void process_tasks();
    
private:
    WorkerConfig config_;
    
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
    
    // 任务队列 (EDF 或 FCFS)
    std::unique_ptr<EDFQueue> edf_queue_;
    std::unique_ptr<FCFSQueue> fcfs_queue_;
    
    // 负载模拟器
    WorkloadSimulator simulator_;
    
    // 指标收集
    MetricsCollector metrics_;
    std::atomic<uint64_t> completed_requests_{0};
    std::atomic<uint64_t> active_requests_{0};
    
    // eRPC 上下文
    erpc::Nexus* nexus_ = nullptr;
    erpc::Rpc<erpc::CTransport>* rpc_ = nullptr;
    
    // RPC 处理回调 (需要静态)
    static void request_handler(erpc::ReqHandle* req_handle, void* context);
};

}  // namespace malcolm
