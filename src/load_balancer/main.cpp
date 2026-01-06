/**
 * Load Balancer 主程序入口
 * 
 * 用法:
 *   ./load_balancer --algorithm=malcolm_strict \
 *                   --workers=10.10.1.4:31850,10.10.1.5:31850,... \
 *                   --model=models/iqn.pt
 */

#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <sstream>
#include <getopt.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "lb_context.h"

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

using namespace malcolm;

static LBContext* g_lb = nullptr;

void signal_handler(int sig) {
    printf("\n[LB] Received signal %d, shutting down...\n", sig);
    if (g_lb) {
        g_lb->stop();
    }
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --port=PORT       Listen port (default: 31850)\n");
    printf("  --workers=LIST    Comma-separated worker addresses (ip:port)\n");
    printf("  --algorithm=ALG   Scheduling algorithm: po2, malcolm, malcolm_strict\n");
    printf("  --model=PATH      Path to DRL model (for malcolm/malcolm_strict)\n");
    printf("  --threads=N       Number of RPC threads (default: 8)\n");
    printf("  --output=DIR      Metrics output directory\n");
    printf("  --help            Show this help\n");
}

std::vector<std::string> parse_worker_list(const char* list) {
    std::vector<std::string> workers;
    std::stringstream ss(list);
    std::string addr;
    while (std::getline(ss, addr, ',')) {
        // 去除空白
        size_t start = addr.find_first_not_of(" \t");
        size_t end = addr.find_last_not_of(" \t");
        if (start != std::string::npos) {
            workers.push_back(addr.substr(start, end - start + 1));
        }
    }
    return workers;
}

int main(int argc, char* argv[]) {
    LBConfig config;
    config.port = constants::kDefaultPort;
    config.algorithm = SchedulerType::kPowerOf2;
    config.num_rpc_threads = 8;
    
    static struct option long_options[] = {
        {"port",      required_argument, 0, 'p'},
        {"workers",   required_argument, 0, 'w'},
        {"algorithm", required_argument, 0, 'a'},
        {"model",     required_argument, 0, 'm'},
        {"threads",   required_argument, 0, 't'},
        {"output",    required_argument, 0, 'o'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "p:w:a:m:t:o:h", 
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'p':
                config.port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'w':
                config.worker_addresses = parse_worker_list(optarg);
                break;
            case 'a':
                if (strcmp(optarg, "po2") == 0) {
                    config.algorithm = SchedulerType::kPowerOf2;
                } else if (strcmp(optarg, "malcolm") == 0) {
                    config.algorithm = SchedulerType::kMalcolm;
                } else if (strcmp(optarg, "malcolm_strict") == 0) {
                    config.algorithm = SchedulerType::kMalcolmStrict;
                } else {
                    fprintf(stderr, "Unknown algorithm: %s\n", optarg);
                    return 1;
                }
                break;
            case 'm':
                config.model_path = optarg;
                break;
            case 't':
                config.num_rpc_threads = std::stoul(optarg);
                break;
            case 'o':
                config.metrics_output_dir = optarg;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    if (config.worker_addresses.empty()) {
        fprintf(stderr, "Error: No workers specified. Use --workers=...\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // 使用实际 IP 地址而不是 0.0.0.0，以便 eRPC 正确处理 RDMA 响应
    config.listen_uri = get_local_ip() + ":" + std::to_string(config.port);
    
    printf("========================================\n");
    printf("Malcolm-Strict Load Balancer\n");
    printf("========================================\n");
    printf("Listen:     %s\n", config.listen_uri.c_str());
    printf("Algorithm:  %s\n", scheduler_type_name(config.algorithm));
    printf("Model:      %s\n", config.model_path.empty() ? "(none)" : config.model_path.c_str());
    printf("Threads:    %zu\n", config.num_rpc_threads);
    printf("Workers:    %zu\n", config.worker_addresses.size());
    for (size_t i = 0; i < config.worker_addresses.size(); ++i) {
        printf("  [%zu] %s\n", i, config.worker_addresses[i].c_str());
    }
    printf("========================================\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建并启动 LB
    LBContext lb(config);
    g_lb = &lb;
    
    lb.start();
    
    printf("[LB] Running, press Ctrl+C to stop...\n");
    
    lb.wait();
    
    printf("[LB] Exited cleanly\n");
    return 0;
}
