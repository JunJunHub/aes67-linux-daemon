#!/usr/bin/env bash
# daemon/noise/tests/download_models.sh
# 下载 DTLN + DeepFilterNet3 + YAMNet ONNX 模型到 noise_models/
# （CI 缓存，不进 git）。denoise 测试和 ML 分类测试在模型缺失时 BOOST_SKIP。
#
# 目录布局（与 adapter init 推导一致）：
#   noise_models/dtln/{model_1.onnx, model_2.onnx}
#   noise_models/deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}
#   noise_models/yamnet/{yamnet_3s.onnx, yamnet_class_map.csv}
#
# adapter 路径推导：onnx_model_dir 指向 noise_models/，各 adapter 先找
# <dir>/<plugin_name>/ 子目录，再回退到 <dir>/ 平铺。
#
# 用法：./daemon/noise/tests/download_models.sh [目标目录]（默认 ./noise_models）
set -euo pipefail

DEST="${1:-./noise_models}"
DTLN_DIR="$DEST/dtln"
DFN_DIR="$DEST/deepfilternet"
YAMNET_DIR="$DEST/yamnet"
mkdir -p "$DTLN_DIR" "$DFN_DIR" "$YAMNET_DIR"

CURL=(curl -L --fail --retry 3 -C -)

# ── DTLN：breizhn/DTLN 仓库 pretrained_model/{model_1,model_2}.onnx ──────────
download_dtln() {
  for m in model_1 model_2; do
    if [ -s "$DTLN_DIR/$m.onnx" ]; then
      echo "[dtln] $m.onnx 已存在，跳过"
      continue
    fi
    echo "[dtln] 下载 $m.onnx ..."
    "${CURL[@]}" -o "$DTLN_DIR/$m.onnx" \
      "https://github.com/breizhn/DTLN/raw/main/pretrained_model/$m.onnx" \
      || { echo "[dtln] 下载 $m.onnx 失败（网络？）"; return 1; }
  done
}

# ── DeepFilterNet3：官方 release tarball，解压出 enc/df_dec/erb_dec.onnx ─────
download_dfn() {
  if [ -s "$DFN_DIR/enc.onnx" ] && [ -s "$DFN_DIR/df_dec.onnx" ] && \
     [ -s "$DFN_DIR/erb_dec.onnx" ]; then
    echo "[dfn] 三子图已存在，跳过"
    return 0
  fi
  local tmp; tmp="$(mktemp -d)"
  echo "[dfn] 下载 DeepFilterNet3_onnx.tar.gz ..."
  local urls=(
    "https://github.com/Rikorose/DeepFilterNet/releases/download/v0.3.1/DeepFilterNet3_onnx.tar.gz"
    "https://github.com/Rikorose/DeepFilterNet/releases/latest/download/DeepFilterNet3_onnx.tar.gz"
  )
  local ok=0
  for u in "${urls[@]}"; do
    if "${CURL[@]}" -o "$tmp/dfn.tar.gz" "$u" 2>/dev/null; then ok=1; break; fi
  done
  if [ "$ok" -eq 0 ]; then
    echo "[dfn] 下载 DeepFilterNet3_onnx.tar.gz 失败（网络？tag 迁移？）"
    rm -rf "$tmp"; return 1
  fi
  tar xzf "$tmp/dfn.tar.gz" -C "$tmp"
  for sub in enc df_dec erb_dec; do
    local f; f="$(find "$tmp" -name "$sub.onnx" | head -1)"
    if [ -z "$f" ]; then
      echo "[dfn] 解压后未找到 $sub.onnx"; rm -rf "$tmp"; return 1
    fi
    cp "$f" "$DFN_DIR/$sub.onnx"
  done
  rm -rf "$tmp"
  echo "[dfn] 三子图已就绪：$DFN_DIR/{enc,df_dec,erb_dec}.onnx"
}

