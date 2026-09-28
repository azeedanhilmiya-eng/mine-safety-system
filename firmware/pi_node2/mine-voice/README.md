# 树莓派 5 日常语音 LoRa 节点 M2

本工程在树莓派 5 上完成按键录音、sherpa-onnx Paraformer 离线中文识别、UTF-8 安全分片和 SX1278 发送。音频不会通过 LoRa 发送；原有 VAD 模式仍可通过配置选用。

当前 `config.json` 已设置为 `lora.enabled: true`、`protocol.ack_enabled: true`，并每 5 秒发送一次心跳。服务会在离线识别后向网关发送纯文本 `TXT` 报文，识别结果同时写入 `logs/transcripts.jsonl`。

## 一 硬件接线

| SX1278 Ra-02 | 树莓派 5 | 物理针脚 |
|---|---|---:|
| VCC | 3.3 V | 17 |
| GND | GND | 20 |
| SCK | GPIO11 | 23 |
| MISO | GPIO9 | 21 |
| MOSI | GPIO10 | 19 |
| NSS | GPIO5 | 29 |
| RST | GPIO25 | 22 |
| DIO0 | 暂不使用 | - |

SX1278 只能使用 3.3 V。发射前必须接好 433 MHz 天线。建议在模块 VCC 与 GND 附近增加 100 μF 和 0.1 μF 去耦电容。树莓派 5 上不要把 NSS 接 GPIO8/CE0；该引脚由内核 SPI 驱动占用，而本工程的 CircuitPython 驱动需要用普通 GPIO 手动控制片选，因此使用 GPIO5。

### 录音按键

使用**两脚常开瞬时按钮**，接法如下；四脚轻触按钮应选按下时才导通的两侧，不要接同侧常通的两脚。

| 按键端子 | 树莓派 5 | 物理针脚 |
| --- | --- | ---: |
| 一端 | GPIO17 | 11 |
| 另一端 | GND | 9 |

GPIO17 使用内部上拉，**不接 3.3 V 或 5 V**。按下时 GPIO17 接地并开始录音，松开后约 30 毫秒结束录音并开始识别；小于 350 毫秒的按键录音会被忽略。单次录音安全上限为 30 秒，达到上限后等待按键松开，不会把一次长按当成多次录音。服务等待按键时仍会按 5 秒间隔向网关发送 M2 心跳。

## 二 安装

在树莓派终端中执行：

```bash
sudo raspi-config
```

进入 `Interface Options -> SPI -> Enable`，然后重启。将工程复制到树莓派：

```bash
cd ~
unzip mine_voice_node.zip
mv mine_voice_node mine-voice
cd ~/mine-voice
chmod +x install.sh
./install.sh
```

## 三 配置麦克风

列出录音设备：

```bash
cd ~/mine-voice
.venv/bin/python audio_test.py --list
```

如果 USB 麦克风不是系统默认输入，在 `config.json` 的 `audio.device` 中填入设备编号。测试按键录音：

```bash
sudo systemctl stop mine-voice
.venv/bin/python audio_test.py
aplay audio-test.wav
sudo systemctl start mine-voice
```

当前设备检测到的 USB 麦克风编号为 `0`，示例配置已经按此设置。可在接好 SX1278 后进行不发射的硬件检查：

```bash
sudo systemctl stop mine-voice
.venv/bin/python hardware_check.py
sudo systemctl start mine-voice
```

运行硬件检查前先停止 `mine-voice` 服务，避免与服务访问同一 SX1278；录音时服务也会占用 USB 麦克风。检查只初始化 SX1278，不发送报文。

当前 Newmine USB 麦克风使用 44100 Hz 采集；程序会自动重采样为离线模型需要的 16000 Hz，无需手动转换。

当前 `audio.trigger_mode` 为 `button`，无需调整 RMS 触发阈值。若切回 `vad` 模式，再按现场噪声调整 `start_rms` 和 `stop_rms`。

## 四 放置离线模型

工程按 sherpa-onnx Paraformer 模型编写。可直接运行下载脚本：

```bash
chmod +x download_model.sh
./download_model.sh
```

脚本使用 sherpa-onnx 官方发布的中文 Paraformer 模型，并建立 `models/model.int8.onnx` 与 `models/tokens.txt` 链接。也可以自行放置兼容模型并修改 `config.json`：

