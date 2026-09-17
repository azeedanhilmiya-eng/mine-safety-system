# voice_node — 语音节点固件

在井下节点 1 上增加按键说话的关键词识别，识别结果经现有 LoRa 链路发往地面网关。
模型和参数头文件由 `voice_kws/` 生成，不要手改。

## 状态

| 部分 | 状态 |
|---|---|
| `kws_postprocess.c/h` 决策逻辑 | **已通过主机端测试**（525 条向量与 Python 实现逐位一致） |
| `kws_packet.h` 报文格式 | **已通过主机端测试**（构造/解析/CRC，合法报文的所有单字符变异均被拒绝） |
| `kws_config.h`、`kws_model_data.cc` | 由训练流程生成，当前是**合成数据占位模型** |
| `voice_node.ino` I2S / 前端 / TFLM 胶水层 | **已过类型检查**（`firmware/test`，用桩头文件），但**未用真实库编译过** |

## 引脚

避开了本节点已用的 LoRa（9–14）、MQ-4/MQ-7/水浸（4–6）、警号（18），
以及 SPI Flash（26–32）、八线 PSRAM（33–37）、USB（19/20）、UART0（43/44）、
strapping（0/45/46）。

| 功能 | GPIO |
|---|---|
| I2S BCLK | 16 |
| I2S WS | 15 |
| I2S SD | 17 |
| 按键（接地，`INPUT_PULLUP`） | 21 |
| INMP441 L/R | 接 GND（走左声道，`I2S_STD_SLOT_LEFT`） |

麦克风在面包板上时，**I2S 三根线尽量短**，GND 要和开发板共地且接实。
面包板上的长跳线在 1 MHz 左右的 BCLK 上足以造成偶发误码，表现为特征里出现随机尖峰。

## 采样增益

`kSampleShift = 14`。INMP441 是 24 bit 左对齐在 32 bit slot 里，右移 16 位是原始量级，
移 14 位约等于 +12 dB，适合一臂距离说话。

用 `tools/serial_record.py` 校准：正常说话 RMS 2000–8000，喊"救命"峰值应低于 30000。
削顶就调大移位量，太轻就调小。**这个值定下来之后，采集和部署必须用同一个值。**

## 任务划分

语音跑在 core 0（井下节点固件不开 WiFi，这个核是空的），LoRa 留在 core 1 的 `loop()`，
两者通过队列传事件。SPI 总线因此始终只有一个使用者，不需要加锁。

## 串口协议

和 `voice_kws/tools/` 共用：

```
'r'  采集一次，输出 #WAV 帧              -> serial_record.py
'p'  采集一次，输出 #WAV + #FEAT 帧      -> device_parity.py
'i'  打印内存和模型信息
```

帧格式：

```
#WAV <sample_rate> <num_samples>\n  <num_samples*2 字节小端 int16>  #END\n
#FEAT <num_elements>\n              <num_elements 字节 int8>        #END\n
```

其余行按普通日志透传。

## LoRa 报文

```
语音事件（节点 -> 网关）  VE,<node>,<cmd>,<conf>,<seq>,<crc8>    例 VE,M1,2,93,17,C0
ACK      （网关 -> 节点）  VA,<node>,<seq>                        例 VA,M1,17
```

`crc8` 对校验和之前（含那个逗号）的全部字符计算，多项式 0x07，初值 0x00，两位大写十六进制。
`cmd` 是 `kKwsLabels` 的下标，也就是 `kws/config.py: LABELS` 的下标。

格式的构造和解析都在 **`kws_packet.h`**（header-only，纯 C），
**节点和网关编译的是同一份**，所以两端不可能分叉。解析是严格的：
字段数、数值范围、校验和格式任何一项不对都拒绝，不做猜测。

### 地面网关（已实现）

`firmware/surface_node.ino` 的 `processPacket()` 是按位置硬解析的：读第一个逗号前的字段
当 node id，然后按 4 个逗号取 mq4/mq7/water/flags。

语音报文如果不加独立分支：

- 写成 `VE,M1,...` → `nodeId` 解析成 `"VE"`，`mineIndex = -1`，事件被**静默丢弃**；
- 写成 `M1,V,...` → `flags` 会取到序号等非零值，`mine.inAlert` 置真，
  **误触发蜂鸣器和短信**。

所以 `processPacket()` 最开头加了分支，语音报文在进入传感器解析之前就被截走。

`processVoiceEvent()` 的顺序是：解析并校验 → **立即回 ACK** → 去重 → 拒绝非指令类 →
OLED → 蜂鸣器 → 短信 → Firebase。

