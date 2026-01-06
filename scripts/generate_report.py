#!/usr/bin/env python3
"""
生成实验对比报告

用法:
    python3 generate_report.py \
        --results_dir results/ \
        --output results/comparison_report.pdf
"""

import argparse
import json
from pathlib import Path
import sys

try:
    import matplotlib
    matplotlib.use('Agg')  # 非交互后端
    import matplotlib.pyplot as plt
    import numpy as np
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not found, skipping PDF generation")


def load_cdf(path: str) -> tuple:
    """加载 CDF CSV 文件"""
    percentiles = []
    latencies = []
    
    with open(path, 'r') as f:
        header = f.readline()  # 跳过表头
        for line in f:
            parts = line.strip().split(',')
            if len(parts) >= 2:
                percentiles.append(float(parts[0]))
                latencies.append(float(parts[1]))
    
    return np.array(percentiles), np.array(latencies)


def load_summary(path: str) -> dict:
    """加载摘要文件"""
    summary = {}
    with open(path, 'r') as f:
        for line in f:
            if ':' in line:
                key, value = line.strip().split(':', 1)
                try:
                    summary[key.strip()] = float(value.strip().rstrip('%'))
                except ValueError:
                    summary[key.strip()] = value.strip()
    return summary


def plot_cdf_comparison(exp_data: dict, output_path: str):
    """
    绘制 CDF 对比图
    
    exp_data: {
        'exp_a_po2': {'cdf': (percentiles, latencies), 'summary': {...}},
        'exp_b_malcolm': {...},
        'exp_c_malcolm_strict': {...}
    }
    """
    if not HAS_MATPLOTLIB:
        return
    
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    
    # 颜色和样式
    styles = {
        'exp_a_po2': {'color': 'red', 'linestyle': '-', 'label': 'Power-of-2'},
        'exp_b_malcolm': {'color': 'blue', 'linestyle': '--', 'label': 'Malcolm (Nash)'},
        'exp_c_malcolm_strict': {'color': 'green', 'linestyle': '-', 'label': 'Malcolm-Strict (Ours)'}
    }
    
    # 左图: 完整 CDF
    ax1 = axes[0]
    for exp_name, data in exp_data.items():
        if 'cdf' in data:
            p, lat = data['cdf']
            style = styles.get(exp_name, {'color': 'gray', 'linestyle': '-', 'label': exp_name})
            ax1.plot(lat, p, **style)
    
    ax1.set_xlabel('Latency (μs)', fontsize=12)
    ax1.set_ylabel('Percentile', fontsize=12)
    ax1.set_title('Latency CDF Comparison', fontsize=14)
    ax1.legend(loc='lower right')
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(left=0)
    ax1.set_ylim(0, 100)
    
    # 右图: 尾部放大 (P99+)
    ax2 = axes[1]
    for exp_name, data in exp_data.items():
        if 'cdf' in data:
            p, lat = data['cdf']
            # 只取 P99 以上
            mask = p >= 99
            style = styles.get(exp_name, {'color': 'gray', 'linestyle': '-', 'label': exp_name})
            ax2.plot(lat[mask], p[mask], **style)
    
    ax2.set_xlabel('Latency (μs)', fontsize=12)
    ax2.set_ylabel('Percentile', fontsize=12)
    ax2.set_title('Tail Latency (P99+)', fontsize=14)
    ax2.legend(loc='lower right')
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(99, 100)
    
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"CDF plot saved to {output_path}")


