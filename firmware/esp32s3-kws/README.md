# 矿安智联 · 井下节点1完整工程

当前调试阶段只启用 ESP32-S3 N16R8、INMP441 离线语音识别、
SSD1306 OLED 和 SX1278/Ra-02 LoRa。MQ-4、MQ-7、Wi-Fi、OneNET 和本地 Web
暂时通过源码功能开关停用，先把语音与 LoRa 链路调通。

本方案明确不使用 SIM 卡通信和水位传感器，当前也不安装 LED 和实体按键。
语音通过软件唤醒词“屈展”触发，不依赖按键。

当前 `src/main.cpp` 的调试开关为：

```cpp
#define ENABLE_WIFI 0
#define ENABLE_LORA 1
#define ENABLE_MQ_SENSORS 0
```

因此 MQ-4/MQ-7 可以完全不接，GPIO4/GPIO5 不会被初始化或采样。LoRa 每 5 秒
发送 `STATUS,M1,KWS_READY,序号` 心跳，语音识别成功后另发 `VOICE` 事件包。

**零训练方案**：直接使用乐鑫 ESP-SR 的 **MultiNet6 中文命令词模型 `mn6_cn`**。当前不使用 WakeNet，软件唤醒词「屈展」和三个告警命令均由 MultiNet6 识别。
命令词识别**不需要训练模型**，也不需要采数据。

---

## 1. 这个工程是什么

| 项目 | 内容 |
|---|---|
| MCU | ESP32-S3 N16R8（16 MB flash + 8 MB 八线 OPI PSRAM） |
| 麦克风 | INMP441（I2S，16 kHz / 16 bit / 单声道） |
| 语音方案 | 乐鑫 ESP-SR：AFE 声学前端 + MultiNet6 中文命令词 |
| 软件唤醒词 | 屈展（`qu zhan`，由 MultiNet6 识别） |
| 命令词 | 紧急救命 / 请求救援 / 有人被困 |
| 气体传感器 | MQ-4 甲烷 + MQ-7 一氧化碳 |
| 离线通信 | SX1278/Ra-02，433 MHz LoRa |
| 显示 | 0.96 英寸 SSD1306 I2C OLED |
| 框架 | Arduino（PlatformIO） |

> ⚠️ **必须带 PSRAM**。MultiNet6 模型运行时需要约 4 MB PSRAM，加上 AFE 前端合计约 4.3 MB。
> N16R8 的 8 MB PSRAM 够用；若只有 2 MB PSRAM 的模组请换用更小的模型。

---

## 2. 目录结构

```
esp32s3-kws/
├── platformio.ini              # N16R8 + 分区表 + 模型烧录配置
├── partitions_esp_sr_16_custom.csv # 带 6 MB model 分区的本地分区表
├── srmodels.bin                # 中文模型包（mn6_cn + fst + vadnet，不含 WakeNet）
├── src/main.cpp                # 主程序：I2S + 关键词识别
├── lib/ESP_SR/                 # 已打补丁的 ESP_SR 库（改用中文模型）
├── tools/
│   ├── pack_model.py           # 乐鑫官方模型打包脚本
│   ├── build_srmodels.ps1      # 重新生成 srmodels.bin
│   ├── flash_srmodels.py       # 烧录后自动写 model 分区
│   ├── srmodels_src/           # 模型源文件（mn6_cn / fst / vadnet1_medium）
│   └── esp_sr_orig/            # 原版 ESP_SR 源码（对照用）
└── .vscode/                    # VSCode 任务配置
```

---

## 3. 为什么需要改动 ESP_SR 库

Arduino-ESP32 自带的 `ESP_SR` 库是**英文专用**的，原版代码里写死了：

```c
char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);  // 只找英文模型
...
char *phonemes = flite_g2p(sr_commands[i].str, 1);                          // 英文音素转换
```

本工程把它复制到 `lib/ESP_SR/`（PlatformIO 会用工程内的库覆盖框架库）并打了补丁：

1. **优先查找中文模型** `ESP_MN_CHINESE`，找不到再回退英文；
2. **中文命令词绕过英文 g2p**：Arduino 的 `ESP_SR` 库是**英文 MultiNet7 编译**的
   （定义了 `CONFIG_SR_MN_EN_MULTINET7_QUANT`），`esp_mn_commands_add` 会强制走
   `flite_g2p` 把中文拼音当英文转成音素（如 `jiu ming → qmo Mgl`），导致中文命令词
   全部 invalid。所以中文命令词改用 `esp_mn_commands_phoneme_add(id, 拼音, 拼音)`
   直接传拼音、跳过 flite_g2p（mn6 的命令词单位本就是拼音/grapheme）。