# ── YAMNet：TF Hub SavedModel -> ONNX（固定 3s 输入 = 48000 样本）────────────
# YAMNet 官方提供 TF SavedModel（变量长度输入），需用 tf2onnx 转换为固定
# 输入 [1, 48000] 的 ONNX。同时下载 class_map.csv（AudioSet 521 类类名）。
#
# 依赖：tensorflow-hub + tensorflow-cpu + tf2onnx（pip install）
# 国内网络可能较慢（TF Hub 在 Google 域名），可手动下载后放到 $YAMNET_DIR。
download_yamnet() {
  local onnx_path="$YAMNET_DIR/yamnet_3s.onnx"
  local csv_path="$YAMNET_DIR/yamnet_class_map.csv"

  # class_map.csv：从 GitHub 直接下载（小文件，~14KB）
  if [ ! -s "$csv_path" ]; then
    echo "[yamnet] 下载 yamnet_class_map.csv ..."
    "${CURL[@]}" -o "$csv_path" \
      "https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv" \
      || { echo "[yamnet] class_map.csv 下载失败"; return 1; }
  else
    echo "[yamnet] yamnet_class_map.csv 已存在，跳过"
  fi

  if [ -s "$onnx_path" ]; then
    echo "[yamnet] yamnet_3s.onnx 已存在，跳过"
    return 0
  fi

  echo "[yamnet] 从 TF Hub 下载 YAMNet SavedModel 并转换为 ONNX ..."
  echo "  （需 tensorflow-hub + tensorflow-cpu + tf2onnx）"

  local py; py="$(command -v python3 || command -v python)"

  # 检查依赖
  if ! "$py" -c "import tensorflow_hub; import tf2onnx" 2>/dev/null; then
    echo "[yamnet] 安装转换依赖 ..."
    "$py" -m pip install tensorflow-hub tensorflow-cpu tf2onnx 2>&1 | tail -3
  fi

  # 内联转换脚本：加载 TF Hub SavedModel -> 固定输入 -> tf2onnx 导出
  "$py" - "$onnx_path" << 'PYEOF' || { echo "[yamnet] ONNX 转换失败"; return 1; }
import sys, os, tempfile, shutil
import numpy as np

onnx_path = sys.argv[1]

# 1. 加载 YAMNet SavedModel from TF Hub
os.environ.setdefault('TFHUB_CACHE_DIR', tempfile.mkdtemp())
import tensorflow_hub as hub
print("[yamnet] 加载 TF Hub SavedModel ...")
model = hub.load("https://tfhub.dev/google/yamnet/1")

# 2. 用 concrete function 固定输入形状 [1, 48000]（3s @ 16kHz）
import tensorflow as tf
waveform = tf.TensorSpec(shape=[1, 48000], dtype=tf.float32, name="new_input")
cf = model.signatures["serving_default"].get_concrete_function(waveform=waveform)

# 3. tf2onnx 转换
import tf2onnx
print("[yamnet] 转换为 ONNX ...")
model_proto, _ = tf2onnx.convert.from_function(
    cf, input_signature=[waveform], output_path=onnx_path, opset=13
)
print(f"[yamnet] ONNX 已保存: {onnx_path} ({os.path.getsize(onnx_path)} bytes)")
PYEOF

  echo "[yamnet] yamnet_3s.onnx 已就绪：$onnx_path"
}

echo "目标目录：$DEST"
download_dtln   || echo "[warn] DTLN 模型下载失败，相关测试将 SKIP"
download_dfn    || echo "[warn] DFN 模型下载失败，相关测试将 SKIP"
download_yamnet || echo "[warn] YAMNet 模型下载/转换失败，L3 分类测试将 SKIP"
echo "完成。模型目录布局："
echo "  $DEST/dtln/{model_1.onnx, model_2.onnx}"
echo "  $DEST/deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}"
echo "  $DEST/yamnet/{yamnet_3s.onnx, yamnet_class_map.csv}"
