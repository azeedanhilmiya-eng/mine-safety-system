# 井下节点2（树莓派 5 + Ra-02）—— 日常文本 LoRa 通道

> **当前树莓派部署使用 [`mine-voice/`](mine-voice/README.md)。** 其 Ra-02 NSS 接
> **GPIO5（物理针脚 29）**、RST 接 GPIO25（物理针脚 22），可运行
> `cd ~/mine-voice && .venv/bin/python hardware_check.py` 做不发射检查。
> 本目录根下的 `node2.py` / `sx1278.py` 是另一套使用 **GPIO8/CE0** 的旧传输实现，
> 与当前接线不兼容；不要在现有树莓派上运行其 `install.sh` 或 `node2.service`。

对应《矿山安全监测系统_双节点语音与LoRa通信实施方案》v2.0 第五章。
本目录目前实现的是**文本 → LoRa → 地面网关**这一段：分片、逐片回执、超时重发、心跳，以及给节点1 应急包让路。
离线 ASR（麦克风 → 文字）还没接入。ASR 程序只要每识别完一句就往标准输出打印一行，用管道接到 `node2.py` 即可，这边不用改代码。

```
USB 麦克风 → [ASR, 待做] → 一行一句文字 → node2.py → TXT 分片 → SX1278 ──433 MHz──→ 地面网关
                                                    ↑                                  │
                                                    └───────── ACK,M2,<序号>,<片号> ────┘
```

| 文件 | 作用 |
| --- | --- |
| `node2.py` | 主程序：读文字、分片发送、等回执、重发、心跳、信道礼让 |
| `sx1278.py` | SX1278 驱动（spidev + lgpio），射频参数与节点1、网关的 arduino-LoRa 默认值逐寄存器对齐 |
| `protocol.py` | 报文构造与解析、按 UTF-8 字符边界分片 |
| `test_node2.py` | 离线自测，在电脑上也能跑，不需要硬件 |
| `install.sh` | 一键安装：打开 SPI、装依赖、自测、装服务 |
| `node2.service` | systemd 服务模板，由 `install.sh` 填入用户名和目录 |

## 1. 接线（树莓派 5 40 针排针 ↔ Ra-02）

| Ra-02 引脚 | 树莓派物理针脚 | BCM 编号 / 功能 | 说明 |
| --- | ---: | --- | --- |
| 3.3V | 1 | 3V3 | **只能接 3.3 V**，不能接 5 V（针脚 2/4） |
| GND（4 个都接） | 6 / 9 / 20 / 25 | GND | |
| NSS | 24 | GPIO8 / SPI0 CE0 | 片选，由 SPI 驱动自动控制 |
| MOSI | 19 | GPIO10 / SPI0 MOSI | |
| MISO | 21 | GPIO9 / SPI0 MISO | |
| SCK | 23 | GPIO11 / SPI0 SCLK | |
| RESET | 22 | GPIO25 | 可选；不接时加 `--reset-gpio -1` |
| DIO0 | 18 | GPIO24 | **可不接**：程序轮询寄存器，不用中断 |
| DIO1~DIO5 | — | — | 不接 |

- 先接好 **433 MHz 天线**再运行程序，天线开路时发射可能损坏功放。
- Ra-02 发射瞬间电流约 100 mA 以上。在模块 3.3V–GND 之间就近并联 `100 nF` 和 `10~47 µF` 电容。如果出现"发送超时"或者 SX1278 反复检测不到，改用独立的 3.3 V 稳压给模块供电，并与树莓派共地。
- 树莓派本身使用 5 V / 5 A 官方电源并加主动散热，这是方案第五章的要求。

## 2. 树莓派系统准备

**烧录系统**（Raspberry Pi Imager）：
- 系统选 **Raspberry Pi OS Lite (64-bit)**，不需要桌面。
- 在"自定义设置"里填好用户名、密码和 Wi-Fi，并在"服务"页**打开 SSH**，这样才能从电脑拷文件、远程登录。
- 记下主机名，下面以 `raspberrypi` 为例。

**拷代码并安装**（在 Mac 终端里执行，`qupis` 换成你设置的用户名）：

```bash
scp -r rpi-node2 qupis@raspberrypi.local:~/
ssh qupis@raspberrypi.local
cd ~/rpi-node2 && bash install.sh
sudo reboot            # 第一次打开 SPI 后需要重启
```

`install.sh` 会做这几件事：打开 SPI，安装 `python3-spidev` 和 `python3-lgpio`，跑离线自测，最后安装开机自启服务，但暂不启用。
树莓派 5 上 `RPi.GPIO` 不可用，所以这里改用 `lgpio`。

## 3. 联调步骤

射频参数三端必须一致：433 MHz / SF7 / 125 kHz / CR 4-5 / CRC 开 / 前导码 8 / 同步字 `0xA3`。节点2 已按这组参数写死。

1. **地面网关**烧录新固件（`mine-safety-system/firmware/surface_node_vscode`，`pio run -t upload`），打开串口监视器。
2. **心跳**：在树莓派上运行
   ```bash
   python3 node2.py -v
   ```
   日志第一行应为 `SX1278 就绪`。几秒后网关串口出现 `[心跳] 节点2(M2) TXT_READY`，OLED 第三行变成 `节点2 在线 -xxdBm`。
3. **单句文本**：
   ```bash
   echo "三号巷道风机有异响，请派人检查" | python3 node2.py
   ```
   树莓派端依次打印 `[发送] … [回执] … [完成]`。网关串口打印 `[TXT] … [回执] ACK,M2,… [转写] …`，OLED 显示“节点2 语音转写”和正文，停留 10 秒。