**ACK 必须最先发。** 节点只等 900 ms 就重传，而刷 OLED、发短信、传 Firebase
加起来远超这个时间。先做慢活再回 ACK，等于每个事件都发两遍。

**去重**：ACK 丢失时节点会重传同一个 seq。10 秒窗口内重复的 seq 会被再次 ACK
但不重复告警。

**语音状态和传感器状态是分开的**（`VoiceState` vs `MineState`）。如果把语音事件塞进
`flags`/`inAlert`，一声呼救在短信、看板和 App 里会显示成瓦斯读数。

**Firebase 走独立的 `/voice_events/<node>/` 路径**，不进 `/alerts`，
否则 App 的传感器告警页会被污染。

### OLED 中文

默认关闭，显示英文标签（和网关现有界面一致）。打开需要 CJK 字体：

```cpp
#define VOICE_OLED_CHINESE 1                       // surface_node.ino
#define VOICE_LABEL_FONT u8g2_font_wqy12_t_gb2312  // 换成你的 u8g2 实际提供的
```

字体本身占几百 KB flash，网关这边不紧张。默认关掉是因为字体名要和你的 u8g2
版本对得上，而这个我没法在这里验证——编译不过就换一个字体名，或者改回 0。

绘制统一用 `drawUTF8()`，所以切换只影响字体和标签表，不影响其它代码。

### 顺带修掉的三个问题

改网关时发现的，都不是语音功能引入的，但都会影响你的断网演示：

1. `String node = mines[index].nodeId.toLowerCase();` —— arduino-esp32 的
   `toLowerCase()` 返回 `void`，这行**编译不过**。两处，已拆成两条语句。
2. `drawWiFiStatus()` / `drawGsmStatus()` 无条件调 `drawReadyScreen()`，
   会把还在有效期内的告警画面擦掉。已改为重绘当前应显示的内容。
3. WiFi 重连原本是 `millis() % 10000 < 50` 触发，而 `connectWiFi()` 最长阻塞 20 秒。
   **断网演示正好是这个条件**——网关会把大部分时间花在阻塞重连上，收不到 LoRa。
   已改成 30 秒定时器，且告警显示期间不重试。

   演示前还可以考虑把 `WIFI_TIMEOUT_MS` 从 20000 调小到 3000，阻塞会短很多。
   这个我没替你改，因为它会影响正常联网时的行为。

## 三窗投票

按键按下后录 1.4 秒，切出 3 个 1 秒窗（起点 0 / 200 / 400 ms），各推理一次。
至少两窗一致才算数，三窗各不相同直接拒识。置信度取投给获胜类别的那些窗中的最高值。

多花两次推理（合计约 120–240 ms，仍在 300 ms 预算内），换来的是对"按键时机没掐准"的
鲁棒性。现场演示时人会紧张，说话和按键很难对齐，这一条比任何调参都管用。

## 前端状态必须复位

`computeFeatures()` 每次都调 `FrontendReset()`。micro frontend 内部带噪声估计和 PCAN 增益
状态，会跨调用累积；而训练特征来自的 TF op **每次都从干净状态开始**。

不复位 = 板子和训练集看到的不是同一种特征 = PC 上的准确率对这块板子无效。
`voice_kws/tools/device_parity.py` 就是用来抓这个的。

## 构建

主线：PlatformIO，`framework = arduino, espidf`（arduino-esp32 3.x 作为 IDF 5.x 组件），
加 `esp-tflite-micro` 组件——这样既能继续用 `LoRa.h`，又能拿到 ESP-NN 的 INT8 加速内核。

退路：Arduino IDE + `TensorFlowLite_ESP32`。没有 ESP-NN，推理约 150–250 ms，
仍满足 300 ms 指标，只是报告里的数字没那么好看。**如果构建配置卡超过半天就切退路**，
功能优先。

算子解析器只注册了模型实际用到的 7 个算子，未用到的内核不进 flash。

## 首次上板顺序

1. 烧录，串口发 `i`，确认模型加载成功、`arena_used_bytes` 有合理值。
   把 `kArenaSize` 调成实测值的 1.2 倍，这个数字要写进提交材料的内存报告。
2. 发 `r` 采一条，用 `serial_record.py` 存成 WAV，**用 Audacity 听一遍**——
   必须是干净人声，没有嗡嗡声和爆音。这关不过，后面全白搭。
3. 跑 `device_parity.py -n 20`，必须 20/20 逐字节相同。
4. 以上都过了，再看识别结果。
