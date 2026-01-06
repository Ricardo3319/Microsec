#!/bin/bash
# quick_setup.sh - 在 CloudLab 集群上快速设置实验环境
#
# 用法 (在 node0 上运行):
#   ./scripts/quick_setup.sh
#
# 此脚本会:
# 1. 安装必要的依赖
# 2. 编译 eRPC
# 3. 下载 LibTorch
# 4. 编译项目
# 5. 同步到所有节点

set -euo pipefail

# 配置
PROJECT_ROOT="/users/Mingyang/microSec"
ERPC_ROOT="/opt/erpc"
LIBTORCH_ROOT="/opt/libtorch"
NODES=(10.10.1.{1..8})

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

log_section() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

# ==================== 1. 安装系统依赖 ====================

install_dependencies() {
    log_section "Installing system dependencies"
    
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        git \
        wget \
        unzip \
        pkg-config \
        libnuma-dev \
        libhugetlbfs-dev \
        libibverbs-dev \
        librdmacm-dev \
        zlib1g-dev \
        cgroup-tools \
        netcat-openbsd \
        python3 \
        python3-pip \
        python3-numpy
    
    # Python 依赖 (用于报告生成)
    pip3 install --user matplotlib 2>/dev/null || true
    
    log "System dependencies installed"
}

# ==================== 1.5 编译 HdrHistogram ====================

setup_hdrhistogram() {
    log_section "Setting up HdrHistogram"
    
    # 检查是否已安装
    if [ -f "/usr/local/lib/libhdr_histogram.so" ] || [ -f "/usr/local/lib/libhdr_histogram.a" ]; then
        log "HdrHistogram already installed"
        return
    fi
    
    cd /tmp
    
    # 清理旧的构建
    rm -rf HdrHistogram_c
    
    # 克隆 HdrHistogram C 库
    log "Cloning HdrHistogram_c..."
    git clone --depth 1 https://github.com/HdrHistogram/HdrHistogram_c.git
    
    cd HdrHistogram_c
    mkdir -p build && cd build
    
    # 编译
    log "Building HdrHistogram..."
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    
    # 安装到系统目录
    log "Installing HdrHistogram..."
    sudo make install
    
    # 更新动态链接库缓存
    sudo ldconfig
    
    log "HdrHistogram installed successfully"
    
    # 清理
    cd /tmp
    rm -rf HdrHistogram_c
}

# ==================== 2. 编译 eRPC ====================

