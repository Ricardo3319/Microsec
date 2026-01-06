/**
 * Load Balancer 上下文实现 - 真正的 eRPC 服务
 */

#include "lb_context.h"
#include "../scheduler/po2_scheduler.h"
#include "../scheduler/malcolm_scheduler.h"
#include "../scheduler/malcolm_strict_scheduler.h"
#include <chrono>
#include <iostream>

namespace malcolm {

// eRPC 请求上下文，存储在 heap 上直到响应返回
struct LBRequestContext {
    erpc::ReqHandle* client_handle;
    erpc::MsgBuffer req_buf;
    erpc::MsgBuffer resp_buf;
};

// 全局 LB 上下文指针
static LBContext* g_lb_ctx = nullptr;

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
    
    worker_sessions_.resize(config_.worker_addresses.size(), -1);
    
    printf("[LB] Initialized with %zu workers\n", worker_states_.size());
}

LBContext::~LBContext() {
    stop();
}

void LBContext::start() {
    if (running_.exchange(true)) {
        return;
    }
    
    g_lb_ctx = this;
    
    printf("[LB] Starting eRPC on %s...\n", config_.listen_uri.c_str());
    
    // 初始化 eRPC Nexus
    nexus_ = new erpc::Nexus(config_.listen_uri, 0, 0);
    
    // 注册客户端请求处理函数
    nexus_->register_req_func(kReqClientToLB, client_request_handler);
    
    // Session management handler
    auto sm_handler = [](int session_num, erpc::SmEventType sm_event_type,
                         erpc::SmErrType sm_err_type, void *context) {
        LBContext* lb = static_cast<LBContext*>(context);
        printf("[LB] Session %d event: %s, error: %s\n",
               session_num,
               erpc::sm_event_type_str(sm_event_type).c_str(),
               erpc::sm_err_type_str(sm_err_type).c_str());
    };
    
    // 创建 RPC 端点
    rpc_ = new erpc::Rpc<erpc::CTransport>(
        nexus_, 
        this,                           // context
        0,                              // rpc_id
        sm_handler,                     // sm_handler (必须提供)
        1                               // phy_port (10.10.1.x network)
    );
    
    // 连接到所有 Workers
    printf("[LB] Connecting to %zu workers...\n", config_.worker_addresses.size());
    for (size_t i = 0; i < config_.worker_addresses.size(); ++i) {
        std::string worker_uri = config_.worker_addresses[i];
        int session = rpc_->create_session(worker_uri, 0);
        if (session < 0) {
            fprintf(stderr, "[LB] Failed to connect to worker %zu at %s\n",
                    i, worker_uri.c_str());
        } else {
            worker_sessions_[i] = session;
            printf("[LB] Connected to worker %zu at %s (session=%d)\n",
                   i, worker_uri.c_str(), session);
        }
    }
    
    // 等待所有会话建立
    while (true) {
        bool all_connected = true;
        for (size_t i = 0; i < worker_sessions_.size(); ++i) {
            if (worker_sessions_[i] >= 0 && 
                !rpc_->is_connected(worker_sessions_[i])) {
                all_connected = false;
                break;
            }
        }
        if (all_connected) break;
        rpc_->run_event_loop_once();
    }
    printf("[LB] All workers connected\n");
    
    // 启动状态更新线程
    state_thread_ = std::thread([this]() {
        state_update_thread_main();
    });
    
    // eRPC 要求在创建 Rpc 的同一线程中调用 run_event_loop
    // 因此在主线程中运行事件循环
    printf("[LB] Running\n");
    printf("[LB] Running, press Ctrl+C to stop...\n");
    printf("[LB] RPC event loop started in main thread\n");
    
    while (running_.load()) {
        rpc_->run_event_loop_once();
    }
    
    printf("[LB] RPC event loop stopped\n");
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
    
    // 清理 eRPC
    if (rpc_) {
        delete rpc_;
        rpc_ = nullptr;
    }
    if (nexus_) {
        delete nexus_;
        nexus_ = nullptr;
    }
    
    if (!config_.metrics_output_dir.empty()) {
        export_metrics();
    }
    
    g_lb_ctx = nullptr;
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

// 静态客户端请求处理回调
void LBContext::client_request_handler(erpc::ReqHandle* req_handle, void* context) {
    auto* lb = static_cast<LBContext*>(context);
    if (!lb) lb = g_lb_ctx;
    if (!lb) return;
    
    Timestamp recv_time = now_ns();
    
    // 获取请求数据
    const erpc::MsgBuffer* req_msgbuf = req_handle->get_req_msgbuf();
    auto* request = reinterpret_cast<const RpcClientRequest*>(req_msgbuf->buf_);
    
    // 构造内部请求格式
    ClientRequest creq;
    creq.request_id = request->request_id;
    creq.client_send_time = request->client_send_time;
    creq.deadline = request->deadline;
    creq.type = static_cast<RequestType>(request->request_type);
    creq.payload_size = request->payload_size;
    
    // 调度决策
    ScheduleDecision decision;
    {
        std::lock_guard<std::mutex> lock(lb->state_mutex_);
        decision = lb->scheduler_->schedule(creq, lb->worker_states_);
    }
    
    // 记录调度延迟
    lb->scheduling_latency_.record(decision.decision_time);
    
    // 记录待处理请求
    {
        std::lock_guard<std::mutex> lock(lb->pending_mutex_);
        PendingRequest pending;
        pending.request_id = request->request_id;
        pending.send_time = request->client_send_time;
        pending.deadline = request->deadline;
        pending.client_handle = req_handle;
        pending.target_worker = decision.target_worker_id;
        lb->pending_requests_[request->request_id] = pending;
    }
    
    // 更新目标 Worker 的负载估计
    {
        std::lock_guard<std::mutex> lock(lb->state_mutex_);
        auto& ws = lb->worker_states_[decision.target_worker_id];
        ws.queue_length++;
        ws.update_load_ema(ws.queue_length);
    }
    
    // 构造发往 Worker 的请求
    int session = lb->worker_sessions_[decision.target_worker_id];
    if (session < 0) {
        fprintf(stderr, "[LB] Worker %u not connected\n", decision.target_worker_id);
        return;
    }
    
    // 分配请求和响应缓冲区 (存储在 heap 上以保持有效)
    auto* ctx = new LBRequestContext();
    ctx->client_handle = req_handle;
    ctx->req_buf = lb->rpc_->alloc_msg_buffer_or_die(sizeof(RpcWorkerRequest));
    ctx->resp_buf = lb->rpc_->alloc_msg_buffer_or_die(sizeof(RpcWorkerResponse));
    
    auto* wreq = reinterpret_cast<RpcWorkerRequest*>(ctx->req_buf.buf_);
    wreq->request_id = request->request_id;
    wreq->client_send_time = request->client_send_time;
    wreq->deadline = request->deadline;
    wreq->lb_forward_time = recv_time;
    wreq->service_time_hint = request->service_time_hint;
    wreq->worker_id = decision.target_worker_id;
    wreq->request_type = request->request_type;
    wreq->payload_size = request->payload_size;

    // 发送请求到 Worker
    lb->rpc_->enqueue_request(
        session, 
        kReqLBToWorker, 
        &ctx->req_buf, 
        &ctx->resp_buf,
        worker_response_callback, 
        ctx
    );
}

// 静态 Worker 响应回调
void LBContext::worker_response_callback(void* context, void* tag) {
    auto* lb = static_cast<LBContext*>(context);
    if (!lb) lb = g_lb_ctx;
    if (!lb) return;
    
    auto* ctx = static_cast<LBRequestContext*>(tag);
    erpc::ReqHandle* client_handle = ctx->client_handle;
    erpc::MsgBuffer& worker_resp_buf = ctx->resp_buf;
    
    Timestamp complete_time = now_ns();
    
    // 解析 Worker 响应
    auto* wresp = reinterpret_cast<const RpcWorkerResponse*>(worker_resp_buf.buf_);
    
    // 获取待处理请求信息
    PendingRequest pending;
    {
        std::lock_guard<std::mutex> lock(lb->pending_mutex_);
        auto it = lb->pending_requests_.find(wresp->request_id);
        if (it == lb->pending_requests_.end()) {
            fprintf(stderr, "[LB] Unknown response for request %lu\n", 
                    wresp->request_id);
            lb->rpc_->free_msg_buffer(ctx->req_buf);
            lb->rpc_->free_msg_buffer(ctx->resp_buf);
            delete ctx;
            return;
        }
        pending = it->second;
        lb->pending_requests_.erase(it);
    }
    
    // 更新 Worker 状态
    {
        std::lock_guard<std::mutex> lock(lb->state_mutex_);
        auto& ws = lb->worker_states_[wresp->worker_id];
        if (ws.queue_length > 0) {
            ws.queue_length--;
        }
        ws.update_load_ema(ws.queue_length);
        
        // 更新服务时间统计
        Timestamp service_time = us_to_ns(wresp->service_time_us);
        ws.avg_service_time = static_cast<Timestamp>(
            0.9 * ws.avg_service_time + 0.1 * service_time
        );
    }
    
    // 构造请求追踪
    RequestTrace trace;
    trace.request_id = wresp->request_id;
    trace.deadline = pending.deadline;
    trace.t1_client_send = pending.send_time;
    trace.t4_worker_recv = wresp->worker_recv_time;
    trace.t5_worker_done = wresp->worker_done_time;
    trace.t6_lb_response = complete_time;
    trace.target_worker_id = wresp->worker_id;
    
    // 记录指标
    lb->metrics_.record_request(trace);
    
    // 反馈给调度器 (用于学习)
    lb->scheduler_->on_request_complete(trace);
    
    // 构造客户端响应
    erpc::MsgBuffer& client_resp_buf = client_handle->pre_resp_msgbuf_;
    lb->rpc_->resize_msg_buffer(&client_resp_buf, sizeof(RpcClientResponse));
    
    auto* cresp = reinterpret_cast<RpcClientResponse*>(client_resp_buf.buf_);
    cresp->request_id = wresp->request_id;
    cresp->client_send_time = pending.send_time;
    cresp->e2e_latency_ns = complete_time - pending.send_time;
    cresp->service_time_us = wresp->service_time_us;
    cresp->worker_id = wresp->worker_id;
    cresp->deadline_met = complete_time <= pending.deadline ? 1 : 0;
    cresp->success = wresp->success;
    
    // 发送响应给客户端
    lb->rpc_->enqueue_response(client_handle, &client_resp_buf);
    
    // 清理请求上下文
    lb->rpc_->free_msg_buffer(ctx->req_buf);
    lb->rpc_->free_msg_buffer(ctx->resp_buf);
    delete ctx;
}

void LBContext::export_metrics() {
    if (config_.metrics_output_dir.empty()) return;
    
    metrics_.export_all(config_.metrics_output_dir);
    scheduling_latency_.export_hdr(config_.metrics_output_dir + "/scheduling_latency.hdr");
    
    printf("[LB] Metrics exported to %s\n", config_.metrics_output_dir.c_str());
}

void LBContext::handle_client_request(void* req_handle, const ClientRequest* request) {
    (void)req_handle;
    (void)request;
    // 已移到静态 client_request_handler
}

void LBContext::handle_worker_response(const WorkerResponse* response) {
    (void)response;
    // 已移到静态 worker_response_callback
}

void LBContext::send_to_worker(uint8_t worker_id, const WorkerRequest& request) {
    (void)worker_id;
    (void)request;
    // 已在 client_request_handler 中实现
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
