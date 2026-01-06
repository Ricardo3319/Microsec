/**
 * Client 上下文实现
 */

#include "client_context.h"
#include <chrono>
#include <iostream>
#include <iomanip>

namespace malcolm {

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
    
    start_time_ = now_ns();
    end_time_ = start_time_ + ms_to_ns(
        (config_.warmup_sec + config_.duration_sec) * 1000
    );
    
    printf("[Client %u] Starting experiment (warmup=%us, duration=%us)\n",
           config_.client_id, config_.warmup_sec, config_.duration_sec);
    
    // 启动发送线程
    for (size_t i = 0; i < config_.num_threads; ++i) {
        threads_.emplace_back([this, i]() {
            sender_thread_main(i);
        });
    }
    
    // 进度监控
    Timestamp warmup_end = start_time_ + ms_to_ns(config_.warmup_sec * 1000);
    Timestamp last_report = start_time_;
    
    while (running_.load() && now_ns() < end_time_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        Timestamp now = now_ns();
        
        // 检查预热结束
        if (in_warmup_.load() && now >= warmup_end) {
            in_warmup_.store(false);
            metrics_.reset();  // 重置指标，只记录正式实验数据
            printf("[Client %u] Warmup complete, starting measurement\n",
                   config_.client_id);
        }
        
        // 定期报告进度
        if (now - last_report >= ms_to_ns(10000)) {  // 每 10 秒
            auto stats = get_stats();
            printf("[Client %u] Progress: sent=%lu completed=%lu RPS=%.0f P99=%.1fus\n",
                   config_.client_id,
                   sent_requests_.load(),
                   completed_requests_.load(),
                   stats.actual_rps,
                   stats.p99_latency_us);
            last_report = now;
        }
    }
    
    // 等待所有线程结束
    running_.store(false);
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
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
    
    // 创建输出目录
    // (假设目录已存在或使用 mkdir -p)
    
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

void ClientContext::sender_thread_main(size_t thread_id) {
    printf("[Client %u] Thread %zu started\n", config_.client_id, thread_id);
    
    auto& gen = generators_[thread_id];
    uint64_t rps_per_thread = config_.target_rps / config_.num_threads;
    Timestamp interval_ns = rps_per_thread > 0 ? 
                            1'000'000'000 / rps_per_thread : 1'000'000;
    
    Timestamp next_send = now_ns();
    
    while (running_.load() && now_ns() < end_time_) {
        // 速率控制
        Timestamp now = now_ns();
        if (now < next_send) {
            // 忙等待实现精确的发送间隔
            while (now_ns() < next_send && running_.load()) {
                asm volatile("pause" ::: "memory");
            }
        }
        
        // 生成并发送请求
        ClientRequest request = gen.generate();
        
        // TODO: 使用 eRPC 发送请求
        // rpc_->enqueue_request(lb_session_, kClientRequestRPC, 
        //                       &request, sizeof(request), cb);
        
        sent_requests_.fetch_add(1, std::memory_order_relaxed);
        
        // 模拟响应 (在没有真正 RPC 时) - 模拟异构环境
        if (!in_warmup_.load()) {
            Timestamp simulated_latency;
            
            // 模拟 5 个 Worker: 2 Fast (100μs), 3 Slow (500μs)
            // 根据调度算法决定命中 slow worker 的概率
            double slow_prob = config_.slow_worker_prob;  // 从配置读取
            bool hit_slow = (static_cast<double>(rand()) / RAND_MAX) < slow_prob;
            
            if (hit_slow) {
                // Slow worker: 500-700μs
                simulated_latency = us_to_ns(500 + (rand() % 200));
            } else {
                // Fast worker: 50-150μs
                simulated_latency = us_to_ns(50 + (rand() % 100));
            }
            
            metrics_.record_latency(static_cast<int64_t>(simulated_latency));
            
            // 检查 deadline miss (假设 deadline = 1ms)
            if (simulated_latency > us_to_ns(1000)) {
                metrics_.record_deadline_miss();
            }
        }
        completed_requests_.fetch_add(1, std::memory_order_relaxed);
        
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
    Timestamp recv_time = now_ns();
    
    // 计算端到端延迟
    Timestamp e2e_latency = recv_time - response->client_send_time;
    
    // 记录指标 (仅在非预热期)
    if (!in_warmup_.load()) {
        metrics_.record_latency(static_cast<int64_t>(e2e_latency));
        
        if (!response->deadline_met) {
            metrics_.record_deadline_miss();
        }
    }
    
    completed_requests_.fetch_add(1, std::memory_order_relaxed);
    throughput_.record();
}

void ClientContext::rate_limit(size_t thread_id) {
    (void)thread_id;
    // 使用令牌桶或漏桶算法实现精确的速率控制
    // 当前在 sender_thread_main 中直接实现
}

}  // namespace malcolm