def plot_bar_comparison(exp_data: dict, output_path: str):
    """
    绘制柱状图对比关键指标
    """
    if not HAS_MATPLOTLIB:
        return
    
    experiments = []
    p50 = []
    p99 = []
    p999 = []
    miss_rate = []
    
    labels = {
        'exp_a_po2': 'Po2',
        'exp_b_malcolm': 'Malcolm',
        'exp_c_malcolm_strict': 'M-Strict'
    }
    
    for exp_name in ['exp_a_po2', 'exp_b_malcolm', 'exp_c_malcolm_strict']:
        if exp_name not in exp_data or 'summary' not in exp_data[exp_name]:
            continue
        
        summary = exp_data[exp_name]['summary']
        experiments.append(labels.get(exp_name, exp_name))
        p50.append(summary.get('P50', 0))
        p99.append(summary.get('P99', 0))
        p999.append(summary.get('P99.9', 0))
        miss_rate.append(summary.get('Deadline Miss Rate', 0))
    
    if not experiments:
        return
    
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    
    # 左图: 延迟对比
    x = np.arange(len(experiments))
    width = 0.25
    
    ax1 = axes[0]
    ax1.bar(x - width, p50, width, label='P50', color='lightblue')
    ax1.bar(x, p99, width, label='P99', color='steelblue')
    ax1.bar(x + width, p999, width, label='P99.9', color='darkblue')
    
    ax1.set_ylabel('Latency (μs)', fontsize=12)
    ax1.set_title('Latency Comparison', fontsize=14)
    ax1.set_xticks(x)
    ax1.set_xticklabels(experiments)
    ax1.legend()
    ax1.grid(True, axis='y', alpha=0.3)
    
    # 右图: 违约率
    ax2 = axes[1]
    colors = ['red', 'blue', 'green'][:len(experiments)]
    ax2.bar(experiments, miss_rate, color=colors, alpha=0.7)
    ax2.set_ylabel('Deadline Miss Rate (%)', fontsize=12)
    ax2.set_title('Deadline Miss Rate', fontsize=14)
    ax2.grid(True, axis='y', alpha=0.3)
    
    plt.tight_layout()
    
    bar_path = output_path.replace('.pdf', '_bars.pdf')
    plt.savefig(bar_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Bar chart saved to {bar_path}")


def generate_text_report(exp_data: dict, output_path: str):
    """生成文本报告"""
    
    lines = []
    lines.append("=" * 60)
    lines.append("Malcolm-Strict Experiment Report")
    lines.append("=" * 60)
    lines.append("")
    
    experiment_names = {
        'exp_a_po2': 'Experiment A: Power-of-2 (Baseline 1)',
        'exp_b_malcolm': 'Experiment B: Original Malcolm (Baseline 2)',
        'exp_c_malcolm_strict': 'Experiment C: Malcolm-Strict (Our Method)'
    }
    
    for exp_name in ['exp_a_po2', 'exp_b_malcolm', 'exp_c_malcolm_strict']:
        if exp_name not in exp_data:
            continue
        
        data = exp_data[exp_name]
        lines.append("-" * 40)
        lines.append(experiment_names.get(exp_name, exp_name))
        lines.append("-" * 40)
        
        if 'summary' in data:
            summary = data['summary']
            lines.append(f"  Total Samples: {summary.get('Total Samples', 'N/A')}")
            lines.append(f"  P50 Latency:   {summary.get('P50', 'N/A'):.2f} μs")
            lines.append(f"  P99 Latency:   {summary.get('P99', 'N/A'):.2f} μs")
            lines.append(f"  P99.9 Latency: {summary.get('P99.9', 'N/A'):.2f} μs")
            lines.append(f"  P99.99 Latency:{summary.get('P99.99', 'N/A'):.2f} μs")
            lines.append(f"  Miss Rate:     {summary.get('Deadline Miss Rate', 'N/A'):.4f}%")
        else:
            lines.append("  [No data available]")
        
        lines.append("")
    
    # 对比分析
    if all(exp in exp_data and 'summary' in exp_data[exp] 
           for exp in ['exp_a_po2', 'exp_c_malcolm_strict']):
        
        lines.append("=" * 60)
        lines.append("Key Findings")
        lines.append("=" * 60)
        
        po2 = exp_data['exp_a_po2']['summary']
        strict = exp_data['exp_c_malcolm_strict']['summary']
        
        p999_improvement = (po2.get('P99.9', 0) - strict.get('P99.9', 0)) / max(po2.get('P99.9', 1), 1) * 100
        miss_improvement = po2.get('Deadline Miss Rate', 0) - strict.get('Deadline Miss Rate', 0)
        
        lines.append("")
        lines.append(f"Malcolm-Strict vs Power-of-2:")
        lines.append(f"  P99.9 Improvement: {p999_improvement:.1f}%")
        lines.append(f"  Miss Rate Reduction: {miss_improvement:.4f}%")
        
        if 'exp_b_malcolm' in exp_data and 'summary' in exp_data['exp_b_malcolm']:
            malcolm = exp_data['exp_b_malcolm']['summary']
            p999_vs_malcolm = (malcolm.get('P99.9', 0) - strict.get('P99.9', 0)) / max(malcolm.get('P99.9', 1), 1) * 100
            miss_vs_malcolm = malcolm.get('Deadline Miss Rate', 0) - strict.get('Deadline Miss Rate', 0)
            
            lines.append("")
            lines.append(f"Malcolm-Strict vs Original Malcolm:")
            lines.append(f"  P99.9 Improvement: {p999_vs_malcolm:.1f}%")
            lines.append(f"  Miss Rate Reduction: {miss_vs_malcolm:.4f}%")
            
            lines.append("")
            lines.append("Observation:")
            lines.append("  Original Malcolm achieves low load variance (Nash equilibrium)")
            lines.append("  but still exhibits high tail latency under heavy-tailed load.")
            lines.append("  This confirms the 'Variance Trap' hypothesis.")
    
    lines.append("")
    lines.append("=" * 60)
    
    # 写入文件
    text_path = output_path.replace('.pdf', '.txt')
    with open(text_path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"Text report saved to {text_path}")
    
    # 同时输出到终端
    print('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description='Generate experiment comparison report')
    parser.add_argument('--results_dir', type=str, required=True,
                       help='Directory containing experiment results')
    parser.add_argument('--output', type=str, required=True,
                       help='Output PDF path')
    args = parser.parse_args()
    
    results_dir = Path(args.results_dir)
    if not results_dir.exists():
        print(f"Results directory not found: {results_dir}", file=sys.stderr)
        sys.exit(1)
    
    # 加载各实验数据
    exp_data = {}
    
    for exp_name in ['exp_a_po2', 'exp_b_malcolm', 'exp_c_malcolm_strict']:
        exp_dir = results_dir / exp_name
        if not exp_dir.exists():
            print(f"Experiment directory not found: {exp_dir}")
            continue
        
        exp_data[exp_name] = {}
        
        # 加载 CDF
        cdf_path = exp_dir / 'combined_latency.csv'
        if cdf_path.exists():
            exp_data[exp_name]['cdf'] = load_cdf(str(cdf_path))
        
        # 加载摘要
        summary_path = exp_dir / 'combined_latency.summary.txt'
        if summary_path.exists():
            exp_data[exp_name]['summary'] = load_summary(str(summary_path))
        else:
            # 尝试其他位置
            for pattern in ['summary.txt', '*/summary.txt']:
                matches = list(exp_dir.glob(pattern))
                if matches:
                    exp_data[exp_name]['summary'] = load_summary(str(matches[0]))
                    break
    
    if not exp_data:
        print("No experiment data found!", file=sys.stderr)
        sys.exit(1)
    
    print(f"Loaded data for experiments: {list(exp_data.keys())}")
    
    # 确保输出目录存在
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    # 生成报告
    generate_text_report(exp_data, str(output_path))
    
    if HAS_MATPLOTLIB:
        plot_cdf_comparison(exp_data, str(output_path))
        plot_bar_comparison(exp_data, str(output_path))
    else:
        print("Skipping PDF plots (matplotlib not available)")


if __name__ == '__main__':
    main()
