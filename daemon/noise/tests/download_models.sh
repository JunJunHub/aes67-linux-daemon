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

# ── YAMNet：预转换 ONNX 直接下载（15MB，固定 3s 输入 = 48000 样本）────────
# YAMNet ONNX 模型（yamnet_3s.onnx）由 tf2onnx 从 TF Hub SavedModel 导出，
# 输入固定 [1, 48000]（3s @ 16kHz），输出 scores [6,521] + embeddings [6,1024]。
# 直接下载预转换 ONNX，无需本地 TF + tf2onnx 转换（省 ~4GB 内存）。
# 同时下载 class_map.csv（AudioSet 521 类类名，~14KB）。
download_yamnet() {
  local onnx_path="$YAMNET_DIR/yamnet_3s.onnx"
  local csv_path="$YAMNET_DIR/yamnet_class_map.csv"

  # class_map.csv：从 GitHub 直接下载
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

  echo "[yamnet] 下载 yamnet_3s.onnx (15MB) ..."
  # 预转换 ONNX 直接下载（MD5: 689a919ec4c6c2375dc2e88962e746e5）
  "${CURL[@]}" -o "$onnx_path" \
    "https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yamnet/yamnet_3s.onnx" \
    || { echo "[yamnet] 下载失败（网络？链接过期？）"; \
         echo "  备选：从 TF Hub SavedModel 本地转换（需 tensorflow-hub + tf2onnx，~4GB 内存）"; \
         return 1; }

  # 校验文件大小（至少 14MB）
  local sz; sz="$(stat -c%s "$onnx_path" 2>/dev/null || echo 0)"
  if [ "$sz" -lt 14000000 ]; then
    echo "[yamnet] 下载文件不完整（$sz bytes），删除"
    rm -f "$onnx_path"; return 1
  fi
  echo "[yamnet] yamnet_3s.onnx 已就绪：$onnx_path ($sz bytes)"
}

echo "目标目录：$DEST"
download_dtln   || echo "[warn] DTLN 模型下载失败，相关测试将 SKIP"
download_dfn    || echo "[warn] DFN 模型下载失败，相关测试将 SKIP"
download_yamnet || echo "[warn] YAMNet 模型下载/转换失败，L3 分类测试将 SKIP"
echo "完成。模型目录布局："
echo "  $DEST/dtln/{model_1.onnx, model_2.onnx}"
echo "  $DEST/deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}"
echo "  $DEST/yamnet/{yamnet_3s.onnx, yamnet_class_map.csv}"