4. **多片文本**：输入 40~60 个汉字的一句话，确认 2~3 片都被回执，网关拼出的句子完整、没有乱码。
5. **关键词升级**：输入“五号巷道有人被困”，网关按报警处理，蜂鸣器响、OLED 显示报警画面，30 秒后自动解除。
6. **与节点1 并发**：树莓派运行 `python3 node2.py --demo`，每 20 秒发一句。此时对节点1 说“屈展 → 紧急救命”，树莓派日志应出现 `[礼让] 听到 M1 应急包，暂停发送 3 s`，网关两路消息都能收到。
7. 交互输入：直接运行 `python3 node2.py`，每输入一行按回车就发一句，`Ctrl-D` 结束。

没有 Ra-02 时可以加 `--fake-radio` 在任何电脑上走一遍流程，`--fake-drop 0.4` 模拟 40% 丢包，用来看重发。

## 4. 开机自启

`install.sh` 已经按当前用户名和目录装好了服务。联调通过后执行：

```bash
sudo systemctl enable --now node2
journalctl -u node2 -f            # 看实时日志
```

服务默认以 `--demo` 模式运行，适合挂机测试 2 小时稳定性。接入 ASR 后，把 `node2.service` 里的 `ExecStart` 改成注释掉的那行管道写法，再重新运行一次 `install.sh`。

## 5. 协议与可靠性

| 报文 | 方向 | 例子 |
| --- | --- | --- |
| `TXT,<节点>,<序号>,<片号>/<总片数>,<正文>` | 井下 → 地面 | `TXT,M2,77,1/2,三号巷道风机有异响，` |
| `STATUS,<节点>,<状态>,<序号>` | 井下 → 地面 | `STATUS,M2,TXT_READY,78` |
| `ACK,<节点>,<序号>,<片号>` | 地面 → 井下 | `ACK,M2,77,1` |

- **分片**：每片正文 ≤ 80 字节（约 26 个汉字），只在 UTF-8 字符边界处切，尽量切在标点后面；每条消息最多 8 片，更长的文本会拆成多条消息。
- **回执与重发**：每片发出后等 2 s。收不到回执就在 0.2~0.8 s 随机退避后重发，单片最多重发 3 次，仍失败则放弃整条并记 ERROR 日志。
- **去重**：网关按“节点 + 序号 + 片号”去重。重复到达的片只回执、不重复显示。已完成的消息 60 s 内再收到分片也只回执。30 s 内收不齐的消息会被丢弃，并记录缺了哪几片。
- **信道礼让**：节点2 常驻接收，能听到节点1 的包。一旦听到 `VOICE` 应急包，暂停所有发送 3 s，让出网关给节点1 连发回执的那段时间。每次发送前还会检查信道上是否有正在进行的 LoRa 包（先听后发）。
- **心跳**：间隔在 4~6 s 之间随机，网关 15 s 收不到就判离线。加随机是为了避免和节点1 固定 5 s 的心跳长期撞在同一时刻。
- **序号**：开机时随机起点（1~9999），重启后不会和网关还记着的上一条消息序号撞车。

## 6. 还没做的

- 离线 ASR：已在本目录提供 `asr_front.py`（按键说话 → sherpa-onnx 识别 → 逐行输出给 `node2.py`）。
  另外把模型名称、版本和许可证写进提交清单。
- 发送日志落盘：加 `--log-file ~/rpi-node2/node2.log`，或者直接用 `journalctl` 导出。

## 7. 语音识别前端（本仓库新增）

`asr_front.py` 把"按键说话 → 离线识别"做成一行一句的标准输出，交给传输层：

```
USB 麦克风 → asr_front.py（按键/常开麦 + VAD + sherpa-onnx）→ 一行一句文字 → node2.py → LoRa
```

| 环境变量 | 默认 | 说明 |
| --- | --- | --- |
| `TRIGGER_MODE` | `button` | `button`＝按住 GPIO17 说话；`vad`＝常开麦克风自动切句 |
| `BUTTON_GPIO` | `17` | 按键一脚接该 GPIO，另一脚接 GND（内部上拉，无需外接电阻） |
| `RECORD_MAX_MS` | `8000` | 单次录音上限 |
| `AUDIO_DEVICE` | `plughw:CARD=Newmine,DEV=0` | USB 麦克风（用 `arecord -l` 确认名字） |
| `MIC_GAIN` | `1.0` | 麦克风偏轻时的软件增益（参考 `sox t.wav -n stat` 的 Volume adjustment） |
| `ASR_MODEL_DIR` | `~/models/paraformer-zh` | 中文模型目录 |

手动联调（不装服务）：

```bash
cd ~/pi_node2
MIC_GAIN=4 ./venv/bin/python -u asr_front.py 2>asr.log | /usr/bin/python3 -u node2.py --state ASR_READY -v
```

- `asr_front.py` 的**日志走 stderr**（可 `2>asr.log` 或 `journalctl` 看），**识别结果只走 stdout**，所以管道不会被污染；
- 不接麦克风时也能单独测传输层：`python3 node2.py --demo`（每 20 秒发一句内置语料）。

> **网关侧要求**：`node2.py` 用的是 `TXT,<节点>,<序号>,<片号>/<总片数>,<正文>` 并**等待逐片 `ACK`**。
> 地面网关必须按这套协议解析并回 `ACK,M2,<序号>,<片号>`，否则节点会重发 3 次后放弃、且网关显示乱码。
