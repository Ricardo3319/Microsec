cmake_minimum_required(VERSION 3.16)
project(MalcolmStrict CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==================== 编译选项 ====================
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# 调试模式
option(DEBUG_MODE "Enable debug mode" OFF)
if(DEBUG_MODE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0 -DDEBUG")
endif()

# ==================== eRPC 配置 ====================
set(ERPC_ROOT "/opt/erpc" CACHE PATH "Path to eRPC installation")
option(USE_DPDK "Use DPDK transport" OFF)
option(USE_RDMA "Use RDMA transport" ON)
option(USE_RAW "Use Raw Ethernet transport" OFF)

# eRPC 头文件和库
# 注意: eRPC 头文件直接在 ERPC_ROOT 目录下，不是 /src 子目录
include_directories(${ERPC_ROOT})
link_directories(${ERPC_ROOT}/build)

# 根据传输类型设置编译定义和链接库
if(USE_RDMA)
    add_definitions(-DERPC_INFINIBAND=true)
    find_library(IBVERBS_LIB ibverbs REQUIRED)
    set(TRANSPORT_LIBS ${IBVERBS_LIB} pthread numa dl)
    message(STATUS "Using RDMA transport")
elseif(USE_DPDK)
    add_definitions(-DERPC_DPDK=true)
    # DPDK 需要更复杂的配置，这里简化处理
    set(TRANSPORT_LIBS pthread numa dl)
    message(STATUS "Using DPDK transport")
else()
    message(FATAL_ERROR "Must specify transport: USE_RDMA or USE_DPDK")
endif()

# ==================== LibTorch (可选) ====================
option(USE_LIBTORCH "Use LibTorch for DRL inference" ON)
if(USE_LIBTORCH)
    set(CMAKE_PREFIX_PATH "/opt/libtorch" CACHE PATH "Path to LibTorch")
    find_package(Torch QUIET)
    if(Torch_FOUND)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")
        add_definitions(-DUSE_LIBTORCH)
        message(STATUS "LibTorch found: ${TORCH_LIBRARIES}")
    else()
        message(WARNING "LibTorch not found, DRL inference disabled")
        set(USE_LIBTORCH OFF)
    endif()
endif()

# ==================== ONNX Runtime (可选) ====================
option(USE_ONNXRUNTIME "Use ONNX Runtime for inference" OFF)
if(USE_ONNXRUNTIME)
    set(ONNXRUNTIME_ROOT "/opt/onnxruntime" CACHE PATH "Path to ONNX Runtime")
    find_library(ONNXRUNTIME_LIB onnxruntime PATHS ${ONNXRUNTIME_ROOT}/lib)
    if(ONNXRUNTIME_LIB)
        include_directories(${ONNXRUNTIME_ROOT}/include)
        add_definitions(-DUSE_ONNXRUNTIME)
        message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIB}")
    else()
        message(WARNING "ONNX Runtime not found")
        set(USE_ONNXRUNTIME OFF)
    endif()
endif()

# ==================== HdrHistogram ====================
find_library(HDR_HISTOGRAM_LIB hdr_histogram PATHS /usr/local/lib /usr/lib)
find_path(HDR_HISTOGRAM_INCLUDE hdr/hdr_histogram.h PATHS /usr/local/include /usr/include)
if(HDR_HISTOGRAM_LIB AND HDR_HISTOGRAM_INCLUDE)
    include_directories(${HDR_HISTOGRAM_INCLUDE})
    message(STATUS "HdrHistogram found: ${HDR_HISTOGRAM_LIB}")
else()
    message(FATAL_ERROR "HdrHistogram not found. Install with: sudo apt install libhdr-histogram-dev")
endif()

# ==================== 公共头文件 ====================
include_directories(${CMAKE_SOURCE_DIR}/src)

# ==================== 公共库 ====================
add_library(common STATIC
    src/common/metrics.cpp
    src/common/config.cpp
    src/common/workload.cpp
)
target_link_libraries(common ${HDR_HISTOGRAM_LIB})

# ==================== Worker 执行器 ====================
add_executable(worker
    src/worker/main.cpp
    src/worker/worker_context_erpc.cpp
    src/scheduler/edf_queue.cpp
    src/scheduler/fcfs_queue.cpp
)
target_link_libraries(worker 
    common
    erpc 
    ${TRANSPORT_LIBS}
)

# ==================== Load Balancer ====================
set(LB_SOURCES
    src/load_balancer/main.cpp
    src/load_balancer/lb_context_erpc.cpp
    src/scheduler/po2_scheduler.cpp
    src/scheduler/malcolm_scheduler.cpp
    src/scheduler/malcolm_strict_scheduler.cpp
)

add_executable(load_balancer ${LB_SOURCES})

set(LB_LIBS common erpc ${TRANSPORT_LIBS})
if(USE_LIBTORCH)
    list(APPEND LB_LIBS ${TORCH_LIBRARIES})
endif()
if(USE_ONNXRUNTIME)
    list(APPEND LB_LIBS ${ONNXRUNTIME_LIB})
endif()
target_link_libraries(load_balancer ${LB_LIBS})

# ==================== Client ====================
add_executable(client
    src/client/main.cpp
    src/client/client_context_erpc.cpp
    src/client/request_generator.cpp
)
target_link_libraries(client 
    common
    erpc 
    ${TRANSPORT_LIBS}
)

# ==================== 工具程序 (可选) ====================
# histogram_merge 工具 - 如果需要请创建 tools/histogram_merge.cpp
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/histogram_merge.cpp")
    add_executable(histogram_merge
        tools/histogram_merge.cpp
    )
    target_link_libraries(histogram_merge ${HDR_HISTOGRAM_LIB})
endif()

# ==================== 安装 ====================
install(TARGETS worker load_balancer client
    RUNTIME DESTINATION bin
)

# ==================== 测试 (可选) ====================
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    
    add_executable(test_edf_queue tests/test_edf_queue.cpp src/scheduler/edf_queue.cpp)
    target_link_libraries(test_edf_queue GTest::gtest_main)
    add_test(NAME EDFQueueTest COMMAND test_edf_queue)
endif()

# ==================== 打印配置摘要 ====================
message(STATUS "")
message(STATUS "========== Malcolm-Strict Build Configuration ==========")
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "ERPC_ROOT: ${ERPC_ROOT}")
if(USE_RDMA)
    message(STATUS "Transport: RDMA")
elseif(USE_DPDK)
    message(STATUS "Transport: DPDK")
endif()
message(STATUS "LibTorch: ${USE_LIBTORCH}")
message(STATUS "ONNX Runtime: ${USE_ONNXRUNTIME}")
message(STATUS "=========================================================")
