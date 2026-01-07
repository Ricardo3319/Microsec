/**
 * Client 上下文实现 - 真正的 eRPC 客户端
 */

#include "client_context.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace malcolm {

// 获取 10.10.1.x 网络的本地 IP
static std::string get_local_ip() {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        return "0.0.0.0";
    }
    std::string result = "0.0.0.0";
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, addr, sizeof(addr));
            std::string ip(addr);
            if (ip.rfind("10.10.1.", 0) == 0) {
                result = ip;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return result;
}

// 全局客户端上下文
static ClientContext* g_client_ctx = nullptr;

ClientContext::ClientContext(const ClientConfig& config)
    : config_(config) {
    
    // 为每个线程创建独立的请求生成器
    generators_.reserve(config_.num_threads);
    for (size_t i = 0; i < config_.num_threads; ++i) {
        RequestGenerator gen(config_.workload);
        gen.set_seed(config_.client_id * 1000 + i);  // 确保可重复
        generators_.push_back(std::move(gen));
    }
    
    printf("[Client %u] Initialized with %zu threads, target RPS=%lu\n",
           config_.client_id, config_.num_threads, config_.target_rps);
}

ClientContext::~ClientContext() {
    stop();
}

void ClientContext::run() {
    if (running_.exchange(true)) {
        return;
    }
    
    g_client_ctx = this;
    
    // 初始化 eRPC
    // eRPC 要求端口在 31850-31881 范围内
    // Client 使用 31870 + client_id 避免与 Worker (31850-31859) 和 LB (31860-31869) 冲突
    uint16_t client_port = 31870 + config_.client_id;
    std::string local_ip = get_local_ip();
    std::string local_uri = local_ip + ":" + std::to_string(client_port);
    printf("[Client %u] Using eRPC port %u, local IP %s\n", config_.client_id, client_port, local_ip.c_str());
    nexus_ = new erpc::Nexus(local_uri, 0, 0);
    
    // Session management handler
    auto sm_handler = [](int session_num, erpc::SmEventType sm_event_type,
                         erpc::SmErrType sm_err_type, void *context) {
        ClientContext* client = static_cast<ClientContext*>(context);
        printf("[Client %u] Session %d event: %s, error: %s\n",
               client->config_.client_id, session_num,
               erpc::sm_event_type_str(sm_event_type).c_str(),
               erpc::sm_err_type_str(sm_err_type).c_str());
    };
    
    rpc_ = new erpc::Rpc<erpc::CTransport>(
        nexus_,
        this,
        0,           // rpc_id  
        sm_handler,  // sm_handler (必须提供)
        1            // phy_port (10.10.1.x network)
    );
    
    // 连接到 Load Balancer
    printf("[Client %u] Connecting to LB at %s...\n", 
           config_.client_id, config_.lb_address.c_str());
    lb_session_ = rpc_->create_session(config_.lb_address, 0);
    if (lb_session_ < 0) {
        fprintf(stderr, "[Client %u] Failed to connect to LB\n", config_.client_id);
        return;
    }
    
    // 等待连接建立
    while (!rpc_->is_connected(lb_session_)) {
        rpc_->run_event_loop_once();
    }
    printf("[Client %u] Connected to LB (session=%d)\n", 
           config_.client_id, lb_session_);
    
    // 预分配请求/响应缓冲区
    size_t buf_pool_size = config_.num_threads * 1000;  // 每线程 1000 个
    req_bufs_.resize(buf_pool_size);
    resp_bufs_.resize(buf_pool_size);
    for (size_t i = 0; i < buf_pool_size; ++i) {
        req_bufs_[i] = rpc_->alloc_msg_buffer_or_die(sizeof(RpcClientRequest));
        resp_bufs_[i] = rpc_->alloc_msg_buffer_or_die(sizeof(RpcClientResponse));
    }
    // [NEW] 初始化本地 deadline 数组
    req_deadlines_.resize(buf_pool_size, 0);
    
    start_time_ = now_ns();
    end_time_ = start_time_ + ms_to_ns(
        (config_.warmup_sec + config_.duration_sec) * 1000
    );
    
    printf("[Client %u] Starting experiment (warmup=%us, duration=%us)\n",
           config_.client_id, config_.warmup_sec, config_.duration_sec);
    
    // eRPC 要求所有 RPC 操作在创建 Rpc 的同一线程中执行
    // 因此在主线程中同时运行事件循环和发送请求
    printf("[Client %u] Running in single-threaded mode for eRPC compatibility\n",
           config_.client_id);
    
    auto& gen = generators_[0];
    uint64_t rps = config_.target_rps;
    Timestamp interval_ns = rps > 0 ? 1'000'000'000 / rps : 1'000'000;
    Timestamp next_send = now_ns();
    uint64_t local_req_id = 0;
    
    // 进度监控变量
    Timestamp warmup_end = start_time_ + ms_to_ns(config_.warmup_sec * 1000);
    Timestamp last_report = start_time_;
    
    printf("[Client %u] Starting main loop (interval=%lu ns)\n", 
           config_.client_id, interval_ns);
    
    while (running_.load() && now_ns() < end_time_) {
        Timestamp now = now_ns();
        
        // 运行 eRPC 事件循环处理响应 (需要多次调用以处理所有待处理事件)
        // 增加到 200 次以确保响应被及时处理，尤其是当 inflight 较高时
        for (int i = 0; i < 200; ++i) {
            rpc_->run_event_loop_once();
        }
        
        // 检查预热结束
        if (in_warmup_.load() && now >= warmup_end) {
            in_warmup_.store(false);
            metrics_.reset();
            printf("[Client %u] Warmup complete, starting measurement\n",
                   config_.client_id);
        }
        
        // 定期报告进度
        if (now - last_report >= ms_to_ns(5000)) {  // 每 5 秒
            auto stats = get_stats();
            printf("[Client %u] Progress: sent=%lu completed=%lu inflight=%zu RPS=%.0f P99=%.1fus\n",
                   config_.client_id,
                   sent_requests_.load(),
                   completed_requests_.load(),
                   inflight_requests_.load(),
                   stats.actual_rps,
                   stats.p99_latency_us);
            last_report = now;
        }
        
        // 发送请求 (如果到了发送时间且有可用槽位)
        // 关键：不能让 inflight 超过缓冲区数量，否则缓冲区会被覆盖
        size_t available_bufs = req_bufs_.size() - inflight_requests_.load();
        if (now >= next_send && inflight_requests_.load() < kMaxInflight && available_bufs > 0) {
            // 生成请求
            ClientRequest creq = gen.generate();
            creq.request_id = local_req_id++;
            creq.client_send_time = now_ns();
            
            // 获取缓冲区 (使用 local_req_id 循环分配，确保不与在途请求冲突)
            // 只要 inflight < buf_size，就不会有冲突
            size_t idx = local_req_id % req_bufs_.size();
            erpc::MsgBuffer& req_buf = req_bufs_[idx];
            erpc::MsgBuffer& resp_buf = resp_bufs_[idx];
            
            // 填充 RPC 请求
            auto* rpc_req = reinterpret_cast<RpcClientRequest*>(req_buf.buf_);
            rpc_req->request_id = creq.request_id;
            rpc_req->client_send_time = creq.client_send_time;
            rpc_req->deadline = creq.deadline;
            // [FIX] 使用生成器生成的原始服务时间，不基于 deadline 计算
            rpc_req->service_time_hint = creq.expected_service_us;
            rpc_req->client_id = config_.client_id;
            rpc_req->request_type = static_cast<uint8_t>(creq.type);
            rpc_req->payload_size = creq.payload_size;
            
            // 记录本 slot 的 Deadline (Client 时钟域)
            req_deadlines_[idx] = creq.deadline;

            // 增加在途请求计数
            inflight_requests_.fetch_add(1, std::memory_order_relaxed);

            // 发送请求，tag 传递 idx
            rpc_->enqueue_request(
                lb_session_,
                kReqClientToLB,
                &req_buf,
                &resp_buf,
                response_callback,
                reinterpret_cast<void*>(idx)
            );

            sent_requests_.fetch_add(1, std::memory_order_relaxed);
            
            // 更新下次发送时间
            next_send += interval_ns;
            if (next_send < now) {
                next_send = now;  // 防止累积
            }
        }
    }
    
    printf("[Client %u] Main loop ended\n", config_.client_id);
    
    // 打印最终结果
    auto stats = get_stats();
    printf("\n[Client %u] Experiment Complete\n", config_.client_id);
    printf("  Total Requests:  %lu\n", stats.total_requests);
    printf("  Completed:       %lu\n", stats.successful_requests);
    printf("  Deadline Misses: %lu (%.4f%%)\n", 
           stats.deadline_misses,
           100.0 * stats.deadline_misses / std::max(stats.total_requests, 1UL));
    printf("  Actual RPS:      %.0f\n", stats.actual_rps);
    printf("  P50 Latency:     %.2f us\n", stats.p50_latency_us);
    printf("  P99 Latency:     %.2f us\n", stats.p99_latency_us);
    printf("  P99.9 Latency:   %.2f us\n", stats.p999_latency_us);
    
    // 导出结果
    if (!config_.output_dir.empty()) {
        export_results();
    }
    
    // 清理 eRPC
    for (auto& buf : req_bufs_) {
        rpc_->free_msg_buffer(buf);
    }
    for (auto& buf : resp_bufs_) {
        rpc_->free_msg_buffer(buf);
    }
    
    if (rpc_) {
        delete rpc_;
        rpc_ = nullptr;
    }
    if (nexus_) {
        delete nexus_;
        nexus_ = nullptr;
    }
    
    g_client_ctx = nullptr;
}

