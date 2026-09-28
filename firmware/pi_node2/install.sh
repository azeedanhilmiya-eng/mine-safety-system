#!/usr/bin/env bash
# 矿安智联 节点2 一键安装 (在树莓派上运行):  cd ~/rpi-node2 && bash install.sh
# 做的事: 打开 SPI -> 装 spidev/lgpio -> 离线自测 -> 安装(但不启用)开机自启服务
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="$(id -un)"
echo "== 用户: $USER_NAME  目录: $DIR"

echo "== 1/5 打开 SPI"
sudo raspi-config nonint do_spi 0

echo "== 2/5 安装依赖 python3-spidev python3-lgpio"
sudo apt-get update -q
sudo apt-get install -y python3-spidev python3-lgpio

# 首个用户默认就在 spi/gpio 组; 换过用户时补上, 下次登录生效
for g in spi gpio; do
  if getent group "$g" >/dev/null && ! id -nG "$USER_NAME" | grep -qw "$g"; then
    sudo usermod -aG "$g" "$USER_NAME"
    echo "   已把 $USER_NAME 加入 $g 组 (重新登录后生效)"
  fi
done

echo "== 3/5 离线自测"
(cd "$DIR" && python3 -m unittest -q test_node2)

echo "== 4/5 Python 环境（asr_front.py 用，含 sherpa-onnx）"
if [ ! -x "$DIR/venv/bin/python" ]; then
  # --system-site-packages: 让 venv 能看到 apt 装的 spidev / lgpio
  python3 -m venv --system-site-packages "$DIR/venv"
  "$DIR/venv/bin/pip" install -q -i https://pypi.tuna.tsinghua.edu.cn/simple --upgrade pip
  "$DIR/venv/bin/pip" install -q -i https://pypi.tuna.tsinghua.edu.cn/simple sherpa-onnx numpy
  echo "   已创建 $DIR/venv 并安装 sherpa-onnx"
else
  echo "   venv 已存在，跳过"
fi
if [ ! -f "$HOME/models/paraformer-zh/model.int8.onnx" ]; then
  echo "   注意：还没下载中文识别模型，先执行："
  echo "     mkdir -p ~/models/paraformer-zh && cd ~/models/paraformer-zh"
  echo "     wget https://hf-mirror.com/csukuangfj/sherpa-onnx-paraformer-zh-2023-09-14/resolve/main/model.int8.onnx"
  echo "     wget https://hf-mirror.com/csukuangfj/sherpa-onnx-paraformer-zh-2023-09-14/resolve/main/tokens.txt"
fi

echo "== 5/5 安装 systemd 服务 node2 (暂不启用)"
sed -e "s|@USER@|$USER_NAME|g" -e "s|@DIR@|$DIR|g" "$DIR/node2.service" \
  | sudo tee /etc/systemd/system/node2.service >/dev/null
sudo systemctl daemon-reload

echo
if [ -e /dev/spidev0.0 ]; then
  echo "完成。接好 Ra-02 后先手动联调:  python3 node2.py -v"
else
  echo "完成, 但 /dev/spidev0.0 还不存在: 请先 sudo reboot, 重启后再 python3 node2.py -v"
fi
echo "联调通过后开机自启:  sudo systemctl enable --now node2"
