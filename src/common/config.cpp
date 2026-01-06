#include "config.h"
#include <getopt.h>
#include <cstring>
#include <fstream>
#include <sstream>

namespace malcolm {

ExperimentConfig parse_config(int argc, char* argv[]) {
    ExperimentConfig config;
    
    static struct option long_options[] = {
        {"lb",           required_argument, 0, 'l'},
        {"workers",      required_argument, 0, 'w'},
        {"algorithm",    required_argument, 0, 'a'},
        {"scheduler",    required_argument, 0, 's'},
        {"model",        required_argument, 0, 'm'},
        {"rps",          required_argument, 0, 'r'},
        {"duration",     required_argument, 0, 'd'},
        {"warmup",       required_argument, 0, 'W'},
        {"alpha",        required_argument, 0, 'A'},
        {"output",       required_argument, 0, 'o'},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "l:w:a:s:m:r:d:W:A:o:vh", 
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'l':
                config.lb_address = optarg;
                break;
            case 'w': {
                // 逗号分隔的 Worker 地址列表
                std::stringstream ss(optarg);
                std::string addr;
                while (std::getline(ss, addr, ',')) {
                    config.worker_addresses.push_back(addr);
                }
                break;
            }
            case 'a':
                if (strcmp(optarg, "po2") == 0) {
                    config.algorithm = SchedulerType::kPowerOf2;
                } else if (strcmp(optarg, "malcolm") == 0) {
                    config.algorithm = SchedulerType::kMalcolm;
                } else if (strcmp(optarg, "malcolm_strict") == 0) {
                    config.algorithm = SchedulerType::kMalcolmStrict;
                }
                break;
            case 's':
                if (strcmp(optarg, "edf") == 0) {
                    config.local_scheduler = LocalSchedulerType::kEDF;
                } else {
                    config.local_scheduler = LocalSchedulerType::kFCFS;
                }
                break;
            case 'm':
                config.model_path = optarg;
                break;
            case 'r':
                config.target_rps = std::stoull(optarg);
                break;
            case 'd':
                config.duration_sec = std::stoul(optarg);
                break;
            case 'W':
                config.warmup_sec = std::stoul(optarg);
                break;
            case 'A':
                config.pareto_alpha = std::stod(optarg);
                break;
            case 'o':
                config.output_dir = optarg;
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
            default:
                // 打印帮助信息由调用者处理
                break;
        }
    }
    
    return config;
}

ExperimentConfig load_config_file(const std::string& path) {
    ExperimentConfig config;
    
    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }
    
    // 简单的 key: value 解析
    std::string line;
    while (std::getline(file, line)) {
        // 跳过注释和空行
        if (line.empty() || line[0] == '#') continue;
        
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        
        // 去除空白
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t");
            size_t end = s.find_last_not_of(" \t");
            if (start != std::string::npos) {
                s = s.substr(start, end - start + 1);
            }
        };
        trim(key);
        trim(value);
        
        if (key == "lb_address") {
            config.lb_address = value;
        } else if (key == "target_rps") {
            config.target_rps = std::stoull(value);
        } else if (key == "duration_sec") {
            config.duration_sec = std::stoul(value);
        } else if (key == "pareto_alpha") {
            config.pareto_alpha = std::stod(value);
        }
        // ... 添加更多配置项
    }
    
    return config;
}

}  // namespace malcolm
