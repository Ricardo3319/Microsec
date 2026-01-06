/**
 * Load Balancer 上下文实现
 */

#include "lb_context.h"
#include "../scheduler/po2_scheduler.h"
#include "../scheduler/malcolm_scheduler.h"
#include "../scheduler/malcolm_strict_scheduler.h"
#include <chrono>
#include <iostream>

namespace malcolm {

LBContext::LBContext(const LBConfig& config)
    : config_(config) {
    
    // 创建调度器
    switch (config_.algorithm) {
        case SchedulerType::kPowerOf2:
            scheduler_ = std::make_unique<Po2Scheduler>();
            break;
        case SchedulerType::kMalcolm:
            scheduler_ = std::make_unique<MalcolmScheduler>(config_.model_path);
            break;
        case SchedulerType::kMalcolmStrict:
            scheduler_ = std::make_unique<MalcolmStrictScheduler>(config_.model_path);
            break;
    }
    
    printf("[LB] Using scheduler: %s\n", scheduler_->name().c_str());
    
    // 初始化 Worker 状态
    worker_states_.resize(config_.worker_addresses.size());
    for (size_t i = 0; i < worker_states_.size(); ++i) {
        auto& ws = worker_states_[i];
        ws.worker_id = static_cast<uint8_t>(i);
        ws.address = config_.worker_addresses[i];
        ws.is_healthy = true;
        ws.capacity_factor = 1.0;
        ws.load_ema = 0.0;
        ws.queue_length = 0;
        memset(ws.slack_histogram, 0, sizeof(ws.slack_histogram));
    }
    
    printf("[LB] Initialized with %zu workers\n", worker_states_.size());
}

LBContext::~LBContext() {
    stop();
}

void LBContext::start() {
    if (running_.exchange(true)) {
        return;
    }
    
    printf("[LB] Starting on %s...\n", config_.listen_uri.c_str());
    
    // 启动状态更新线程
    state_thread_ = std::thread([this]() {
        state_update_thread_main();
    });
    
    // TODO: 初始化 eRPC
    // - 创建 Nexus
    // - 注册 RPC 处理函数
    // - 连接到所有 Workers
    
    printf("[LB] Running\n");
}

void LBContext::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    
    printf("[LB] Stopping...\n");
    
    if (state_thread_.joinable()) {
        state_thread_.join();
    }
    
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    if (!config_.metrics_output_dir.empty()) {
        export_metrics();
    }
}

void LBContext::wait() {
    if (state_thread_.joinable()) {
        state_thread_.join();
    }
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void LBContext::export_metrics() {
    if (config_.metrics_output_dir.empty()) return;
    
    metrics_.export_all(config_.metrics_output_dir);
    scheduling_latency_.export_hdr(config_.metrics_output_dir + "/scheduling_latency.hdr");
    
    printf("[LB] Metrics exported to %s\n", config_.metrics_output_dir.c_str());
}

void LBContext::handle_client_request(void* req_handle, const ClientRequest* request) {
    Timestamp recv_time = now_ns();
    
    // 调度决策
    ScheduleDecision decision;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        decision = scheduler_->schedule(*request, worker_states_);
    }
    
    // 记录调度延迟
    scheduling_latency_.record(decision.decision_time);
    
    // 构造 Worker 请求
    WorkerRequest wreq;
    wreq.request_id = request->request_id;
    wreq.deadline = request->deadline;
    wreq.lb_dispatch_time = now_ns();
    wreq.type = request->type;
    wreq.payload_size = request->payload_size;
    wreq.source_client_id = 0;  // TODO: 从 req_handle 提取
    
    // 记录待处理请求
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        PendingRequest pending;
        pending.request_id = request->request_id;
        pending.send_time = recv_time;
        pending.deadline = request->deadline;
        pending.client_handle = req_handle;
        pending.target_worker = decision.target_worker_id;
        pending_requests_[request->request_id] = pending;
    }
    
    // 更新目标 Worker 的负载估计
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& ws = worker_states_[decision.target_worker_id];
        ws.queue_length++;
        ws.update_load_ema(ws.queue_length);
    }
    
    // 发送到 Worker
    send_to_worker(decision.target_worker_id, wreq);
}

void LBContext::handle_worker_response(const WorkerResponse* response) {
    Timestamp complete_time = now_ns();
    
    PendingRequest pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(response->request_id);
        if (it == pending_requests_.end()) {
            fprintf(stderr, "[LB] Unknown response for request %lu\n", 
                    response->request_id);
            return;
        }
        pending = it->second;
        pending_requests_.erase(it);
    }
    
    // 更新 Worker 状态
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& ws = worker_states_[response->worker_id];
        if (ws.queue_length > 0) {
            ws.queue_length--;
        }
        ws.update_load_ema(ws.queue_length);
        
        // 更新服务时间统计
        Timestamp service_time = response->worker_done_time - response->worker_recv_time;
        ws.avg_service_time = static_cast<Timestamp>(
            0.9 * ws.avg_service_time + 0.1 * service_time
        );
    }
    
    // 构造请求追踪
    RequestTrace trace;
    trace.request_id = response->request_id;
    trace.deadline = pending.deadline;
    trace.t1_client_send = pending.send_time;  // 近似
    trace.t2_lb_receive = pending.send_time;
    trace.t3_lb_dispatch = pending.send_time;  // TODO: 精确记录
    trace.t4_worker_recv = response->worker_recv_time;
    trace.t5_worker_done = response->worker_done_time;
    trace.t6_lb_response = complete_time;
    trace.t7_client_recv = complete_time;  // 近似
    trace.target_worker_id = response->worker_id;
    
    // 记录指标
    metrics_.record_request(trace);
    
    // 反馈给调度器 (用于学习)
    scheduler_->on_request_complete(trace);
    
    // 发送响应给客户端
    ClientResponse cresp;
    cresp.request_id = response->request_id;
    cresp.client_send_time = pending.send_time;
    cresp.e2e_complete_time = complete_time;
    cresp.response_size = response->response_size;
    cresp.worker_id = response->worker_id;
    cresp.deadline_met = complete_time <= pending.deadline;
    
    // TODO: 通过 eRPC 发送响应
    // rpc_->enqueue_response(pending.client_handle, &cresp, sizeof(cresp));
    
    (void)pending;
}

void LBContext::send_to_worker(uint8_t worker_id, const WorkerRequest& request) {
    // TODO: 使用 eRPC 发送请求到指定 Worker
    // rpc_->enqueue_request(worker_sessions_[worker_id], 
    //                       kWorkerRequestRPC, 
    //                       &request, sizeof(request));
    
    (void)worker_id;
    (void)request;
}

void LBContext::state_update_thread_main() {
    printf("[LB] State update thread started\n");
    
    while (running_.load(std::memory_order_relaxed)) {
        update_worker_states();
        
        // 休眠到下一个更新周期
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(config_.state_update_interval_ns)
        );
    }
    
    printf("[LB] State update thread stopped\n");
}

void LBContext::update_worker_states() {
    // TODO: 向各 Worker 发送状态查询请求
    // 收集: queue_length, slack_histogram, 健康状态
    
    // 目前使用本地估计的状态
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    Timestamp now = now_ns();
    for (auto& ws : worker_states_) {
        // 检查心跳超时
        if (now - ws.last_heartbeat > ms_to_ns(1000)) {
            // ws.is_healthy = false;
        }
        
        // 负载衰减 (如果没有收到请求，负载应该下降)
        ws.load_ema *= 0.99;
    }
}

}  // namespace malcolm