void ClientContext::stop() {
    running_.store(false);
    
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ClientContext::export_results() {
    if (config_.output_dir.empty()) return;
    
    metrics_.export_all(config_.output_dir);
    printf("[Client %u] Results exported to %s\n", 
           config_.client_id, config_.output_dir.c_str());
}

ClientContext::Stats ClientContext::get_stats() const {
    Stats stats;
    
    stats.total_requests = sent_requests_.load();
    stats.successful_requests = completed_requests_.load();
    stats.deadline_misses = metrics_.deadline_misses();
    
    // 计算实际 RPS
    Timestamp now = now_ns();
    Timestamp elapsed = now - start_time_;
    if (elapsed > 0) {
        stats.actual_rps = static_cast<double>(stats.successful_requests) * 1e9 / elapsed;
    } else {
        stats.actual_rps = 0;
    }
    
    // 百分位延迟
    stats.p50_latency_us = ns_to_us(metrics_.e2e_latency().percentile(50.0));
    stats.p99_latency_us = ns_to_us(metrics_.e2e_latency().percentile(99.0));
    stats.p999_latency_us = ns_to_us(metrics_.e2e_latency().percentile(99.9));
    
    return stats;
}

// 响应回调
void ClientContext::response_callback(void* context, void* tag) {
    auto* client = static_cast<ClientContext*>(context);
    if (!client) client = g_client_ctx;
    if (!client) return;
    
    Timestamp recv_time = now_ns();
    
    // 从 tag 恢复 index (发送时传入)
    size_t idx = reinterpret_cast<size_t>(tag);
    if (idx >= client->resp_bufs_.size()) return; // 防御性检查

    auto* response = reinterpret_cast<const RpcClientResponse*>(client->resp_bufs_[idx].buf_);

    // 计算端到端延迟
    Timestamp e2e_latency = recv_time - response->client_send_time;
    
    // 记录指标 (仅在非预热期)
    if (!client->in_warmup_.load()) {
        client->metrics_.record_latency(static_cast<int64_t>(e2e_latency));
        
        // 使用本地记录的 deadline 进行判定 (客户端时钟域)
        Timestamp original_deadline = client->req_deadlines_[idx];
        bool actual_deadline_met = (recv_time <= original_deadline);
        if (!actual_deadline_met) {
            client->metrics_.record_deadline_miss();
        }
    }
    
    // 减少在途请求计数
    client->inflight_requests_.fetch_sub(1, std::memory_order_relaxed);
    client->completed_requests_.fetch_add(1, std::memory_order_relaxed);
    client->throughput_.record();
}

void ClientContext::sender_thread_main(size_t thread_id) {
    printf("[Client %u] Thread %zu started\n", config_.client_id, thread_id);
    
    auto& gen = generators_[thread_id];
    uint64_t rps_per_thread = config_.target_rps / config_.num_threads;
    Timestamp interval_ns = rps_per_thread > 0 ? 
                            1'000'000'000 / rps_per_thread : 1'000'000;
    
    Timestamp next_send = now_ns();
    uint64_t local_req_id = thread_id * 1000000000ULL;  // 每线程独立 ID 空间
    
    while (running_.load() && now_ns() < end_time_) {
        // 速率控制
        Timestamp now = now_ns();
        if (now < next_send) {
            // 忙等待实现精确的发送间隔
            while (now_ns() < next_send && running_.load()) {
                asm volatile("pause" ::: "memory");
            }
        }
        
        // 生成请求
        ClientRequest creq = gen.generate();
        creq.request_id = local_req_id++;
        creq.client_send_time = now_ns();
        
        // 获取缓冲区
        size_t idx = buf_idx_.fetch_add(1) % req_bufs_.size();
        erpc::MsgBuffer& req_buf = req_bufs_[idx];
        erpc::MsgBuffer& resp_buf = resp_bufs_[idx];
        
        // 填充 RPC 请求
        auto* rpc_req = reinterpret_cast<RpcClientRequest*>(req_buf.buf_);
        rpc_req->request_id = creq.request_id;
        rpc_req->client_send_time = creq.client_send_time;
        rpc_req->deadline = creq.deadline;
        // [FIX] 使用生成器生成的原始服务时间，不基于 deadline 计算
        rpc_req->service_time_hint = creq.expected_service_us;
        rpc_req->client_id = config_.client_id;
        rpc_req->request_type = static_cast<uint8_t>(creq.type);
        rpc_req->payload_size = creq.payload_size;
        
        // 记录本 slot 的 Deadline (Client 时钟域)
        req_deadlines_[idx] = creq.deadline;

        // 发送请求 (tag = idx)
        rpc_->enqueue_request(
            lb_session_,
            kReqClientToLB,
            &req_buf,
            &resp_buf,
            response_callback,
            reinterpret_cast<void*>(idx)
        );

        sent_requests_.fetch_add(1, std::memory_order_relaxed);
        
        // 更新下次发送时间
        next_send += interval_ns;
        
        // 防止累积延迟
        if (next_send < now_ns()) {
            next_send = now_ns();
        }
    }
    
    printf("[Client %u] Thread %zu stopped\n", config_.client_id, thread_id);
}

void ClientContext::handle_response(const ClientResponse* response) {
    (void)response;
    // 已移到静态 response_callback
}

void ClientContext::rate_limit(size_t thread_id) {
    (void)thread_id;
    // 在 sender_thread_main 中直接实现
}

}  // namespace malcolm
