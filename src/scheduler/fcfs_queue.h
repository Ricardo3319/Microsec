#pragma once

/**
 * FCFS (First-Come-First-Served) 队列
 * 
 * Baseline 调度策略，用于 Power-of-2 和 Original Malcolm
 * 使用无锁 SPSC/MPMC 队列实现高吞吐
 */

#include <queue>
#include <mutex>
#include <atomic>
#include <optional>
#include "../common/types.h"
#include "edf_queue.h"  // 复用 Task 结构

namespace malcolm {

/**
 * 简单的锁保护 FCFS 队列
 * 
 * 对于 Baseline 实验足够使用
 */
class FCFSQueueLocked {
public:
    FCFSQueueLocked() = default;
    
    /// 入队
    void push(Task&& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(task));
    }
    
    /// 尝试出队 (非阻塞)
    bool try_pop(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        
        task = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    /// 查看队首
    std::optional<Task> peek() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        return queue_.front();
    }
    
    /// 队列长度
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }
    
private:
    std::queue<Task> queue_;
    mutable std::mutex mutex_;
};

/**
 * 高性能无锁 SPSC (Single-Producer-Single-Consumer) 队列
 * 
 * 适用于单生产者-单消费者场景
 * 基于环形缓冲区实现
 */
template<typename T, size_t Capacity = 65536>
class SPSCQueue {
public:
    SPSCQueue() : buffer_(new T[Capacity]) {
        static_assert((Capacity & (Capacity - 1)) == 0, 
                      "Capacity must be power of 2");
    }
    
    ~SPSCQueue() {
        delete[] buffer_;
    }
    
    // 禁用拷贝
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    
    /// 尝试入队 (非阻塞)
    bool try_push(T&& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & (Capacity - 1);
        
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }
        
        buffer_[head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    /// 尝试出队 (非阻塞)
    bool try_pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }
    
    /// 近似大小 (非精确)
    size_t size_approx() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail + Capacity) & (Capacity - 1);
    }
    
    bool empty() const {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
private:
    T* buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

/**
 * FCFS 队列统一接口
 */
class FCFSQueue {
public:
    void push(Task&& task) {
        queue_.push(std::move(task));
    }
    
    bool try_pop(Task& task) {
        return queue_.try_pop(task);
    }
    
    size_t size() const {
        return queue_.size();
    }
    
    bool empty() const {
        return queue_.empty();
    }
    
private:
    FCFSQueueLocked queue_;
};

}  // namespace malcolm
