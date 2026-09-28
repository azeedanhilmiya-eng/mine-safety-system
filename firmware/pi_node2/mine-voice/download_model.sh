#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
MODEL_NAME="sherpa-onnx-paraformer-zh-2023-09-14"
ARCHIVE="${MODEL_NAME}.tar.bz2"
URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/${ARCHIVE}"

mkdir -p models
if [[ -f models/model.int8.onnx && -f models/tokens.txt ]]; then
  printf '%s\n' '模型已经存在，无需重复下载。'
  exit 0
fi

printf '正在下载中文 Paraformer 模型：%s\n' "$URL"
curl -L --fail --retry 3 --continue-at - --output "models/${ARCHIVE}" "$URL"
tar -xjf "models/${ARCHIVE}" -C models

MODEL_DIR="models/${MODEL_NAME}"
MODEL_FILE="$(find "$MODEL_DIR" -maxdepth 2 -type f -name 'model.int8.onnx' -print -quit)"
TOKENS_FILE="$(find "$MODEL_DIR" -maxdepth 2 -type f -name 'tokens.txt' -print -quit)"
if [[ -z "$MODEL_FILE" || -z "$TOKENS_FILE" ]]; then
  printf '%s\n' '模型压缩包中没有找到 model.int8.onnx 或 tokens.txt。' >&2
  exit 1
fi

ln -sfn "${MODEL_FILE#models/}" models/model.int8.onnx
ln -sfn "${TOKENS_FILE#models/}" models/tokens.txt
printf '%s\n' '模型准备完成：models/model.int8.onnx 和 models/tokens.txt'
