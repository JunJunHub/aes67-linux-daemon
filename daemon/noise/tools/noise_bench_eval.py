#!/usr/bin/env python3
"""noise_bench_eval.py - 降噪质量对比评测

从 VoiceBank-DEMAND 测试集中采样 N 对 clean/noisy 文件，
用 noise-bench 分别跑 RNNoise/DTLN/DFN，计算 PESQ/STOI，输出对比表。

用法:
  python3 noise_bench_eval.py --bench ./build/noise-bench \
    --dataset /path/to/VoiceBank-DEMAND \
    --model-dir ../noise_models \
    --num-files 30
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from pesq import pesq
from pystoi import stoi


def select_files(dataset_dir, num_files):
    """均匀采样 num_files 对 clean/noisy 文件。"""
    clean_dir = Path(dataset_dir) / "clean_testset_wav"
    noisy_dir = Path(dataset_dir) / "noisy_testset_wav"
    all_files = sorted(f.name for f in clean_dir.iterdir() if f.suffix == ".wav")
    if len(all_files) > num_files:
        step = len(all_files) / num_files
        selected = [all_files[int(i * step)] for i in range(num_files)]
    else:
        selected = all_files
    return [(clean_dir / f, noisy_dir / f) for f in selected]


def run_bench(bench, plugin, noisy_wav, out_wav, model_dir):
    """运行 noise-bench 处理单个文件。"""
    cmd = [bench, "--plugin", plugin, "--input", str(noisy_wav),
           "--output", str(out_wav), "--threads", "1"]
    if model_dir and plugin != "rnnoise":
        sub_dir = "dtln" if plugin == "dtln" else "deepfilternet"
        cmd += ["--model-dir", str(Path(model_dir) / sub_dir)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr.strip()}", file=sys.stderr)
        return None
    # 解析 RTF
    for line in result.stdout.splitlines():
        if "RTF:" in line:
            rtf = float(line.split("RTF:")[1].strip())
            return rtf
    return None


def evaluate(clean_wav, denoised_wav):
    """计算 PESQ 和 STOI（互相关自动对齐）。"""
    clean, sr_c = sf.read(str(clean_wav))
    denoised, sr_d = sf.read(str(denoised_wav))
    if sr_c != 48000 or sr_d != 48000:
        print(f"  WARN: sample rate mismatch sr_c={sr_c} sr_d={sr_d}",
              file=sys.stderr)
    n = min(len(clean), len(denoised))
    clean = clean[:n].astype(np.float64)
    denoised = denoised[:n].astype(np.float64)

    # 互相关自动对齐（补偿算法延迟）
    from scipy.signal import correlate
    corr = correlate(clean, denoised, mode='full')
    lag = np.argmax(np.abs(corr)) - (len(denoised) - 1)
    # lag > 0: denoised 提前 lag -> 去掉 clean 前 lag 个
    # lag < 0: denoised 延迟 |lag| -> 去掉 denoised 前 |lag| 个
    if lag > 0:
        clean = clean[lag:]
        denoised = denoised[:len(clean)]
    elif lag < 0:
        denoised = denoised[-lag:]
        clean = clean[:len(denoised)]

    n = min(len(clean), len(denoised))
    clean = clean[:n]
    denoised = denoised[:n]

    # PESQ: 48k -> 16k（pesq 库仅支持 8k/16k）
    from scipy.signal import resample_poly
    clean_16k = resample_poly(clean, 1, 3)
    den_16k = resample_poly(denoised, 1, 3)
    try:
        p = pesq(16000, clean_16k, den_16k, "wb")
    except Exception:
        p = float("nan")
    # STOI: 16k
    try:
        s = stoi(clean_16k, den_16k, 16000, extended=False)
    except Exception:
        s = float("nan")
    # SNR 估计
    noise = clean - denoised
    snr = 10 * np.log10(np.sum(clean**2) / (np.sum(noise**2) + 1e-12) + 1e-12)
    return p, s, snr


def main():
    parser = argparse.ArgumentParser(description="Noise denoise evaluation")
    parser.add_argument("--bench", required=True, help="noise-bench binary")
    parser.add_argument("--dataset", required=True,
                        help="VoiceBank-DEMAND directory")
    parser.add_argument("--model-dir", default="../noise_models",
                        help="ONNX model directory")
    parser.add_argument("--num-files", type=int, default=30,
                        help="Number of file pairs to evaluate")
    parser.add_argument("--out-dir", default="/tmp/bench_output",
                        help="Output directory for denoised WAVs")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    file_pairs = select_files(args.dataset, args.num_files)
    print(f"Selected {len(file_pairs)} file pairs from {args.dataset}")

    plugins = ["rnnoise", "dtln", "deepfilternet"]
    results = {p: {"pesq": [], "stoi": [], "snr": [], "rtf": []} for p in plugins}

    # 先评估 noisy（baseline）
    noisy_results = {"pesq": [], "stoi": [], "snr": []}
    print("\n=== Evaluating noisy baseline ===")
    for i, (clean_wav, noisy_wav) in enumerate(file_pairs):
        p, s, snr = evaluate(clean_wav, noisy_wav)
        noisy_results["pesq"].append(p)
        noisy_results["stoi"].append(s)
        noisy_results["snr"].append(snr)
        if (i + 1) % 10 == 0:
            print(f"  {i+1}/{len(file_pairs)}")

    for plugin in plugins:
        print(f"\n=== Processing with {plugin} ===")
        for i, (clean_wav, noisy_wav) in enumerate(file_pairs):
            out_wav = out_dir / f"{plugin}_{clean_wav.name}"
            rtf = run_bench(args.bench, plugin, noisy_wav, out_wav,
                            args.model_dir)
            if rtf is None:
                continue
            p, s, snr = evaluate(clean_wav, out_wav)
            results[plugin]["pesq"].append(p)
            results[plugin]["stoi"].append(s)
            results[plugin]["snr"].append(snr)
            results[plugin]["rtf"].append(rtf)
            if (i + 1) % 10 == 0:
                print(f"  {i+1}/{len(file_pairs)}")

    # 汇总
    print("\n" + "=" * 70)
    print(f"{'Metric':<12} {'Noisy':>10} {'RNNoise':>10} {'DTLN':>10} {'DFN':>10}")
    print("-" * 70)

    def fmt(vals):
        return f"{np.nanmean(vals):.3f} ± {np.nanstd(vals):.3f}"

    print(f"{'PESQ':<12} {fmt(noisy_results['pesq']):>10} "
          f"{fmt(results['rnnoise']['pesq']):>10} "
          f"{fmt(results['dtln']['pesq']):>10} "
          f"{fmt(results['deepfilternet']['pesq']):>10}")
    print(f"{'STOI':<12} {fmt(noisy_results['stoi']):>10} "
          f"{fmt(results['rnnoise']['stoi']):>10} "
          f"{fmt(results['dtln']['stoi']):>10} "
          f"{fmt(results['deepfilternet']['stoi']):>10}")
    print(f"{'SNR(dB)':<12} {fmt(noisy_results['snr']):>10} "
          f"{fmt(results['rnnoise']['snr']):>10} "
          f"{fmt(results['dtln']['snr']):>10} "
          f"{fmt(results['deepfilternet']['snr']):>10}")
    print(f"{'RTF':<12} {'':>10} "
          f"{fmt(results['rnnoise']['rtf']):>10} "
          f"{fmt(results['dtln']['rtf']):>10} "
          f"{fmt(results['deepfilternet']['rtf']):>10}")
    print("=" * 70)

    # JSON 输出
    json_path = out_dir / "results.json"
    with open(json_path, "w") as f:
        json.dump({
            "noisy": noisy_results,
            "plugins": results,
            "num_files": len(file_pairs),
        }, f, indent=2, default=str)
    print(f"\nResults saved to {json_path}")


if __name__ == "__main__":
    main()