补丁是 `lib/ESP_SR/src/esp32-hal-sr.c`，改动处都有 `[PATCHED]` 注释。

---

## 4. 编译 & 烧录

> **本机已经配置好并且编译通过了**。2026-09-22 在本机编译通过
> （RAM 12.6% / Flash 49.5%；首次全量约 57 秒，无改动的增量约 16 秒）。
> 下面第 4.1 节是首次配置时踩过的坑，换电脑时照做即可。

```powershell
# 编译
pio run

# 烧录固件 + 自动写入中文模型包到 model 分区
pio run -t upload

# 查看串口输出
pio device monitor -b 115200
```

Windows 下 GNU 链接器无法在含中文的路径里创建 `firmware.map`，所以工程必须放在
**纯 ASCII 目录**下构建。请将仓库放在不含中文字符的目录中，并在 VSCode 中直接打开
本目录进行编译。

在 VSCode 里编译/运行时要注意：要用 `文件 → 打开文件夹` 打开**本目录**
（`platformio.ini` 所在层），不要打开上层目录，否则 PlatformIO 找不到工程根。

在 VSCode 里也可以直接用任务面板（`Ctrl+Shift+P` → `Tasks: Run Task`）：
- `PIO: 编译`
- `PIO: 烧录固件 + 中文模型`
- `PIO: 串口监视`
- `重新打包中文模型 srmodels.bin`

### 4.1 环境配置（换电脑时看这里）

**① 平台必须用 pioarduino，不能用 PlatformIO 官方平台。**

官方 `espressif32` 平台目前最高只到 Arduino **2.0.17**，那个版本**没有 `ESP_SR` / `ESP_I2S` 库**，
编译会在 `#include "ESP_I2S.h"` 处失败。`platformio.ini` 里已经指向 pioarduino 的
`platform-espressif32 @ 55.03.311`（Arduino 3.3.11 / ESP-IDF 5.5.5）。

**② 板子只能选 `esp32-s3-devkitc-1`。**

PlatformIO 的板子列表里**没有 N16R8**，`esp32-s3-devkitc-1` 的默认配置是
「8 MB Flash / 无 PSRAM」。靠 `platformio.ini` 里这 5 行覆盖成 N16R8：

```ini
board_build.flash_size          = 16MB
board_build.flash_mode          = qio
board_build.arduino.memory_type = qio_opi   ; <flash>_<psram>，八线 PSRAM 必须写 opi
board_upload.flash_size         = 16MB
board_upload.maximum_size       = 16777216
```

配置生效的**验证方法**：编译日志里 `HARDWARE:` 一行应显示 `16MB Flash`；
并且链接时的库路径必须是 `framework-arduinoespressif32-libs/esp32s3/qio_opi/`
（那套预编译库的 `sdkconfig.h` 里才有 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` 和
`CONFIG_SPIRAM_MODE_OCT`）。查一下 `firmware.map` 里 `qio_opi` 出现、`qio_qspi` 不出现即可。

> 板子名那一行仍会显示 “DevKitC-1-N8 (8 MB QD, No PSRAM)”——那只是板子 JSON 里的显示名，
> **不影响实际构建**。

**③ 国内下载慢 → 走乐鑫官方镜像（关键）。**

ESP-IDF 工具链（Xtensa GCC 约 413 MB、GDB 约 43 MB）默认从 `github.com` 下载，
国内直连基本为 0，走代理也只有 ~1 MB/s。乐鑫有自己的 GitHub 镜像，
**直连速度 20~35 MB/s**：

```powershell
# 永久写入用户环境变量（已在本机设置好，VSCode 需重启一次才能继承）
setx IDF_GITHUB_ASSETS "dl.espressif.com/github_assets"
```

`tool-esp_install/tools/idf_tools.py` 会自动把 `https://github.com/...` 重写成
`https://dl.espressif.com/github_assets/...`，无需改任何脚本。
（注意这个变量**不能带 `https://`**，只写域名加路径。）

**④ 另一个坑：包目录里残留 `tools.json` → 每次构建都重跑 idf_tools 安装。**

pioarduino 的 `platform.py` 判断逻辑是「包目录里存在 `tools.json` → 每次构建都重跑
idf_tools 安装；不存在则只做版本校验、不联网」。也就是说 `tools.json` 是**安装未完成的
标记**，装好的包不应该有它。两种典型后果：

**(a) `tool-xtensa-esp-elf-gdb`：重复下载 + WinError 32。**

