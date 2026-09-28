# 固件与双节点接入

| 设备 | 当前工程 | 本地职责 | OneNET 接入 |
|---|---|---|---|
| 井下节点一 M1 | [`esp32s3-kws/`](esp32s3-kws/README.md) | ESP32-S3 离线中文命令识别、OLED、LoRa | 原设备 `zlmdesign` 的 MQTT 源码；新版固件上报待核验 |
| 井下节点二 M2 | [`pi_node2/mine-voice/`](pi_node2/mine-voice/README.md) | 树莓派 5 按键录音、离线转写、LoRa | 独立设备 `mine-voice-m2`，MQTT 直报已部署；实时属性待核验 |
| 地面网关 GW | [`surface_node/`](surface_node/platformio.ini) | LoRa 收包与文本重组、中文 OLED、蜂鸣器、M2 分片 ACK | 原设备 `zlmdesign` 的 HTTPS 源码；新版固件烧录与上报待验证 |

## 当前代码

- `esp32s3-kws/` 是当前 M1 PlatformIO 工程。ESP-SR MultiNet6 识别三条中文求助命令并发送 `VOICE` / `STATUS`；MQ-4/MQ-7 采样代码保留，但当前 `ENABLE_MQ_SENSORS=0`。
- `pi_node2/mine-voice/` 是当前 M2 按键录音与离线 ASR 服务。识别文本通过 LoRa 分片发给网关，同时 MQTT 直报到独立 OneNET 设备 `mine-voice-m2`；云端属性更新仍待核验。Ra-02 使用 GPIO5 作为 NSS。
- `surface_node/` 是当前网关 OneNET 工程。它接收 `VOICE` / `TXT` / `STATUS`，重组 UTF-8 文本、显示 OLED、驱动蜂鸣器并返回 M2 分片 ACK；HTTPS 汇总运行在后台任务中。源码具备 OneNET 配置，但新固件尚待烧录和实测。
- `surface_node_vscode/` 是保留的 Firebase 网关工程；`voice_node_vscode/` 是 ESP-IDF 语音节点替代实现。根目录的 `surface_node.ino` 是早期 Arduino 原型，和当前 `surface_node/` 工程分开维护。

## 当前 OneNET 状态与验证边界

M1 与地面网关使用原设备 `zlmdesign`，M1 使用 MQTT 源码路径、网关使用 HTTPS 源码路径；M2 已部署独立设备 `mine-voice-m2` 并配置 MQTT 直报。M2 的实时属性、M1/网关新固件上报和可视化数据源仍待核验。云端设备在线不代表 LoRa 链路在线。

已保存 15 个物模型属性和一个未绑定实时数据、未发布的 View 页面草稿。源码配置不代表硬件已经烧录。具体设备字段、页面配置和验证记录见 [`ONENET_INTEGRATION.md`](ONENET_INTEGRATION.md) 与 [`pi_node2/INTEGRATION_STATUS.md`](pi_node2/INTEGRATION_STATUS.md)。

本机配置与密钥文件（包括 `surface_node/include/onenet_secrets.h`、`pi_node2/mine-voice/config.json` 和 `pi_node2/mine-voice/onenet_secrets.json`）已由 Git 忽略；提交时仅提供 `.example` 模板，不要把凭据写进仓库。

Wi-Fi / OneNET 与本地 OLED、蜂鸣器、LoRa 是独立路径。云端断连时，网关仍按本地链路工作；联网恢复后网关上传最新快照，不补发全部离线历史。
