#pragma once

/**
 * EDF (Earliest Deadline First) 优先队列
 * 
 * 用于 Worker 节点内部的任务调度
 * 支持多种实现策略以适应不同负载场景
 */

#include <queue>
#include <mutex>
#include <atomic>
#include <vector>
#include <array>
#include <algorithm>
#include <optional>
#include "../common/types.h"

namespace malcolm {

/**
 * 任务结构 (用于节点内调度)
 */
struct Task {
    uint64_t request_id;
    Timestamp deadline;          // 绝对截止时间
    Timestamp arrival_time;      // 到达时间
    Timestamp client_send_time;  // 客户端发送时间 (用于响应)
    uint32_t service_time_hint;  // 服务时间提示 (μs)
    RequestType type;
    size_t payload_size;
    void* request_msg;           // eRPC 请求消息指针
    void* request_handle;        // eRPC 请求句柄
    
    // EDF 比较: 截止时间越早优先级越高
    bool operator>(const Task& other) const {
        return deadline > other.deadline;
    }
    
    bool operator<(const Task& other) const {
        return deadline < other.deadline;
    }
    
    // 计算当前松弛时间
    Duration slack_time(Timestamp now) const {
        return static_cast<Duration>(deadline - now);
    }
    
    // 是否已过期
    bool is_expired(Timestamp now) const {
        return deadline <= now;
    }
};

/**
 * 方案 A: 锁保护的标准堆
 * 
 * 适用场景: 中等负载 (< 100K RPS/Worker)
 * 优点: 实现简单，稳定可靠
 * 缺点: 锁竞争在高并发下成为瓶颈
 */
class EDFQueueLocked {
public:
    EDFQueueLocked() = default;
    
    /// 入队
    void push(Task&& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        heap_.push(std::move(task));
    }
    
    /// 尝试出队 (非阻塞)
    bool try_pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heap_.empty()) return false;
        
        task = std::move(const_cast<Task&>(heap_.top()));
        heap_.pop();
        return true;
    }
    
    /// 查看队首元素 (不出队)
    std::optional<Task> peek() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heap_.empty()) return std::nullopt;
        return heap_.top();
    }
    
    /// 队列长度
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_.size();
    }
    
    /// 是否为空
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_.empty();
    }
    
    /// 清空队列
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!heap_.empty()) heap_.pop();
    }
    
    /// 获取所有已过期任务 (deadline < now)
    std::vector<Task> get_expired(Timestamp now) {
        std::vector<Task> expired;
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 由于堆顶是最早 deadline，持续弹出直到遇到未过期任务
        while (!heap_.empty() && heap_.top().deadline <= now) {
            expired.push_back(std::move(const_cast<Task&>(heap_.top())));
            heap_.pop();
        }
        return expired;
    }
    
private:
    // 最小堆 (deadline 最小的在顶部)
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> heap_;
    mutable std::mutex mutex_;
};

/**
 * 方案 B: 分层时间轮 (Hierarchical Timing Wheel)
 * 
 * 适用场景: 超高吞吐 (> 500K RPS/Worker)
 * 优点: O(1) 入队，分桶减少锁竞争
 * 缺点: 精度受桶宽度限制，需要定期扫描
 * 
 * 设计:
 * - 将 deadline 按时间桶分组
 * - 每个桶内按到达顺序 (近似 EDF，因为同桶内 deadline 接近)
 * - 工作线程优先从当前/过期桶获取任务
 */
class HierarchicalTimingWheel {
public:
    static constexpr size_t kNumBuckets = 1024;         // 2^10 个桶
    static constexpr Timestamp kBucketWidthNs = 1000;   // 1μs 每桶
    // 总覆盖范围: 1024 * 1μs = ~1ms
    
    HierarchicalTimingWheel() {
        current_tick_.store(now_ns() / kBucketWidthNs);
    }
    
    /// 入队
    void insert(Task&& task) {
        size_t bucket_idx = (task.deadline / kBucketWidthNs) % kNumBuckets;
        auto& bucket = buckets_[bucket_idx];
        
        std::lock_guard<std::mutex> lock(bucket.mutex);
        bucket.tasks.push_back(std::move(task));
        total_size_.fetch_add(1, std::memory_order_relaxed);
    }
    
