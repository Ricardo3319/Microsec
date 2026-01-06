/**
 * Worker 上下文实现 - 真正的 eRPC 服务
 */

#include "worker_context.h"
#include <chrono>
#include <iostream>

namespace malcolm {

// 全局上下文指针 (用于静态回调)
static WorkerContext* g_worker_ctx = nullptr;

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
    
    g_worker_ctx = this;
    
    printf("[Worker %u] Starting eRPC service on %s...\n",
           config_.worker_id, config_.server_uri.c_str());
    
    // 初始化 eRPC Nexus
    std::string server_uri = config_.server_uri;
    nexus_ = new erpc::Nexus(server_uri, 0, 0);
    
    // 注册 RPC 处理函数
    nexus_->register_req_func(kReqLBToWorker, request_handler);
    
    // 创建 RPC 端点
    rpc_ = new erpc::Rpc<erpc::CTransport>(
        nexus_, 
        this,                           // context
        0,                              // rpc_id
        nullptr,                        // sm_handler (不需要)
        config_.phy_port                // 物理端口
    );
    
    printf("[Worker %u] eRPC initialized, running event loop in main thread...\n",
           config_.worker_id);
    
    // 注意：eRPC 要求 Rpc 对象在同一线程创建和使用
    // 因此事件循环在主线程运行，请求处理也在主线程完成（同步模式）
    printf("[Worker %u] Running, press Ctrl+C to stop...\n", config_.worker_id);
    printf("[Worker %u] RPC event loop started\n", config_.worker_id);
    
    while (running_.load()) {
        // 运行 eRPC 事件循环 - 处理入站请求
        rpc_->run_event_loop_once();
        
        // 同步处理队列中的任务（在主线程）
        process_tasks();
    }
    
    printf("[Worker %u] RPC event loop stopped\n", config_.worker_id);
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
    
    // 清理 eRPC
    if (rpc_) {
        delete rpc_;
        rpc_ = nullptr;
    }
    if (nexus_) {
        delete nexus_;
        nexus_ = nullptr;
    }
    
    // 导出最终指标
    if (!config_.metrics_output_dir.empty()) {
        export_metrics();
    }
    
    g_worker_ctx = nullptr;
}

void WorkerContext::wait() {
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

// 静态 RPC 请求处理回调
void WorkerContext::request_handler(erpc::ReqHandle* req_handle, void* context) {
    auto* worker = static_cast<WorkerContext*>(context);
    if (!worker) {
        worker = g_worker_ctx;  // fallback to global
    }
    if (!worker) return;
    
    Timestamp recv_time = now_ns();
    
    // 获取请求数据
    const erpc::MsgBuffer* req_msgbuf = req_handle->get_req_msgbuf();
    auto* request = reinterpret_cast<const RpcWorkerRequest*>(req_msgbuf->buf_);
    
    // 构造任务
    Task task;
    task.request_id = request->request_id;
    task.deadline = request->deadline;
    task.arrival_time = recv_time;
    task.type = static_cast<RequestType>(request->request_type);
    task.payload_size = request->payload_size;
    task.request_handle = req_handle;
    task.client_send_time = request->client_send_time;
    task.service_time_hint = request->service_time_hint;
    
    // 入队
    if (worker->edf_queue_) {
        worker->edf_queue_->push(std::move(task));
    } else if (worker->fcfs_queue_) {
        worker->fcfs_queue_->push(std::move(task));
    }
    
    worker->active_requests_.fetch_add(1, std::memory_order_relaxed);
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
    (void)req_handle;
    (void)request;
    // 已移到静态 request_handler
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
    Timestamp queue_time = start - task.arrival_time;
    
    // 模拟处理
    uint32_t expected_service_us = task.service_time_hint > 0 ? 
                                   task.service_time_hint : 10;
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
    
    // 构造并发送响应
    if (task.request_handle && rpc_) {
        auto* req_handle = static_cast<erpc::ReqHandle*>(task.request_handle);
        
        // 分配响应缓冲区
        erpc::MsgBuffer& resp_msgbuf = req_handle->pre_resp_msgbuf_;
        rpc_->resize_msg_buffer(&resp_msgbuf, sizeof(RpcWorkerResponse));
        
        // 填充响应
        auto* response = reinterpret_cast<RpcWorkerResponse*>(resp_msgbuf.buf_);
        response->request_id = task.request_id;
        response->worker_recv_time = task.arrival_time;
        response->worker_done_time = done_time;
        response->queue_time_ns = queue_time;
        response->service_time_us = static_cast<uint32_t>(ns_to_us(actual_time));
        response->queue_length = static_cast<uint16_t>(queue_length());
        response->worker_id = config_.worker_id;
        response->success = 1;
        
        // 发送响应
        rpc_->enqueue_response(req_handle, &resp_msgbuf);
    }
    
    active_requests_.fetch_sub(1, std::memory_order_relaxed);
    completed_requests_.fetch_add(1, std::memory_order_relaxed);
    
    (void)actual_time;
    (void)start;
}

}  // namespace malcolm
