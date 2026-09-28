#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
sudo apt update
sudo apt install -y python3-venv python3-pip python3-lgpio libsndfile1 portaudio19-dev python3-dev
python3 -m venv --system-site-packages .venv
.venv/bin/python -m pip install --prefer-binary --timeout 60 --retries 3 -r requirements.txt
if [[ ! -f config.json ]]; then
  cp config.example.json config.json
fi
mkdir -p models logs state recordings
printf '%s\n' '安装完成。下一步请修改 config.json，并把模型文件放入 models/。'
