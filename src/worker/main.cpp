/**
 * Worker 主程序入口
 * 
 * 用法:
 *   ./worker --id=0 --port=31850 --mode=fast --scheduler=edf
 */

#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <getopt.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker_context.h"

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

// 全局上下文用于信号处理
static WorkerContext* g_worker = nullptr;

void signal_handler(int sig) {
    printf("\n[Worker] Received signal %d, shutting down...\n", sig);
    if (g_worker) {
        g_worker->stop();
    }
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --id=N          Worker ID (default: 0)\n");
    printf("  --port=PORT     Listen port (default: 31850)\n");
    printf("  --threads=N     Number of RPC threads (default: 8)\n");
    printf("  --mode=MODE     Worker mode: 'fast' or 'slow' (default: fast)\n");
    printf("  --scheduler=S   Local scheduler: 'fcfs' or 'edf' (default: fcfs)\n");
    printf("  --capacity=F    Capacity factor (default: 1.0 for fast, 0.2 for slow)\n");
    printf("  --output=DIR    Metrics output directory\n");
    printf("  --help          Show this help\n");
}

int main(int argc, char* argv[]) {
    WorkerConfig config;
    config.worker_id = 0;
    config.port = constants::kDefaultPort;
    config.num_rpc_threads = 8;
    config.scheduler = LocalSchedulerType::kFCFS;
    config.capacity_factor = 1.0;
    
    std::string mode = "fast";
    
    static struct option long_options[] = {
        {"id",        required_argument, 0, 'i'},
        {"port",      required_argument, 0, 'p'},
        {"threads",   required_argument, 0, 't'},
        {"mode",      required_argument, 0, 'm'},
        {"scheduler", required_argument, 0, 's'},
        {"capacity",  required_argument, 0, 'c'},
        {"output",    required_argument, 0, 'o'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "i:p:t:m:s:c:o:h", 
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'i':
                config.worker_id = static_cast<uint8_t>(std::stoi(optarg));
                break;
            case 'p':
                config.port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 't':
                config.num_rpc_threads = std::stoul(optarg);
                break;
            case 'm':
                mode = optarg;
                break;
            case 's':
                if (strcmp(optarg, "edf") == 0) {
                    config.scheduler = LocalSchedulerType::kEDF;
                } else {
                    config.scheduler = LocalSchedulerType::kFCFS;
                }
                break;
            case 'c':
                config.capacity_factor = std::stod(optarg);
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
    
    // 根据 mode 设置默认参数
    if (mode == "slow") {
        if (config.capacity_factor == 1.0) {
            config.capacity_factor = 0.2;  // Slow node 默认 20% 容量
        }
        if (config.num_rpc_threads > 2) {
            config.num_rpc_threads = 2;    // Slow node 限制线程数
        }
        if (config.artificial_delay_ns == 0) {
            config.artificial_delay_ns = 500000;  // Slow node 注入 500μs 延迟
        }
    }
    
    // 设置服务器 URI - 使用实际 IP 地址以便 eRPC RDMA 正确处理
    std::string local_ip = get_local_ip();
    config.server_uri = local_ip + ":" + std::to_string(config.port);
    printf("Local IP:        %s\n", local_ip.c_str());
    
    printf("========================================\n");
    printf("Malcolm-Strict Worker\n");
    printf("========================================\n");
    printf("Worker ID:       %u\n", config.worker_id);
    printf("Mode:            %s\n", mode.c_str());
    printf("Port:            %u\n", config.port);
    printf("Threads:         %zu\n", config.num_rpc_threads);
    printf("Scheduler:       %s\n", 
           config.scheduler == LocalSchedulerType::kEDF ? "EDF" : "FCFS");
    printf("Capacity Factor: %.2f\n", config.capacity_factor);
    printf("Artificial Delay: %lu us\n", config.artificial_delay_ns / 1000);
    printf("========================================\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建并启动 Worker
    WorkerContext worker(config);
    g_worker = &worker;
    
    worker.start();
    
    printf("[Worker %u] Running, press Ctrl+C to stop...\n", config.worker_id);
    
    worker.wait();
    
    printf("[Worker %u] Exited cleanly\n", config.worker_id);
    return 0;
}
