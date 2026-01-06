#pragma once

/**
 * 指标收集模块
 * 
 * 使用 HdrHistogram 进行高精度延迟分布收集
 * 支持并发安全的指标记录和导出
 */

#include <hdr/hdr_histogram.h>
#include <atomic>
#include <mutex>
#include <string>
#include <fstream>
#include <cstdio>
#include <memory>
#include <array>
#include "types.h"

namespace malcolm {

/**
 * 延迟直方图封装
 * 
 * 基于 HdrHistogram 实现的高精度延迟分布收集
 * 特点: 对数压缩存储，内存占用约 1-2KB，精度 3 位有效数字
 */
class LatencyHistogram {
public:
    /**
     * @param lowest_trackable  最小可追踪值 (ns)
     * @param highest_trackable 最大可追踪值 (ns)
     * @param significant_figures 有效数字位数 (1-5)
     */
    explicit LatencyHistogram(
        int64_t lowest_trackable = 1,
        int64_t highest_trackable = 10'000'000'000LL,  // 10 秒
        int significant_figures = 3
    ) {
        int ret = hdr_init(lowest_trackable, highest_trackable, 
                          significant_figures, &hist_);
        if (ret != 0) {
            throw std::runtime_error("Failed to initialize HdrHistogram");
        }
    }
    
    ~LatencyHistogram() {
        if (hist_) {
            hdr_close(hist_);
        }
    }
    
    // 禁用拷贝
    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;
    
    // 允许移动
    LatencyHistogram(LatencyHistogram&& other) noexcept : hist_(other.hist_) {
        other.hist_ = nullptr;
    }
    
    LatencyHistogram& operator=(LatencyHistogram&& other) noexcept {
        if (this != &other) {
            if (hist_) hdr_close(hist_);
            hist_ = other.hist_;
            other.hist_ = nullptr;
        }
        return *this;
    }
    
    /// 记录一个延迟值 (纳秒)
    void record(int64_t value_ns) {
        hdr_record_value(hist_, value_ns);
    }
    
    /// 记录多个相同值
    void record_count(int64_t value_ns, int64_t count) {
        hdr_record_values(hist_, value_ns, count);
    }
    
    /// 获取指定百分位的值 (返回纳秒)
    int64_t percentile(double p) const {
        return hdr_value_at_percentile(hist_, p);
    }
    
    /// 获取总样本数
    int64_t total_count() const {
        return hist_->total_count;
    }
    
    /// 获取最小值
    int64_t min() const {
        return hdr_min(hist_);
    }
    
    /// 获取最大值
    int64_t max() const {
        return hdr_max(hist_);
    }
    
    /// 获取平均值
    double mean() const {
        return hdr_mean(hist_);
    }
    
    /// 获取标准差
    double stddev() const {
        return hdr_stddev(hist_);
    }
    
    /// 重置直方图
    void reset() {
        hdr_reset(hist_);
    }
    
    /// 合并另一个直方图
    void merge_from(const LatencyHistogram& other) {
        hdr_add(hist_, other.hist_);
    }
    
    /// 打印摘要信息到标准输出
    void print_summary(const std::string& name) const {
        printf("[%s] count=%ld mean=%.2fus P50=%.2fus P99=%.2fus P99.9=%.2fus P99.99=%.2fus max=%.2fus\n",
               name.c_str(),
               total_count(),
               mean() / 1000.0,
               percentile(50.0) / 1000.0,
               percentile(99.0) / 1000.0,
               percentile(99.9) / 1000.0,
               percentile(99.99) / 1000.0,
               static_cast<double>(max()) / 1000.0);
    }
    
    /// 导出为 HDR 格式文件 (可用于后续合并和分析)
    bool export_hdr(const std::string& path) const {
        FILE* fp = fopen(path.c_str(), "w");
        if (!fp) return false;
        
        hdr_percentiles_print(hist_, fp, 5, 1.0, CLASSIC);
        fclose(fp);
        return true;
    }
    
    /// 导出 CDF 数据为 CSV 格式 (用于绘图)
    bool export_cdf(const std::string& path, int num_points = 10000) const {
        std::ofstream out(path);
        if (!out) return false;
        
        out << "percentile,latency_ns,latency_us\n";
        for (int i = 0; i <= num_points; ++i) {
            double p = 100.0 * i / num_points;
            int64_t val = percentile(p);
            out << p << "," << val << "," << (val / 1000.0) << "\n";
        }
        return true;
    }
    
private:
    hdr_histogram* hist_ = nullptr;
};

/**
 * 线程安全的指标收集器
 * 
 * 收集端到端延迟、截止时间违约率等核心指标
 */
class MetricsCollector {
public:
    static constexpr size_t kMaxWorkers = 16;
    
    MetricsCollector() = default;
    
    /// 获取全局单例
    static MetricsCollector& instance() {
        static MetricsCollector inst;
        return inst;
    }
    
    /// 记录一个完成的请求
    void record_request(const RequestTrace& trace) {
        e2e_latency_.record(trace.e2e_latency_ns());
        
        if (trace.is_deadline_miss()) {
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        }
        total_requests_.fetch_add(1, std::memory_order_relaxed);
        
        // 记录 LB 开销
        lb_overhead_.record(trace.lb_overhead_ns());
        
        // 按 Worker 分类统计
        if (trace.target_worker_id < kMaxWorkers) {
            per_worker_latency_[trace.target_worker_id].record(trace.e2e_latency_ns());
        }
    }
    
