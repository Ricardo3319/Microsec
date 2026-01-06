#pragma once

/**
 * 工作负载生成器
 * 
 * 生成符合重尾分布 (Pareto/Lognormal) 的服务时间请求
 * 这是触发 "方差陷阱" 的关键
 */

#include <random>
#include <cmath>
#include <algorithm>
#include <atomic>
#include "../common/types.h"

namespace malcolm {

/**
 * Pareto 分布生成器
 * 
 * PDF: f(x) = α * x_m^α / x^(α+1), x >= x_m
 * 
 * 当 α <= 2 时，方差无穷大 (触发方差陷阱)
 * 当 α <= 1 时，均值无穷大
 */
class ParetoGenerator {
public:
    /**
     * @param alpha 形状参数 (越小尾越重，通常 1.1 ~ 1.5)
     * @param x_min 最小值 (scale parameter)
     */
    ParetoGenerator(double alpha = 1.2, double x_min = 10.0)
        : alpha_(alpha), x_min_(x_min), dist_(0.0, 1.0) {}
    
    /// 生成一个 Pareto 分布样本
    double sample(std::mt19937& rng) {
        double u = dist_(rng);
        // 逆变换采样: x = x_min / u^(1/α)
        return x_min_ / std::pow(u, 1.0 / alpha_);
    }
    
    /// 理论均值 (仅当 α > 1 时有限)
    double theoretical_mean() const {
        if (alpha_ <= 1.0) return std::numeric_limits<double>::infinity();
        return alpha_ * x_min_ / (alpha_ - 1.0);
    }
    
    /// 理论方差 (仅当 α > 2 时有限)
    double theoretical_variance() const {
        if (alpha_ <= 2.0) return std::numeric_limits<double>::infinity();
        // 方差公式: (x_min^2 * alpha) / ((alpha-1)^2 * (alpha-2))
        return x_min_ * x_min_ * alpha_ / ((alpha_ - 1.0) * (alpha_ - 1.0) * (alpha_ - 2.0));
    }
    
private:
    double alpha_;
    double x_min_;
    std::uniform_real_distribution<double> dist_;
};

/**
 * Lognormal 分布生成器
 * 
 * 另一种常见的重尾分布，用于对比实验
 */
class LognormalGenerator {
public:
    /**
     * @param mu 对数均值
     * @param sigma 对数标准差 (越大尾越重)
     */
    LognormalGenerator(double mu = 2.3, double sigma = 1.0)
        : dist_(mu, sigma) {}
    
    double sample(std::mt19937& rng) {
        return dist_(rng);
    }
    
private:
    std::lognormal_distribution<double> dist_;
};

/**
 * 双峰分布生成器
 * 
 * 模拟混合工作负载 (大量轻请求 + 少量重请求)
 */
class BimodalGenerator {
public:
    /**
     * @param p_light 轻请求概率
     * @param light_mean 轻请求均值 (μs)
     * @param heavy_mean 重请求均值 (μs)
     */
    BimodalGenerator(double p_light = 0.9, 
                     double light_mean = 10.0, 
                     double heavy_mean = 1000.0)
        : p_light_(p_light),
          light_dist_(light_mean, light_mean * 0.1),
          heavy_dist_(heavy_mean, heavy_mean * 0.2),
          uniform_(0.0, 1.0) {}
    
    double sample(std::mt19937& rng) {
        if (uniform_(rng) < p_light_) {
            return std::max(1.0, light_dist_(rng));
        } else {
            return std::max(1.0, heavy_dist_(rng));
        }
    }
    
private:
    double p_light_;
    std::normal_distribution<double> light_dist_;
    std::normal_distribution<double> heavy_dist_;
    std::uniform_real_distribution<double> uniform_;
};

/**
 * 工作负载生成器工厂
 */
enum class WorkloadDistribution {
    kPareto,
    kLognormal,
    kBimodal,
    kUniform
};

/**
 * 请求生成器配置
 */
struct RequestGeneratorConfig {
    WorkloadDistribution distribution = WorkloadDistribution::kPareto;
    
    // Pareto 参数
    double pareto_alpha = 1.2;       // 形状参数
    double service_time_min_us = 10; // 最小服务时间 (μs)
    
    // Deadline 参数
    double deadline_multiplier = 5.0;  // deadline = service_time * multiplier
    Timestamp fixed_deadline_us = 0;   // 固定截止时间 (0 = 使用 multiplier)
    
    // 请求类型分布
    double p_get = 0.7;    // GET 请求概率
    double p_put = 0.2;    // PUT 请求概率
    double p_scan = 0.05;  // SCAN 请求概率
    // 剩余为 Compute
};

/**
 * 请求生成器
 * 
 * 生成带有服务时间和截止时间的请求
 */
class RequestGenerator {
public:
    using Config = RequestGeneratorConfig;
    
    RequestGenerator()
        : config_(),
          pareto_(config_.pareto_alpha, config_.service_time_min_us),
          rng_(std::random_device{}()),
          uniform_(0.0, 1.0) {}
    
    explicit RequestGenerator(const Config& config)
        : config_(config),
          pareto_(config.pareto_alpha, config.service_time_min_us),
          rng_(std::random_device{}()),
          uniform_(0.0, 1.0) {}
    
    /// 生成下一个请求
    ClientRequest generate() {
        ClientRequest req;
        req.request_id = next_id_++;
        req.client_send_time = now_ns();
        
        // 生成请求类型
        double r = uniform_(rng_);
        if (r < config_.p_get) {
            req.type = RequestType::kGetRequest;
        } else if (r < config_.p_get + config_.p_put) {
            req.type = RequestType::kPutRequest;
        } else if (r < config_.p_get + config_.p_put + config_.p_scan) {
            req.type = RequestType::kScanRequest;
        } else {
            req.type = RequestType::kCompute;
        }
        
        // 生成服务时间
        double service_us = 0;
        switch (config_.distribution) {
            case WorkloadDistribution::kPareto:
                service_us = pareto_.sample(rng_);
                break;
            case WorkloadDistribution::kLognormal:
                service_us = LognormalGenerator().sample(rng_);
                break;
            case WorkloadDistribution::kBimodal:
                service_us = BimodalGenerator().sample(rng_);
                break;
            case WorkloadDistribution::kUniform:
                service_us = config_.service_time_min_us * (1.0 + uniform_(rng_));
                break;
        }
        
        req.expected_service_us = static_cast<uint32_t>(service_us);
        
        // 设置截止时间
        if (config_.fixed_deadline_us > 0) {
            req.deadline = req.client_send_time + us_to_ns(config_.fixed_deadline_us);
        } else {
            req.deadline = req.client_send_time + 
                           us_to_ns(static_cast<uint64_t>(service_us * config_.deadline_multiplier));
        }
        
        // 生成负载大小 (简化)
        req.payload_size = 64 + (rng_() % 256);
        
        return req;
    }
    
    /// 设置随机种子 (用于可重复性)
    void set_seed(uint64_t seed) {
        rng_.seed(seed);
    }
    
private:
    Config config_;
    ParetoGenerator pareto_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uniform_;
    uint64_t next_id_{0};  // Not thread-safe, each thread should have its own generator
};

}  // namespace malcolm
