# OneNET M1、M2 与地面网关接入

## 当前方案：M2 独立设备（2026-09-28）

为保留已配置的 M1 设备，在同一 `esp32` 产品下新增独立设备 `mine-voice-m2`。
树莓派 `~/mine-voice` 已启用 MQTT 直连，识别成功时向自己的设备上报 `m2_text`、
`m2_alarm`；原有 LoRa 到地面网关的路径继续运行。M2 的 MQTT 在线状态只表示树莓派
连接 OneNET，不表示网关收到 LoRa。网关仍可向原 `zlmdesign` 设备上报
`m2_lora_online`、`m2_rssi`，但需先烧录更新后的网关固件。
后文“历史方案”仅供追溯，当前 M2 采用本节的独立设备方案。

## OneNET 网络路径（2026-09-28）

```text
M1 ── Wi-Fi/MQTT ── 原设备 zlmdesign（源码已配置；新版固件上报待核验）
M2 ── LoRa ── 地面网关（本地 OLED/蜂鸣器）
 └── Wi-Fi/MQTT ── 独立设备 mine-voice-m2（已部署；实时属性待核验）
地面网关 ── Wi-Fi/HTTPS ── 原设备 zlmdesign（源码已配置；新版固件烧录待验证）
```

同一 `esp32` 产品下包含 M1/GW 与 M2 设备可用的物模型属性，但 M1/GW 的原设备和 M2 的独立设备是不同设备。M2 MQTT 在线仅代表 M2 到 OneNET 的连接，不代表 LoRa 到网关；网关 LoRa 状态需在新固件烧录后再核验。

### 历史方案（旧配置，不代表当前）

旧方案曾计划让 M2 经网关写入 `zlmdesign` 同一设备。该方案已由本节开头所述独立设备部署替代，后续配置不得再按旧方案操作。
M1 与网关使用原 OneNET 设备 `zlmdesign`，分别走 MQTT 与 HTTPS 源码路径；M2 使用独立设备 `mine-voice-m2` 并已部署 MQTT 直报。M2 的实时属性仍待设备属性页核验，M1/网关新版固件上报及页面数据源也待完成。设备在线状态不能代替 `m1_lora_online`、`m2_lora_online` 等 LoRa 接收状态。

## 物模型属性

在现有 OneNET Studio **产品**的物模型中创建下列属性，标识符和数据类型须与源码一致。
已有的同名属性保留，无需重复创建。当前未接 MQ 传感器时，不上报 `mq4`、`mq7`；
它们若已存在，可保留待传感器接入。

| 标识符 | 类型 | 来源 | 页面建议显示 |
|---|---|---|---|
| `m1_online` | 布尔 | M1 MQTT | M1 Wi-Fi 上报心跳 |
| `m1_event` | 字符串 | M1 MQTT | 最近识别的命令码 |
| `m1_event_seq` | 整数 | M1 MQTT | 事件序号 |
| `m1_lora_online` | 布尔 | 网关 LoRa 接收 | M1 射频在线 |
| `m1_rssi` | 整数，dBm | 网关 | M1 收包信号强度 |
| `m1_alarm` | 布尔 | 网关 | M1 报警 |
| `m1_rx_event` | 字符串 | 网关 | M1 最近接收事件中文说明 |
| `m2_lora_online` | 布尔 | 网关 LoRa 接收 | M2 射频在线 |
| `m2_rssi` | 整数，dBm | 网关 | M2 收包信号强度 |
| `m2_alarm` | 布尔 | M2 独立设备 MQTT | M2 报警（实时值待核验） |
| `m2_text` | 字符串 | M2 独立设备 MQTT | M2 最近转写文本（实时值待核验） |
| `gw_wifi` | 布尔 | 网关 | 网关 Wi-Fi 已连接 |
| `gw_uptime` | 整数，秒 | 网关 | 网关开机时长 |

OneNET 在线状态只能说明对应设备的 MQTT 会话，不能说明 LoRa 已到达网关。平台可能保留最后一次属性值；`m1_lora_online`、`m2_lora_online` 与 `gw_wifi` 也会因设备掉线而停留在旧值，判断状态需同时检查属性更新时间。当前 M2 MQTT 已部署但属性级实时更新尚未确认。

## 配置与使用