```json
"asr": {
  "model": "models/model.int8.onnx",
  "tokens": "models/tokens.txt"
}
```

不同模型压缩包的文件名可能不同，以实际下载内容为准。请确认模型许可符合比赛和演示用途。

测试现有 WAV 文件：

```bash
.venv/bin/python asr_test.py audio-test.wav
```

## 五 测试 LoRa

先确认地面网关参数完全一致：433 MHz、SF7、125 kHz、4/5、CRC 开、前导码 8、同步字 0xA3。

当前网关固件支持逐片 ACK，`config.json` 已开启回执检查。固定文本联调命令：

```bash
.venv/bin/python lora_test.py --text "三号巷道风机有异响"
```

网关应收到：

```text
TXT,M2,1,1/1,三号巷道风机有异响
```

文本较长时会变成多片。网关解析 TXT 报文时应最多切分前四个逗号，保留第五段中的原始文字。

网关回执格式为 `ACK,M2,<seq>,<part>`。程序逐片等待 ACK，超时后最多尝试 `max_attempts` 次。

## 六 运行完整节点

```bash
.venv/bin/python main.py --config config.json
```

程序流程为：等待按键、按住录音、松开后离线识别、生成 TXT 分片、逐片发送。按 `Ctrl+C` 退出。日志保存在 `logs/mine_voice.log`。

## 七 开机自启

服务文件已按当前树莓派用户 `zlm` 和目录 `/home/zlm/mine-voice` 配置。如果以后更改用户名或工程路径，需要同步修改服务文件。然后执行：

```bash
sudo cp systemd/mine-voice.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mine-voice
sudo systemctl status mine-voice
journalctl -u mine-voice -f
```

服务即使在模型缺失、麦克风未接或 SX1278 尚未接好的情况下也会保持运行，每 15 秒重新检测一次。硬件和模型准备好后无需重新开机，程序会自动进入工作状态。

查看开机服务的实时识别日志：

```bash
journalctl -u mine-voice -f
```

查看已保存的识别文本：

```bash
tail -f ~/mine-voice/logs/transcripts.jsonl
```

## 八 推荐调试顺序

1. `audio_test.py --list` 找到 USB 麦克风。
2. `audio_test.py` 能录到清楚语音。
3. `lora_test.py` 能让网关收到固定中文文字。
4. 下载模型，`asr_test.py` 能正确转写 WAV。
5. 运行 `main.py` 完成语音到 LoRa 全链路。
6. 网关实现 ACK 后再开启 ACK 重传。
7. 最后配置 systemd 自启动和两小时稳定性测试。

## 九 常见故障

- `No Default Input Device Available`：设置 `audio.device` 为 `audio_test.py --list` 显示的输入设备编号。
- 找不到 `/dev/spidev0.0`：SPI 未启用，重新运行 `raspi-config`。
- `Failed to find rfm9x`：检查 NSS、RST、SPI 接线、3.3 V 供电和共地。
- 网关收不到或乱码：检查频率、SF、带宽、编码率、CRC、前导码、同步字是否三端一致。
- 模型文件不存在：修改 `asr.model` 和 `asr.tokens`，不能只把模型压缩包放入目录。
- 识别错误多：先用清晰近讲录音验证，再调 VAD 阈值和更换适合普通话的模型。

## 十 运行单元测试

协议测试不需要树莓派硬件：

```bash
.venv/bin/python -m unittest discover -s tests -v
```
# OneNET 节点二直连

树莓派在原有 LoRa 链路之外，通过 Wi-Fi/MQTT 将转写结果发到 OneNET。
设备为现有 `esp32` 产品下的独立设备 `mine-voice-m2`；M1 设备配置不变。
按 `onenet_secrets.example.json` 建立 `onenet_secrets.json`，填入该设备的 Base64 设备密钥，
再将 `config.json` 的 `onenet.enabled` 设为 `true`。密钥文件已被 Git 忽略。
启动后 OneNET 设备连接状态表示树莓派 MQTT 连接；每次识别成功上报 `m2_text` 和
`m2_alarm`。`m2_lora_online`、`m2_rssi` 仍由地面网关负责上报，不能把 MQTT 在线
当作 LoRa 链路正常。离线期间仅保留最新一条待上报文本。
