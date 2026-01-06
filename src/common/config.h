#pragma once

/**
 * 配置管理
 * 
 * 支持从命令行和配置文件加载实验参数
 */

#include <string>
#include <vector>
#include <cstdint>
#include "../common/types.h"

namespace malcolm {

/**
 * 全局实验配置
 */
struct ExperimentConfig {
    // 节点配置
    std::string lb_address;
    std::vector<std::string> worker_addresses;
    
    // 实验参数
    SchedulerType algorithm = SchedulerType::kPowerOf2;
    LocalSchedulerType local_scheduler = LocalSchedulerType::kFCFS;
    std::string model_path;
    
    // 负载参数
    uint64_t target_rps = 500000;
    uint32_t duration_sec = 120;
    uint32_t warmup_sec = 30;
    
    // 工作负载参数
    double pareto_alpha = 1.2;
    uint32_t service_time_min_us = 10;
    double deadline_multiplier = 5.0;
    
    // 输出
    std::string output_dir;
    bool verbose = false;
};

/**
 * 从命令行参数解析配置
 */
ExperimentConfig parse_config(int argc, char* argv[]);

/**
 * 从 YAML 文件加载配置
 */
ExperimentConfig load_config_file(const std::string& path);

}  // namespace malcolm
