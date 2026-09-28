# 节点二接入记录（2026-09-28）

## 当前部署

- 树莓派：`zlm@172.20.10.2`，程序位于 `/home/zlm/mine-voice`，systemd 服务 `mine-voice` 已启用。2026-09-28 已部署按键录音版。
- 本仓库对应源码：[`mine-voice/`](mine-voice/README.md)。模型和运行日志留在树莓派，未复制进仓库。
- 地面网关：`surface_node/src/surface_node.ino` 已编译并烧录到 COM3 上的 ESP32-S3。烧录前的 16 MB Flash 备份在项目 `_archive/gateway-before-node2-20260928.bin`；编译出的整片镜像在 `deliverables/gateway-node2-20260928.factory.bin`。
- `pi_node2/` 根目录下的 `node2.py` 和 `sx1278.py` 使用 GPIO8/CE0，是旧实现，不能用于本次 GPIO5 接线。

## Ra-02 接线

| Ra-02 | 树莓派 5 BCM | 物理针脚 |
| --- | --- | ---: |
| VCC | 3.3 V | 17 |
| GND | GND | 20 |
| SCK | GPIO11 / SPI0 SCLK | 23 |
| MISO | GPIO9 / SPI0 MISO | 21 |
| MOSI | GPIO10 / SPI0 MOSI | 19 |
| NSS | GPIO5 | 29 |
| RST | GPIO25 | 22 |
| DIO0 | 不接 | — |

Ra-02 只用 3.3 V，必须共地；发射前接好 433 MHz 天线。模块电源附近建议并联 `100 μF + 0.1 μF` 去耦电容。

## 现场验证

1. 停止 `mine-voice` 服务后运行 `cd ~/mine-voice && .venv/bin/python hardware_check.py`，输出“麦克风正常”和“SX1278 正常：SPI 通信与 LoRa 参数初始化成功（未发送报文）”。服务运行时会占用 USB 麦克风，使音频设备编号 `0` 的查询失败。
2. 树莓派收到节点一 `STATUS,M1,KWS_READY` 心跳；网关持续收到节点一心跳。
3. 树莓派发送单片 `TXT,M2,42144,1/1,节点二联调测试`，网关显示文本并返回 `ACK,M2,42144,1`，树莓派确认 ACK。
4. 固定长文本分为 `2/2` 片，两片均获 ACK，网关拼接后显示完整原句。
5. `mine-voice` 服务已设置 `lora.enabled=true`、`ack_enabled=true`、5 秒心跳。网关收到服务自动产生的 M2 心跳与 ASR 转写文本，同时继续收到 M1 心跳。

## 后续质量工作

麦克风采集和离线识别可运行。原先自动 VAD 多次录到 15 秒上限，现场自动转写出现不准确的短句。已将按键录音版部署到树莓派：常开按键接 GPIO17（物理 11）与 GND（物理 9），内部上拉，按住录音、松开停止，最大 30 秒。服务重启后状态为 `active`，日志显示“按键就绪：GPIO17，内部上拉，按下接地”，`pinctrl get 17` 为 `input, pull-up, high`，5 秒心跳仍持续发送。尚未接线按下按键做实际录音与文本发射验证。节点一的应急语音事件在本次网关更新后尚未重新触发验证；已验证其心跳接收正常。

## 构建说明

本机 PlatformIO 工具链在中文目录下链接时无法写出 `firmware.map`。本次将 `surface_node/platformio.ini` 和 `surface_node/src/` 复制到纯英文临时目录构建，源码未作路径相关修改。旧网关 Flash 已备份，可按需回退。
