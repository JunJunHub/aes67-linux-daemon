#!/usr/bin/env bash
# daemon/noise/tests/download_models.sh
# Spec5 Task 2：下载 DTLN + DeepFilterNet3 ONNX 模型到 noise_models/（CI 缓存，
# 不进 git）。denoise 测试（dtln_denoises_speech / dfn_denoises_nonstation 等）
# 在模型缺失时 BOOST_SKIP，故本脚本非强制；下载后相关 case 才实跑。
#
# 目录布局（与 adapter init 推导一致）：
#   noise_models/dtln/{model_1.onnx, model_2.onnx}
#   noise_models/deepfilternet/{enc.onnx, df_dec.onnx, erb_dec.onnx}
#
# 用法：./daemon/noise/tests/download_models.sh [目标目录]（默认 ./noise_models）
set -euo pipefail

DEST="${1:-./noise_models}"
DTLN_DIR="$DEST/dtln"
DFN_DIR="$DEST/deepfilternet"
mkdir -p "$DTLN_DIR" "$DFN_DIR"

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
# tarball 解压后含 ./tmp/export/{enc,df_dec,erb_dec}.onnx（与配置 ini 同目录）。
download_dfn() {
  if [ -s "$DFN_DIR/enc.onnx" ] && [ -s "$DFN_DIR/df_dec.onnx" ] && \
     [ -s "$DFN_DIR/erb_dec.onnx" ]; then
    echo "[dfn] 三子图已存在，跳过"
    return 0
  fi
  local tmp; tmp="$(mktemp -d)"
  echo "[dfn] 下载 DeepFilterNet3_onnx.tar.gz ..."
  # 尝试多个已知 release URL（tag 可能迁移）。
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
  # 在解压目录中查找三子图（路径可能为 tmp/export/ 或平铺）。
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

echo "目标目录：$DEST"
download_dtln || echo "[warn] DTLN 模型下载失败，相关测试将 SKIP"
download_dfn   || echo "[warn] DFN 模型下载失败，相关测试将 SKIP"
echo "完成。dtln_denoises_speech / dfn_denoises_nonstation 等需模型的 case 现可实跑。"
