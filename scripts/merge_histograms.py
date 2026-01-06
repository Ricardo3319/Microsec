#!/usr/bin/env python3
"""
合并多个 HdrHistogram 文件并计算全局百分位

用法:
    python3 merge_histograms.py \
        --inputs "results/exp_a/client_*/*.hdr" \
        --output "results/exp_a/combined_latency.csv"
"""

import argparse
import glob
import numpy as np
from pathlib import Path
import sys


def parse_hdr_classic(path: str) -> np.ndarray:
    """
    解析 HdrHistogram 的 CLASSIC 格式输出
    
    格式:
    # [Histogram ... ]
    # [Buckets ... ]
    Value     Percentile TotalCount 1/(1-Percentile)
    1.000     0.000000      1        1.00
    ...
    """
    values = []
    counts = []
    
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('Value'):
                continue
            
            parts = line.split()
            if len(parts) >= 3:
                try:
                    value = float(parts[0])
                    count = int(float(parts[2]))
                    
                    # 下采样大量计数以控制内存
                    sample_count = min(count, 1000)
                    values.extend([value] * sample_count)
                except (ValueError, IndexError):
                    continue
    
    return np.array(values)


def parse_hdr_log(path: str) -> np.ndarray:
    """
    解析 HdrHistogram Log 格式 (Base64 编码)
    
    这是更紧凑的格式，需要专用库解析
    简化版本：直接读取原始值
    """
    # 对于 Log 格式，建议使用 hdrhistogram 包
    # pip install hdrhistogram
    try:
        from hdrhistogram import HdrHistogram
        # TODO: 实现 Log 格式解析
        pass
    except ImportError:
        pass
    
    return np.array([])


def parse_raw_csv(path: str) -> np.ndarray:
    """
    解析简单的 CSV 格式延迟数据
    
    格式: latency_ns 或 timestamp,latency_ns
    """
    values = []
    
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or 'latency' in line.lower():
                continue
            
            parts = line.split(',')
            try:
                # 尝试解析最后一列作为延迟值
                value = float(parts[-1])
                values.append(value)
            except (ValueError, IndexError):
                continue
    
    return np.array(values)


def load_latencies(path: str) -> np.ndarray:
    """
    根据文件类型自动选择解析方法
    """
    if path.endswith('.hdr'):
        return parse_hdr_classic(path)
    elif path.endswith('.csv'):
        return parse_raw_csv(path)
    else:
        # 尝试自动检测
        with open(path, 'r') as f:
            first_line = f.readline().strip()
            
        if first_line.startswith('#') or 'Value' in first_line:
            return parse_hdr_classic(path)
        else:
            return parse_raw_csv(path)


def compute_percentiles(latencies: np.ndarray, percentiles: list) -> dict:
    """
    计算指定百分位
    """
    results = {}
    for p in percentiles:
        results[f'P{p}'] = np.percentile(latencies, p)
    return results


def generate_cdf(latencies: np.ndarray, num_points: int = 10000) -> np.ndarray:
    """
    生成 CDF 数据点
    
    返回: [(percentile, latency), ...]
    """
    sorted_lat = np.sort(latencies)
    n = len(sorted_lat)
    
    cdf = []
    for i in range(num_points + 1):
        p = i / num_points * 100
        idx = min(int(p / 100 * n), n - 1)
        cdf.append((p, sorted_lat[idx]))
    
    return np.array(cdf)


def main():
    parser = argparse.ArgumentParser(description='Merge HdrHistogram files')
    parser.add_argument('--inputs', type=str, required=True,
                       help='Glob pattern for input files')
    parser.add_argument('--output', type=str, required=True,
                       help='Output CSV path')
    parser.add_argument('--unit', type=str, default='us',
                       choices=['ns', 'us', 'ms'],
                       help='Output latency unit (default: us)')
    args = parser.parse_args()
    
    # 查找所有输入文件
    files = glob.glob(args.inputs)
    if not files:
        print(f"No files found matching: {args.inputs}", file=sys.stderr)
        sys.exit(1)
    
    print(f"Found {len(files)} input file(s)")
    
    # 加载所有延迟数据
    all_latencies = []
    for path in files:
        print(f"  Loading {path}...")
        lat = load_latencies(path)
        if len(lat) > 0:
            all_latencies.append(lat)
            print(f"    -> {len(lat)} samples, P50={np.median(lat)/1000:.2f}us")
    
    if not all_latencies:
        print("No latency data found!", file=sys.stderr)
        sys.exit(1)
    
    # 合并
    combined = np.concatenate(all_latencies)
    print(f"\nTotal samples: {len(combined)}")
    
    # 单位转换
    divisor = {'ns': 1, 'us': 1000, 'ms': 1000000}[args.unit]
    combined_scaled = combined / divisor
    
    # 计算统计
    percentiles = [50, 90, 95, 99, 99.5, 99.9, 99.99]
    stats = compute_percentiles(combined_scaled, percentiles)
    
    print(f"\n=== Combined Latency Statistics ({args.unit}) ===")
    print(f"  Mean:   {np.mean(combined_scaled):.2f}")
    print(f"  Stddev: {np.std(combined_scaled):.2f}")
    print(f"  Min:    {np.min(combined_scaled):.2f}")
    print(f"  Max:    {np.max(combined_scaled):.2f}")
    for name, value in stats.items():
        print(f"  {name}: {value:.2f}")
    
    # 生成 CDF
    cdf = generate_cdf(combined_scaled)
    
    # 导出
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(output_path, 'w') as f:
        f.write(f"percentile,latency_{args.unit}\n")
        for p, lat in cdf:
            f.write(f"{p:.4f},{lat:.4f}\n")
    
    print(f"\nCDF exported to {output_path}")
    
    # 同时导出摘要
    summary_path = output_path.with_suffix('.summary.txt')
    with open(summary_path, 'w') as f:
        f.write(f"Total Samples: {len(combined)}\n")
        f.write(f"Unit: {args.unit}\n")
        f.write(f"Mean: {np.mean(combined_scaled):.4f}\n")
        f.write(f"Stddev: {np.std(combined_scaled):.4f}\n")
        f.write(f"Min: {np.min(combined_scaled):.4f}\n")
        f.write(f"Max: {np.max(combined_scaled):.4f}\n")
        for name, value in stats.items():
            f.write(f"{name}: {value:.4f}\n")
    
    print(f"Summary exported to {summary_path}")


if __name__ == '__main__':
    main()