    /// 记录单个延迟值 (简化接口)
    void record_latency(int64_t latency_ns) {
        e2e_latency_.record(latency_ns);
        total_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    
    /// 记录截止时间违约
    void record_deadline_miss() {
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    
    /// 获取截止时间违约率
    double deadline_miss_rate() const {
        uint64_t total = total_requests_.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        uint64_t misses = deadline_misses_.load(std::memory_order_relaxed);
        return static_cast<double>(misses) / total;
    }
    
    /// 获取总请求数
    uint64_t total_requests() const {
        return total_requests_.load(std::memory_order_relaxed);
    }
    
    /// 获取违约数
    uint64_t deadline_misses() const {
        return deadline_misses_.load(std::memory_order_relaxed);
    }
    
    /// 获取端到端延迟直方图的只读引用
    const LatencyHistogram& e2e_latency() const { return e2e_latency_; }
    
    /// 获取 LB 开销直方图的只读引用
    const LatencyHistogram& lb_overhead() const { return lb_overhead_; }
    
    /// 获取指定 Worker 的延迟直方图
    const LatencyHistogram& worker_latency(size_t worker_id) const {
        return per_worker_latency_[worker_id % kMaxWorkers];
    }
    
    /// 重置所有指标
    void reset() {
        e2e_latency_.reset();
        lb_overhead_.reset();
        for (auto& h : per_worker_latency_) {
            h.reset();
        }
        total_requests_.store(0, std::memory_order_relaxed);
        deadline_misses_.store(0, std::memory_order_relaxed);
    }
    
    /// 打印摘要
    void print_summary() const {
        printf("\n========== Metrics Summary ==========\n");
        printf("Total Requests: %lu\n", total_requests());
        printf("Deadline Misses: %lu (%.4f%%)\n", 
               deadline_misses(), deadline_miss_rate() * 100);
        e2e_latency_.print_summary("E2E Latency");
        lb_overhead_.print_summary("LB Overhead");
        printf("=====================================\n");
    }
    
    /// 导出所有指标到目录
    bool export_all(const std::string& dir) {
        bool success = true;
        
        success &= e2e_latency_.export_hdr(dir + "/e2e_latency.hdr");
        success &= e2e_latency_.export_cdf(dir + "/e2e_latency_cdf.csv");
        success &= lb_overhead_.export_hdr(dir + "/lb_overhead.hdr");
        
        // 导出摘要
        std::ofstream summary(dir + "/summary.txt");
        if (summary) {
            summary << "Total Requests: " << total_requests() << "\n";
            summary << "Deadline Misses: " << deadline_misses() << "\n";
            summary << "Deadline Miss Rate: " << (deadline_miss_rate() * 100) << "%\n";
            summary << "P50 Latency (us): " << e2e_latency_.percentile(50.0) / 1000.0 << "\n";
            summary << "P99 Latency (us): " << e2e_latency_.percentile(99.0) / 1000.0 << "\n";
            summary << "P99.9 Latency (us): " << e2e_latency_.percentile(99.9) / 1000.0 << "\n";
            summary << "P99.99 Latency (us): " << e2e_latency_.percentile(99.99) / 1000.0 << "\n";
        }
        
        // 导出每个 Worker 的统计
        for (size_t i = 0; i < kMaxWorkers; ++i) {
            if (per_worker_latency_[i].total_count() > 0) {
                per_worker_latency_[i].export_cdf(
                    dir + "/worker_" + std::to_string(i) + "_latency_cdf.csv");
            }
        }
        
        return success;
    }
    
private:
    LatencyHistogram e2e_latency_;
    LatencyHistogram lb_overhead_;
    std::array<LatencyHistogram, kMaxWorkers> per_worker_latency_;
    
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> deadline_misses_{0};
};

/**
 * 吞吐量计数器
 * 
 * 使用滑动窗口计算 RPS
 */
class ThroughputCounter {
public:
    static constexpr size_t kWindowSize = 10;  // 10 个时间桶
    static constexpr uint64_t kBucketDurationNs = 100'000'000;  // 100ms 每桶
    
    ThroughputCounter() {
        for (auto& c : buckets_) c.store(0);
    }
    
    /// 记录一个请求完成
    void record() {
        uint64_t now = now_ns();
        size_t bucket = (now / kBucketDurationNs) % kWindowSize;
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        
        // 清理旧桶
        size_t old_bucket = (bucket + 1) % kWindowSize;
        if (last_bucket_ != bucket) {
            buckets_[old_bucket].store(0, std::memory_order_relaxed);
            last_bucket_ = bucket;
        }
    }
    
    /// 获取当前 RPS
    double get_rps() const {
        uint64_t total = 0;
        for (const auto& c : buckets_) {
            total += c.load(std::memory_order_relaxed);
        }
        // 窗口时长 = kWindowSize * kBucketDurationNs / 1e9 秒
        double window_sec = static_cast<double>(kWindowSize * kBucketDurationNs) / 1e9;
        return static_cast<double>(total) / window_sec;
    }
    
private:
    std::array<std::atomic<uint64_t>, kWindowSize> buckets_;
    size_t last_bucket_ = 0;
};

}  // namespace malcolm
