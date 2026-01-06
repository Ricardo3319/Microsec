/**
 * Worker 上下文实现 - 分离 I/O 和计算线程
 * 
 * 架构改进：
 * - I/O 执行线程（主线程）：
 *   1. 运行 eRPC 事件循环 (同步处理网络 I/O)
 *   2. 在 request_handler 回调中接收请求并入队到 task_queue_
 *   3. 发送响应给客户端
 * 
 * - 计算执行线程（工作线程，数量 = num_rpc_threads）：
 *   1. 从 task_queue_ 取任务 (线程安全, 无竞争)
 *   2. 执行计算模拟和延迟注入
 *   3. 更新指标 (延迟、违约)
 * 
 * 优点：
 * - 消除 eRPC 竞争：只有一个线程在 eRPC 事件循环中运行
 * - 消除 HoL 阻塞：计算不阻塞 I/O，即使计算线程阻塞在 sleep 中
 * - 可扩展性：计算线程数可独立调整
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
    
    // 根据调度策略创建队列 (接口兼容，但新架构中不使用)
    if (config_.scheduler == LocalSchedulerType::kEDF) {
        printf("[Worker %u] Using EDF scheduler (legacy interface)\n", config_.worker_id);
    } else {
        printf("[Worker %u] Using FCFS scheduler (legacy interface)\n", config_.worker_id);
    }
    
    printf("[Worker %u] Initialized (capacity_factor=%.2f, compute_threads=%zu)\n",
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
    
    // 创建 RPC 端点 (主线程)
    rpc_ = new erpc::Rpc<erpc::CTransport>(
        nexus_, 
        this,                           // context
        0,                              // rpc_id
        nullptr,                        // sm_handler (不需要)
        config_.phy_port                // 物理端口
    );
    
    printf("[Worker %u] eRPC initialized\n", config_.worker_id);
    
    // 启动计算线程池
    printf("[Worker %u] Starting %zu compute threads\n", 
           config_.worker_id, config_.num_rpc_threads);
    for (size_t i = 0; i < config_.num_rpc_threads; ++i) {
        compute_threads_.emplace_back(
            [this, i]() { compute_thread_main(i); }
        );
    }
    
    printf("[Worker %u] Running eRPC event loop in main thread...\n",
           config_.worker_id);
    
    // 主线程运行 eRPC 事件循环 (I/O 执行线程)
    // 注意：eRPC 要求 Rpc 对象在同一线程创建和使用
    while (running_.load()) {
        // 运行一次 eRPC 事件循环 - 处理入站请求
        // 这会调用 request_handler 回调，将请求入队到 task_queue_
        rpc_->run_event_loop_once();
    }
    
    printf("[Worker %u] RPC event loop stopped\n", config_.worker_id);
}

void WorkerContext::stop() {
    if (!running_.exchange(false)) {
        return;  // 已经停止
    }
    
    printf("[Worker %u] Stopping...\n", config_.worker_id);
    
    // 等待所有计算线程结束
    for (auto& t : compute_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    compute_threads_.clear();
    
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
    for (auto& t : compute_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

// 静态 RPC 请求处理回调 (I/O 线程调用)
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
    
    // 入队到线程安全队列 (发送给计算线程处理)
    worker->task_queue_.push(std::move(task));
    
    worker->active_requests_.fetch_add(1, std::memory_order_relaxed);
}

size_t WorkerContext::queue_length() const {
    return task_queue_.size();
}

void WorkerContext::get_slack_histogram(
    std::array<uint32_t, constants::kSlackHistogramBins>& hist) const {
    // 新架构中不使用 EDF 队列的 slack 直方图
    hist.fill(0);
}

void WorkerContext::export_metrics() {
    if (config_.metrics_output_dir.empty()) {
        return;
    }
    
    metrics_.export_all(config_.metrics_output_dir);
    printf("[Worker %u] Metrics exported to %s\n",
           config_.worker_id, config_.metrics_output_dir.c_str());
}

void WorkerContext::compute_thread_main(size_t thread_id) {
    printf("[Worker %u] Compute thread %zu started\n", config_.worker_id, thread_id);
    
    while (running_.load(std::memory_order_relaxed)) {
        process_tasks();
    }
    
    printf("[Worker %u] Compute thread %zu stopped\n", config_.worker_id, thread_id);
}

void WorkerContext::process_tasks() {
    Task task;
    
    // 从线程安全队列取任务 (无忙轮询)
    if (!task_queue_.try_pop(task)) {
        // 无任务，短暂等待
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        return;
    }
    
    Timestamp start = now_ns();
    Timestamp queue_time = start - task.arrival_time;
    
    // 执行计算模拟
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
    
    // 构造并发送响应 (线程安全：rpc_ 是指针，响应逻辑不会被修改)
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
        
        // 发送响应 (通过 eRPC 的线程安全接口 enqueue_response)
        rpc_->enqueue_response(req_handle, &resp_msgbuf);
    }
    
    active_requests_.fetch_sub(1, std::memory_order_relaxed);
    completed_requests_.fetch_add(1, std::memory_order_relaxed);
    
    (void)actual_time;
    (void)start;
}

}  // namespace malcolm
