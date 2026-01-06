/**
 * Client 主程序入口
 * 
 * 用法:
 *   ./client --id=0 --lb=10.10.1.3:31850 --threads=8 --target_rps=100000 \
 *            --duration=120 --warmup=30 --output=results/
 */

#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <getopt.h>

#include "client_context.h"

using namespace malcolm;

static ClientContext* g_client = nullptr;

void signal_handler(int sig) {
    printf("\n[Client] Received signal %d, shutting down...\n", sig);
    if (g_client) {
        g_client->stop();
    }
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --id=N            Client ID (default: 0)\n");
    printf("  --lb=ADDR         Load Balancer address (ip:port)\n");
    printf("  --threads=N       Number of sender threads (default: 8)\n");
    printf("  --target_rps=N    Target requests per second (default: 100000)\n");
    printf("  --duration=SEC    Experiment duration in seconds (default: 120)\n");
    printf("  --warmup=SEC      Warmup duration in seconds (default: 30)\n");
    printf("  --pareto_alpha=F  Pareto distribution alpha (default: 1.2)\n");
    printf("  --service_min=US  Minimum service time in microseconds (default: 10)\n");
    printf("  --slow_prob=F     Probability of hitting slow worker (default: 0.6)\n");
    printf("  --output=DIR      Output directory for results\n");
    printf("  --verbose         Enable verbose output\n");
    printf("  --help            Show this help\n");
}

int main(int argc, char* argv[]) {
    ClientConfig config;
    config.client_id = 0;
    config.num_threads = 8;
    config.target_rps = 100000;
    config.duration_sec = 120;
    config.warmup_sec = 30;
    
    // 默认工作负载配置
    config.workload.distribution = WorkloadDistribution::kPareto;
    config.workload.pareto_alpha = 1.2;
    config.workload.service_time_min_us = 10;
    config.workload.deadline_multiplier = 5.0;  // deadline = service_time * multiplier (原始值)
    
    static struct option long_options[] = {
        {"id",          required_argument, 0, 'i'},
        {"lb",          required_argument, 0, 'l'},
        {"threads",     required_argument, 0, 't'},
        {"target_rps",  required_argument, 0, 'r'},
        {"duration",    required_argument, 0, 'd'},
        {"warmup",      required_argument, 0, 'w'},
        {"pareto_alpha",required_argument, 0, 'a'},
        {"service_min", required_argument, 0, 's'},
        {"slow_prob",   required_argument, 0, 'p'},
        {"output",      required_argument, 0, 'o'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "i:l:t:r:d:w:a:s:p:o:vh", 
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'i':
                config.client_id = static_cast<uint8_t>(std::stoi(optarg));
                break;
            case 'l':
                config.lb_address = optarg;
                break;
            case 't':
                config.num_threads = std::stoul(optarg);
                break;
            case 'r':
                config.target_rps = std::stoull(optarg);
                break;
            case 'd':
                config.duration_sec = std::stoul(optarg);
                break;
            case 'w':
                config.warmup_sec = std::stoul(optarg);
                break;
            case 'a':
                config.workload.pareto_alpha = std::stod(optarg);
                break;
            case 's':
                config.workload.service_time_min_us = std::stod(optarg);
                break;
            case 'p':
                config.slow_worker_prob = std::stod(optarg);
                break;
            case 'o':
                config.output_dir = optarg;
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    if (config.lb_address.empty()) {
        fprintf(stderr, "Error: No Load Balancer address specified. Use --lb=...\n");
        print_usage(argv[0]);
        return 1;
    }
    
    printf("========================================\n");
    printf("Malcolm-Strict Client\n");
    printf("========================================\n");
    printf("Client ID:    %u\n", config.client_id);
    printf("LB Address:   %s\n", config.lb_address.c_str());
    printf("Threads:      %zu\n", config.num_threads);
    printf("Target RPS:   %lu\n", config.target_rps);
    printf("Duration:     %us (+%us warmup)\n", config.duration_sec, config.warmup_sec);
    printf("Pareto Alpha: %.2f\n", config.workload.pareto_alpha);
    printf("Service Min:  %.0fus\n", config.workload.service_time_min_us);
    printf("Slow Prob:    %.2f\n", config.slow_worker_prob);
    printf("========================================\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建并运行 Client
    ClientContext client(config);
    g_client = &client;
    
    client.run();
    
    printf("[Client %u] Exited cleanly\n", config.client_id);
    return 0;
}