setup_erpc() {
    log_section "Setting up eRPC"
    
    if [ -d "$ERPC_ROOT" ] && [ -f "$ERPC_ROOT/build/liberpc.a" ]; then
        log "eRPC already built at $ERPC_ROOT"
        return
    fi
    
    sudo mkdir -p "$ERPC_ROOT"
    sudo chown $USER:$(id -gn) "$ERPC_ROOT"
    
    cd /tmp
    rm -rf eRPC
    
    log "Cloning eRPC..."
    git clone --depth 1 https://github.com/erpc-io/eRPC.git
    
    cd eRPC
    
    # 创建构建目录
    mkdir -p build
    cd build
    
    # 配置 eRPC (使用 InfiniBand/RoCE)
    log "Building eRPC..."
    cmake .. \
        -DPERF=ON \
        -DTRANSPORT=infiniband \
        -DCMAKE_BUILD_TYPE=Release
    
    make -j$(nproc)
    
    # 复制到安装目录
    cd ..
    sudo cp -r src/* "$ERPC_ROOT/"
    sudo mkdir -p "$ERPC_ROOT/build"
    sudo cp build/liberpc.a "$ERPC_ROOT/build/"
    sudo chown -R $USER:$(id -gn) "$ERPC_ROOT"
    
    # 清理
    cd /tmp
    rm -rf eRPC
    
    log "eRPC built and installed to $ERPC_ROOT"
}

# ==================== 3. 下载 LibTorch ====================

setup_libtorch() {
    log_section "Setting up LibTorch"
    
    if [ -d "$LIBTORCH_ROOT" ] && [ -f "$LIBTORCH_ROOT/lib/libtorch.so" ]; then
        log "LibTorch already installed at $LIBTORCH_ROOT"
        return
    fi
    
    cd /tmp
    
    # 下载 LibTorch (CPU 版本)
    local LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.0.0%2Bcpu.zip"
    
    if [ ! -f "libtorch.zip" ]; then
        log "Downloading LibTorch..."
        wget -q --show-progress "$LIBTORCH_URL" -O libtorch.zip
    fi
    
    log "Extracting LibTorch..."
    rm -rf libtorch
    unzip -q libtorch.zip
    
    sudo rm -rf "$LIBTORCH_ROOT"
    sudo mv libtorch "$LIBTORCH_ROOT"
    sudo chown -R $USER:$(id -gn) "$LIBTORCH_ROOT"
    
    # 清理
    rm -f libtorch.zip
    
    log "LibTorch installed to $LIBTORCH_ROOT"
}

# ==================== 4. 编译项目 ====================

build_project() {
    log_section "Building Malcolm-Strict"
    
    cd "$PROJECT_ROOT"
    
    mkdir -p build
    cd build
    
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DERPC_ROOT="$ERPC_ROOT" \
        -DCMAKE_PREFIX_PATH="$LIBTORCH_ROOT" \
        -DUSE_RDMA=ON \
        -DUSE_LIBTORCH=ON
    
    make -j$(nproc)
    
    log "Project built successfully"
}

# ==================== 5. 同步到所有节点 ====================

sync_to_nodes() {
    log_section "Syncing to all nodes"
    
    local current_ip=$(hostname -I | awk '{print $1}')
    
    for ip in "${NODES[@]}"; do
        if [ "$ip" != "$current_ip" ]; then
            log "Syncing to $ip..."
            
            # 同步依赖
            ssh -o StrictHostKeyChecking=no $ip "sudo mkdir -p $ERPC_ROOT $LIBTORCH_ROOT && sudo chown \$USER:\$(id -gn) $ERPC_ROOT $LIBTORCH_ROOT" 2>/dev/null || true
            
            rsync -az "$ERPC_ROOT/" "$ip:$ERPC_ROOT/" &
            rsync -az "$LIBTORCH_ROOT/" "$ip:$LIBTORCH_ROOT/" &
            
            # 同步项目
            rsync -az --exclude 'build' "$PROJECT_ROOT/" "$ip:$PROJECT_ROOT/" &
        fi
    done
    
    wait
    log "Sync completed"
    
    # 在远程节点上安装 HdrHistogram 并编译
    log "Installing HdrHistogram and building on remote nodes..."
    for ip in "${NODES[@]}"; do
        if [ "$ip" != "$current_ip" ]; then
            log "  Setting up $ip..."
            ssh $ip "
                # 安装 HdrHistogram (如果尚未安装)
                if [ ! -f /usr/local/lib/libhdr_histogram.so ] && [ ! -f /usr/local/lib/libhdr_histogram.a ]; then
                    sudo apt-get update && sudo apt-get install -y build-essential cmake git zlib1g-dev
                    cd /tmp && rm -rf HdrHistogram_c
                    git clone --depth 1 https://github.com/HdrHistogram/HdrHistogram_c.git
                    cd HdrHistogram_c && mkdir -p build && cd build
                    cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc) && sudo make install && sudo ldconfig
                    cd /tmp && rm -rf HdrHistogram_c
                fi
                
                # 编译项目
                cd $PROJECT_ROOT && mkdir -p build && cd build && \
                cmake .. -DCMAKE_BUILD_TYPE=Release \
                         -DERPC_ROOT=$ERPC_ROOT \
                         -DCMAKE_PREFIX_PATH=$LIBTORCH_ROOT \
                         -DUSE_RDMA=ON -DUSE_LIBTORCH=ON && \
                make -j\$(nproc)
            " &
        fi
    done
    wait
    
    log "All nodes ready"
}

# ==================== 6. 验证 RDMA ====================

verify_rdma() {
    log_section "Verifying RDMA connectivity"
    
    # 检查 RDMA 设备
    if ! ibv_devinfo > /dev/null 2>&1; then
        log "WARNING: No RDMA devices found. Will use RoCE or fall back to DPDK."
        return 1
    fi
    
    log "RDMA devices:"
    ibv_devinfo | head -20
    
    # 简单的 RDMA 连通性测试
    # (实际测试需要在两个节点间运行)
    
    log "RDMA verification completed"
}

# ==================== 7. 创建目录结构 ====================

create_directories() {
    log_section "Creating directory structure"
    
    mkdir -p "$PROJECT_ROOT"/{logs,results,models,configs}
    
    # 创建示例模型占位符
    touch "$PROJECT_ROOT/models/malcolm_nash.pt"
    touch "$PROJECT_ROOT/models/malcolm_strict_iqn.pt"
    
    log "Directory structure created"
}

# ==================== 主流程 ====================

main() {
    log_section "Malcolm-Strict Quick Setup"
    log "Project Root: $PROJECT_ROOT"
    log "eRPC Root: $ERPC_ROOT"
    log "LibTorch Root: $LIBTORCH_ROOT"
    
    # 检查是否为 root
    if [ "$EUID" -eq 0 ]; then
        log "WARNING: Running as root is not recommended"
    fi
    
    install_dependencies
    setup_hdrhistogram
    create_directories
    setup_erpc
    setup_libtorch
    build_project
    verify_rdma
    sync_to_nodes
    
    log_section "Setup Complete!"
    log ""
    log "Next steps:"
    log "  1. Train your IQN model and copy to: $PROJECT_ROOT/models/malcolm_strict_iqn.pt"
    log "  2. Run experiments: cd $PROJECT_ROOT && ./scripts/orchestrate.sh"
    log ""
    log "Quick test:"
    log "  $PROJECT_ROOT/build/worker --help"
    log "  $PROJECT_ROOT/build/load_balancer --help"
    log "  $PROJECT_ROOT/build/client --help"
}

main "$@"