Extract 阶段偶发 `PermissionError: [WinError 32] ... .zip.tmp`，导致安装失败、
`tools.json` 残留，于是每次 `pio run` 都重新下 43 MB。
处理办法：手动把包塞进 `dist/` 缓存，再跑一次即可（idf_tools 校验 sha256 后会直接跳过下载）：

```powershell
$d = "$env:USERPROFILE\.platformio\dist"
curl.exe -L --noproxy "*" -o "$d\xtensa-esp-elf-gdb-17.1_20260402-x86_64-w64-mingw32.zip" `
  "https://dl.espressif.com/github_assets/espressif/binutils-gdb/releases/download/esp-gdb-v17.1_20260402/xtensa-esp-elf-gdb-17.1_20260402-x86_64-w64-mingw32.zip"
```

装好后 `packages/tool-xtensa-esp-elf-gdb/` 下的 `tools.json` 会被删除。

**(b) `tool-esptoolpy`：乐鑫镜像 404，构建在编译前直接中断（2026-09-22 本机遇到）。**

`packages/tool-esptoolpy/` 里同样残留了 `tools.json`，而 esptool 来自 pioarduino 的
fork `pioarduino/esptool`，**乐鑫镜像不托管这个仓库**：

```text
https://github.com/pioarduino/esptool/releases/download/v5.3.0/esptool.zip
  → https://dl.espressif.com/github_assets/pioarduino/esptool/releases/download/v5.3.0/esptool.zip
  → HTTP Error 404: Not Found