1. M1 的本机配置位于 `D:\大三\矿山系统监测\esp32s3-kws(2)\esp32s3-kws\include\onenet_secrets.h`；网关配置位于 `surface_node/include/onenet_secrets.h`。两者都已加入 Git 忽略，不要上传密钥文件。
2. 在 OneNET Studio 产品物模型创建上表属性，并发布/启用物模型。标识符区分大小写。当前产品的 `m1_rx_event`、`m2_text` 均设为最长 255 字节；网关上报时按 UTF-8 边界截断云端副本，本地保留完整文本。
3. 页面接入后分别绑定原设备 `zlmdesign` 与独立设备 `mine-voice-m2`，并按设备分区展示属性。当前 View 数据源尚未建立，不能将页面草稿描述为已接入。
4. M2 独立设备 MQTT 程序已部署，但需到设备属性页核验实时值；M1 与网关的新固件仍待烧录并验证上报。M1 串口看 MQTT 回复，网关串口看 `[OneNET] 网关快照 HTTP=200 errno=0`；HTTP 成功只代表服务端接收本次上报，属性值仍需到设备页确认。
5. 本地 OLED、LoRa、报警不依赖 OneNET 页面。网络中断后仍可在网关 OLED 查看本地收到的信息，恢复网络后会上报**最新快照**，不会补发断网期间每一条历史事件。

## 2026-09-28 云端配置进度

- 已在现有 `esp32` 产品（产品 ID `w0f18T028t`）保存 15 个物模型属性：原有 `mq4`、`mq7` 加上上表 13 个属性。导入前的物模型备份在 `onenet_model_backup.json`，本次导入文件为 `onenet_model_3nodes.json`。
- OneNET 数据可视化内已创建并保存 2D 项目 `MineVoice_M1_M2`。目前只有页面标题，是**未绑定实时数据、未发布的草稿**。
- 账号已完成个人实名认证；应用开发项目 `SML_MineVoice`（项目 ID `2j87ZKWkzdcQuKk14475`）已创建，现有产品 `esp32` 下的设备 `zlmdesign` 已加入该项目。平台此时显示设备离线。
- View 编辑器的 OneNET Studio 数据源需要用户 ID、访问密钥和项目 ID。已通过账号“我的资源 → 访问权限 → 设备管理服务权限管理（OneNET）→ 查看”核对旧 OneNET 访问密钥与提供的值一致。最初误将 View 的 UID `525158` 当作平台用户 ID；浏览器观察到设备列表请求返回 HTTP 200，但响应的 `data` 是上游 openresty 的 `403 Forbidden` HTML。后来在 OneNET「平台概览」确认真正的平台用户 ID 是 `525156`，以此重新尝试 View 设备列表仍提示“获取设备列表失败”。实时数据源尚未创建，不能把页面当作已经接入设备。
- 账号“访问权限”页明确提示：下方的旧 OneNET AccessKey 不能访问 AIoT 平台能力接口。已通过手机验证码创建上方 AIoT 平台访问账号，页面已生成访问钥匙和秘密钥匙；两者只保留在 OneNET 账号内，均未写入项目文件。官方新版[安全鉴权文档](https://iot.10086.cn/doc/aiot/fuse/detail/1464)指出主用户 ID 可在平台概览查看，API Token 使用 `2022-05-01` 版本。View 内置 OneNET Studio 数据源只提供单个访问密钥字段，其与新版 AIoT API 的对接方式尚未确认。不要把访问密钥、秘密钥匙或验证码写入文档或提交到 Git。
- 树莓派 M2 已恢复联网：`172.20.10.2` 的 SSH 可访问，`mine-voice.service` 正在运行，配置为 GPIO17 按键录音、GPIO5 LoRa 片选。远端与本地的 19 个源码/配置文件内容一致；服务日志持续出现 `ASR_READY` 心跳。M1、网关的新固件尚未据此轮配置烧录，云端属性没有获得实测更新；不能将物模型保存或页面草稿视为联调完成。

协议依据：[MQTT 属性主题](https://iot.10086.cn/doc/iot_platform/book/device-connect%26manager/MQTT/topic.html)、[HTTP 接入](https://iot.10086.cn/doc/iot_platform/book/device-connect%26manager/HTTP/HTTP-introduce.html)、[HTTP 属性上报](https://iot.10086.cn/doc/iot_platform/book/device-connect%26manager/HTTP/api/devicePropertyNotify.html)。