    /// 获取最紧急的到期/即将到期任务
    bool try_get_urgent(Timestamp now, Task& out_task) {
        size_t current_bucket = (now / kBucketWidthNs) % kNumBuckets;
        
        // 扫描当前及之前的几个桶 (处理过期任务)
        for (size_t offset = 0; offset < kNumBuckets / 8; ++offset) {
            size_t idx = (current_bucket - offset + kNumBuckets) % kNumBuckets;
            auto& bucket = buckets_[idx];
            
            std::lock_guard<std::mutex> lock(bucket.mutex);
            if (!bucket.tasks.empty()) {
                // 找到桶内 deadline 最早的任务
                auto it = std::min_element(bucket.tasks.begin(), bucket.tasks.end());
                
                out_task = std::move(*it);
                bucket.tasks.erase(it);
                total_size_.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    
    /// 获取下一个任务 (不考虑是否过期)
    bool try_pop(Task& out_task) {
        Timestamp now = now_ns();
        return try_get_urgent(now, out_task);
    }
    
    /// 总任务数
    size_t size() const {
        return total_size_.load(std::memory_order_relaxed);
    }
    
    bool empty() const {
        return size() == 0;
    }
    
    /// 获取松弛时间分布直方图 (用于状态向量)
    void get_slack_histogram(Timestamp now, 
                             std::array<uint32_t, constants::kSlackHistogramBins>& hist) const {
        hist.fill(0);
        
        for (const auto& bucket : buckets_) {
            std::lock_guard<std::mutex> lock(bucket.mutex);
            for (const auto& task : bucket.tasks) {
                Duration slack = task.slack_time(now);
                
                // 将松弛时间映射到直方图桶
                // 负值 (过期) -> 桶 0
                // 0 ~ kSlackBinWidth -> 桶 1
                // ...
                size_t bin = 0;
                if (slack > 0) {
                    bin = std::min<size_t>(
                        static_cast<size_t>(slack / constants::kSlackBinWidth) + 1,
                        constants::kSlackHistogramBins - 1
                    );
                }
                ++hist[bin];
            }
        }
    }
    
private:
    struct Bucket {
        mutable std::mutex mutex;
        std::vector<Task> tasks;
    };
    
    std::array<Bucket, kNumBuckets> buckets_;
    std::atomic<size_t> total_size_{0};
    std::atomic<uint64_t> current_tick_;
};

/**
 * EDF 队列统一接口
 * 
 * 根据编译选项选择具体实现
 */
class EDFQueue {
public:
    enum class Implementation {
        kLocked,      // 锁保护堆
        kTimingWheel  // 时间轮
    };
    
    explicit EDFQueue(Implementation impl = Implementation::kLocked)
        : impl_(impl) {}
    
    void push(Task&& task) {
        switch (impl_) {
            case Implementation::kLocked:
                locked_queue_.push(std::move(task));
                break;
            case Implementation::kTimingWheel:
                timing_wheel_.insert(std::move(task));
                break;
        }
    }
    
    bool try_pop(Task& task) {
        switch (impl_) {
            case Implementation::kLocked:
                return locked_queue_.try_pop(task);
            case Implementation::kTimingWheel:
                return timing_wheel_.try_pop(task);
        }
        return false;
    }
    
    size_t size() const {
        switch (impl_) {
            case Implementation::kLocked:
                return locked_queue_.size();
            case Implementation::kTimingWheel:
                return timing_wheel_.size();
        }
        return 0;
    }
    
    bool empty() const { return size() == 0; }
    
    /// 获取松弛时间直方图
    void get_slack_histogram(
        std::array<uint32_t, constants::kSlackHistogramBins>& hist) const {
        Timestamp now = now_ns();
        timing_wheel_.get_slack_histogram(now, hist);
    }
    
private:
    Implementation impl_;
    EDFQueueLocked locked_queue_;
    HierarchicalTimingWheel timing_wheel_;
};

}  // namespace malcolm