```

于是 `idf_tools.py` 退出码 1，日志里只有一行 `idf_tools.py installation failed (rc=1)`，
连第一步编译都到不了。（注意这跟 `IDF_GITHUB_ASSETS` 设置无关，是镜像里没有该仓库；
另外 `IDF_GITHUB_ASSETS` 对工具链那种 400 MB 级下载仍然是必需的。）

但 esptool 5.3.0 其实已经完整安装（`package.json`、`.piopm` 都在，版本与
`platform.json` 要求的 `package-version: 5.3.0` 一致），所以直接删掉这个残留标记，
让它走「版本校验、不联网」分支即可：

```powershell
# 删掉残留标记
Remove-Item "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\tools.json"
# 顺手清掉下载失败的临时文件（可选）
Remove-Item "$env:USERPROFILE\.platformio\dist\*.tmp" -Force -ErrorAction SilentlyContinue
```

> 自查手法（正常状态应全部为 `False`，哪个是 `True` 构建就会去重装哪个包）：
>
> ```powershell
> Get-ChildItem "$env:USERPROFILE\.platformio\packages" -Directory -Filter "tool*" | ForEach-Object { "{0}: tools.json={1}" -f $_.Name, (Test-Path (Join-Path $_.FullName "tools.json")) }
> ```

**⑤ VSCode 设置**（`.vscode/settings.json` 已写好）：

- `platformio-ide.useBuiltinPIOCore = true`（**必须保持 `true`**）→ 用扩展自带的 pioarduino Core。
  首次打开工程时它会联网下载便携 Python + Core（几十 MB，装一次即可）。装完之后工具栏的
  Build / Upload / Monitor 才会注册；**安装过程中点按钮会报 `command 'platformio-ide.upload' not found`**，
  这是正常的，等进度条走完再点。
  ⚠️ 千万别改成 `false`：那样扩展会改用系统 PATH 里的 `python`，而本机 PATH 第一位是
  系统 PATH 中的 Python（若未安装 PlatformIO），
  于是报 `ModuleNotFoundError: No module named 'platformio'`，核心永远起不来、按钮全废
  （日志位置：`%APPDATA%\Code\logs\<日期>\window1\exthost\exthost.log`）。
  命令行 `pio` 不受这个设置影响，始终用 `~/.platformio/penv`。
- `terminal.integrated.env.windows` 里注入了 `IDF_GITHUB_ASSETS` / `PYTHONUTF8`，
  保证 VSCode 内置终端的下载也走镜像。
- `PYTHONUTF8=1` 是必需的：pioarduino 用 `subprocess.run(text=True)` 读 idf_tools 输出，
  中文 Windows 默认 GBK，遇到进度条里的非 ASCII 字节会抛 `UnicodeDecodeError`。

**⑥ 中文模型用 mn6_cn + model 分区扩到 6MB（本方案已改好）**

- 中文命令词模型用的是 **mn6_cn**（不是 mn7_cn）。mn7_cn 是 esp-sr v1.6.0 才加入的，
  Arduino ESP_SR 3.3.11 库太旧，加载后中文命令词会全部失败（详见第 3 节）。
- `mn6_cn` 的 `mn6_data` 有 3.68 MB，加上 fst + vadnet + wn9，模型包约 4.08 MB，
  超过原 model 分区 3.875 MB，所以把 `model` 分区扩到了 **6 MB @ 0x710000**，
  `spiffs` 缩到 1 MB（本方案用不到 spiffs）。
- ⚠️ **坑**：本地分区表不能与框架自带的 `esp_sr_16.csv` 同名，否则 PlatformIO
  可能使用框架版本，导致固件认为模型在 `0xC10000`，烧录脚本却写到 `0x710000`。
  本工程使用唯一文件名 `partitions_esp_sr_16_custom.csv`，不再修改框架文件。
  换电脑/重装框架后要重新改一次，否则 model 分区还是 3.875 MB、模型烧不进去。

---

## 5. 节点1接线

## 5.1 连接手机热点

复制 `include/onenet_secrets.example.h` 为被 Git 忽略的 `include/onenet_secrets.h`，
在其中填写你们自己的 2.4 GHz Wi-Fi 和 OneNET 参数。启动时会尝试连接最多 20 秒，
连接成功会在串口打印 ESP32 的局域网 IP；连接失败则继续运行离线语音识别，不会卡死。
ESP32-S3 只能连接 2.4GHz Wi-Fi，私密配置文件不会被提交到 Git。

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3.3V（禁止接 5V） |
| GND | GND |
| L/R | GND（左声道） |
| SCK | GPIO15 |
| WS | GPIO16 |
| SD | GPIO17 |

| 气体传感器 | ESP32-S3 |
|---|---|
| MQ-4/MQ-7 VCC | 5V（仅模块加热与供电） |
| MQ-4 AO | GPIO4 (ADC1) |
| MQ-7 AO | GPIO5 (ADC1) |
| MQ-4/MQ-7 GND | GND（必须共地） |

> MQ 模块 AO 若可能超过 3.3V，必须先经过电阻分压再接 ESP32-S3。ESP32-S3
> GPIO 不耐 5V。程序每 5 秒分别对 MQ-4/MQ-7 进行 16 次 ADC 采样取平均，
> 并将 0~4095 原始值上报 OneNET 的 `mq4`、`mq7` 属性。

以下引脚为 **PCB 定版方案**（2026-09-17 核对），已打样，不可更改。

| OLED SSD1306 | ESP32-S3 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

| SX1278 / Ra-02 | ESP32-S3 |
|---|---|
| VCC | 独立稳定 3.3V（禁止接 5V） |
| GND | GND |
| DIO0 | GPIO21 |
| NSS/CS | GPIO13 |
| MOSI | GPIO11 |
| SCK | GPIO10 |
| MISO | GPIO12 |
| RST | GPIO14 |

> Ra-02 发射前必须接好 433 MHz 天线。建议在模块电源附近放置 100 µF 与
> 0.1 µF 去耦电容。所有模块必须共地。

### ESP32 内置 Web 仪表盘

完整监控网页位于 `web/`，编译时由 `tools/embed_web.py` 压缩并嵌入固件。
ESP32 连接手机热点后，串口会输出类似：

```text
[Web] 仪表盘已启动: http://192.168.x.x/
```

手机或电脑连接同一个热点，然后在浏览器打开该地址即可。网页通过 ESP32 本机
`/api/sensors` 接口读取 MQ-4/MQ-7，不需要 Node.js 或 `npm start`。

> INMP441 输出 24 bit 数据、放在 32 bit I2S 槽里，所以代码用
> `I2S_DATA_BIT_WIDTH_32BIT` + `I2S_RX_TRANSFORM_32_TO_16` 转换，这是 INMP441 的正确接法。

---

## 6. 改命令词（不用重新训练）

编辑 `src/main.cpp`：

```cpp
static const sr_cmd_t sr_commands[] = {
  {CMD_JINJI_JIUMING,   "jin ji jiu ming"},   // 紧急救命
  {CMD_QINGQIU_JIUYUAN, "qing qiu jiu yuan"}, // 请求救援
  {CMD_YOUREN_BEIKUN,   "you ren bei kun"},   // 有人被困
  {CMD_QUZHAN,           "qu zhan"},           // 软件唤醒词：屈展
};
```

**中文 MultiNet 的命令词是拼音**，汉字转拼音用乐鑫工具 `tools/` 里的
[`multinet_pinyin.py`](https://github.com/espressif/esp-sr/blob/master/tool/multinet_pinyin.py)。

几个要点：
- 一条命令可以挂多个说法，用逗号分隔，例如 `"jiu ming,kuai jiu ming,jiu ming a"`。
- 命令词不能含阿拉伯数字和特殊字符，不支持中英文混用。
- 最多 200 条命令词。
- 当前不安装实体按键。待机时先说软件唤醒词“屈展”，再说安全命令。

---

## 7. 软件唤醒逻辑

本工程没有使用 WakeNet，也没有实体按键。MultiNet6 将“屈展”作为普通命令词持续监听；
识别到“屈展”后打开 15 秒命令窗口，在窗口内响应“紧急救命”“请求救援”“有人被困”。
超时后自动回到等待“屈展”的状态。

---

## 8. LoRa 报文

节点1每 5 秒发送一次气体遥测：

```text
DATA,M1,<mq4>,<mq7>,<flags>,<seq>
```

`flags` 的 bit0 表示 MQ-4 超阈值，bit1 表示 MQ-7 超阈值；没有水位字段。

识别到语音命令时发送：

```text
VOICE,M1,HELP,<seq>       紧急救命
VOICE,M1,RESCUE,<seq>     请求救援
VOICE,M1,TRAPPED,<seq>    有人被困
VOICE,M1,AWAKE,<seq>      命中唤醒词"屈展"(非报警: 只让地面网关切到"请说命令"提示画面)
```

> `AWAKE` 是给地面网关做交互提示用的：喊"屈展"时井下节点本身只打开 15 秒命令窗口，
> 若不把这一事件也发出去，地面网关无法知道你已经唤醒（它只靠心跳判断在线）。

LoRa 未接好或初始化失败不会阻止语音、OLED、气体采样和网页运行，串口会明确提示。

---

## 9. 论文/文档里要如实说明的内容

参赛要求「第三方开源代码、模型名称和版本如实说明」，建议写明：

> 语音识别采用乐鑫 ESP-SR 框架（`espressif/esp-sr`）提供的 **MultiNet6 中文命令词模型 `mn6_cn`**。
> “屈展”和三个安全命令均作为 MultiNet6 命令词，以拼音方式配置，**未使用 WakeNet，未自行训练声学模型**。
> 模型以二进制形式分发，许可为乐鑫 ESPRESSIF MIT（仅限乐鑫芯片使用）。

---

## 10. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 串口打印 `启动失败` | `model` 分区没烧入模型包，执行 `pio run -t upload`（会自动烧 `srmodels.bin`） |
| 识别不到命令词 | 先说“屈展”，再在 15 秒内说安全命令；确认使用中文模型 `mn6_cn` |
| VS Code 提示 `command 'platformio-ide.build/upload/serialMonitor' not found` | 扩展自带 Core 还没就绪（首次要下载几十 MB）。看状态栏/`OUTPUT → pioarduino` 的进度，装完再点按钮。若长期如此，查 `%APPDATA%\Code\logs\<日期>\window1\exthost\exthost.log` 有没有 `No module named 'platformio'`，并确认 `platformio-ide.useBuiltinPIOCore` 为 `true`。急用时直接用任务「PIO: 编译 / 烧录固件」 |
| VS Code 报 `无法使用 compilerPath “...” 解析配置`，并建议改用 Dev-Cpp 的 `gcc.exe` | `.vscode/c_cpp_properties.json` 由 PlatformIO 自动生成，里面残留了**旧机器**的 `compilerPath` / `includePath`。把 `C:/Users/<旧用户名>/.platformio` 和旧工程路径改成本机路径，或在 VSCode 里 Build 一次让它按 `platformio.ini` 重新生成；不要真的把 `compilerPath` 指向 Dev-Cpp 的 gcc |
| 构建报 `idf_tools.py installation failed (rc=1)` | 包目录里残留 `tools.json` 触发重装（esptool 会被改写成乐鑫镜像地址并 404）。删 `~/.platformio/packages/tool-esptoolpy/tools.json` 即可，详见 4.1 ④ |
| 链接器无法创建 `firmware.map` | 工程路径含中文；把工程放在纯 ASCII 目录下构建 |
| 一直重启 / 分配内存失败 | PSRAM 没启用，检查 `board_build.arduino.memory_type = qio_opi` |
| 编译报错找不到 `ESP_SR.h` | 确认 `lib/ESP_SR/` 目录完整 |
| `jiu ming` 容易误触发 | 2 音节偏短，实测后加长，如 `jiu ming a`、`kuai che li` |
