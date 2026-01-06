/**
 * Worker 上下文实现
 **/

#include "worker_context.h"
#include <chrono>
#include <iostream>

namespace malcolm {

WorkerContext::WorkerContext(const WorkerConfig& config)
    : config_(config),
      simulator_(config.capacity_factor) {
    
    // 根据调度策略创建队列
    if (config_.scheduler == LocalSchedulerType::kEDF) {
        edf_queue_ = std::make_unique<EDFQueue>(EDFQueue::Implementation::kLocked);
        printf("[Worker %u] Using EDF scheduler\n", config_.worker_id);
    } else {
        fcfs_queue_ = std::make_unique<FCFSQueue>();
        printf("[Worker %u] Using FCFS scheduler\n", config_.worker_id);
    }
    
    printf("[Worker %u] Initialized (capacity_factor=%.2f, threads=%zu)\n",
           config_.worker_id, config_.capacity_factor, config_.num_rpc_threads);
}

WorkerContext::~WorkerContext() {
    stop();
}

void WorkerContext::start() {
    if (running_.exchange(true)) {
        return;  // 已经在运行
    }
    
    printf("[Worker %u] Starting %zu worker threads...\n",
           config_.worker_id, config_.num_rpc_threads);
    
    // 启动工作线程
    for (size_t i = 0; i < config_.num_rpc_threads; ++i) {
        threads_.emplace_back([this, i]() {
            worker_thread_main(i);
        });
    }
    
    // TODO: 初始化 eRPC 并注册 RPC 处理函数
    // nexus_ = new erpc::Nexus(config_.server_uri);
    // rpc_ = new erpc::Rpc<erpc::CTransport>(nexus_, ...);
    // rpc_->run_event_loop_once();
}

void WorkerContext::stop() {
    if (!running_.exchange(false)) {
        return;  // 已经停止
    }
    
    printf("[Worker %u] Stopping...\n", config_.worker_id);
    
    // 等待所有线程结束
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
    
    // 导出最终指标
    if (!config_.metrics_output_dir.empty()) {
        export_metrics();
    }
}

void WorkerContext::wait() {
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

size_t WorkerContext::queue_length() const {
    if (edf_queue_) {
        return edf_queue_->size();
    } else if (fcfs_queue_) {
        return fcfs_queue_->size();
    }
    return 0;
}

void WorkerContext::get_slack_histogram(
    std::array<uint32_t, constants::kSlackHistogramBins>& hist) const {
    if (edf_queue_) {
        edf_queue_->get_slack_histogram(hist);
    } else {
        hist.fill(0);
    }
}

void WorkerContext::export_metrics() {
    if (config_.metrics_output_dir.empty()) {
        return;
    }
    
    metrics_.export_all(config_.metrics_output_dir);
    printf("[Worker %u] Metrics exported to %s\n",
           config_.worker_id, config_.metrics_output_dir.c_str());
}

void WorkerContext::handle_request(void* req_handle, const WorkerRequest* request) {
    Timestamp recv_time = now_ns();
    
    // 构造任务
    Task task;
    task.request_id = request->request_id;
    task.deadline = request->deadline;
    task.arrival_time = recv_time;
    task.type = request->type;
    task.payload_size = request->payload_size;
    task.request_handle = req_handle;
    
    // 入队
    if (edf_queue_) {
        edf_queue_->push(std::move(task));
    } else if (fcfs_queue_) {
        fcfs_queue_->push(std::move(task));
    }
    
    active_requests_.fetch_add(1, std::memory_order_relaxed);
}

void WorkerContext::worker_thread_main(size_t thread_id) {
    printf("[Worker %u] Thread %zu started\n", config_.worker_id, thread_id);
    
    while (running_.load(std::memory_order_relaxed)) {
        process_tasks();
    }
    
    printf("[Worker %u] Thread %zu stopped\n", config_.worker_id, thread_id);
}

void WorkerContext::process_tasks() {
    Task task;
    bool got_task = false;
    
    if (edf_queue_) {
        got_task = edf_queue_->try_pop(task);
    } else if (fcfs_queue_) {
        got_task = fcfs_queue_->try_pop(task);
    }
    
    if (!got_task) {
        // 无任务，短暂等待避免忙轮询
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        return;
    }
    
    Timestamp start = now_ns();
    
    // 模拟处理
    // 这里假设 payload 中包含期望服务时间信息
    uint32_t expected_service_us = 10;  // 默认 10μs
    Timestamp actual_time = simulator_.process(task.type, expected_service_us);
    
    Timestamp done_time = now_ns();
    
    // 注入人工延迟 (用于异构模拟)
    if (config_.artificial_delay_ns > 0) {
        Timestamp delay_end = done_time + config_.artificial_delay_ns;
        while (now_ns() < delay_end) {
            asm volatile("pause" ::: "memory");
        }
        done_time = now_ns();
    }
    
    // 检查是否违约
    bool deadline_met = done_time <= task.deadline;
    
    // 记录指标
    Timestamp e2e_latency = done_time - task.arrival_time;
    metrics_.record_latency(static_cast<int64_t>(e2e_latency));
    if (!deadline_met) {
        metrics_.record_deadline_miss();
    }
    
    // TODO: 构造响应并发送回 LB
    // WorkerResponse response;
    // response.request_id = task.request_id;
    // response.worker_recv_time = task.arrival_time;
    // response.worker_done_time = done_time;
    // response.worker_id = config_.worker_id;
    // response.success = true;
    // rpc_->enqueue_response(task.request_handle, &response, sizeof(response));
    
    active_requests_.fetch_sub(1, std::memory_order_relaxed);
    completed_requests_.fetch_add(1, std::memory_order_relaxed);
    
    (void)actual_time;
    (void)start;
}

}  // namespace malcolm
