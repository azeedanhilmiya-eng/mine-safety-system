# ESP32-S3井下语音节点

本工程在ESP32-S3-N16R8上运行乐鑫ESP-SR中文MultiNet量化模型。它不是连续语音转文字，而是离线识别预先配置的安全命令，并通过433 MHz LoRa发送短事件包。

## 命令与报文

| 说出的命令 | 命令ID | LoRa事件码 | 报文示例 |
|---|---:|---|---|
| 救命 | 1 | HELP | `VOICE,M1,HELP,91,1` |
| 撤离 | 2 | EVACUATE | `VOICE,M1,EVACUATE,88,2` |
| 瓦斯 | 3 | GAS | `VOICE,M1,GAS,90,3` |
| 透水 | 4 | WATER | `VOICE,M1,WATER,87,4` |

报文字段依次为：类型、节点ID、事件码、置信度百分数、事件序号。地面网关收到后再把英文事件码映射为中文显示和告警动作。

## 接线

### INMP441

| INMP441 | ESP32-S3 |
|---|---:|
| VDD | 3.3V |
| GND | GND |
| SCK | GPIO15 |
| WS | GPIO16 |
| SD | GPIO17 |
| L/R | GND |

### 其他

- 常开瞬时按键：GPIO7与GND之间。
- 状态LED：GPIO21经330欧姆电阻连接LED正极，LED负极接GND。
- Ra-02：NSS=10、MOSI=11、SCK=12、MISO=13、RST=14、DIO0=9；使用3.3V并在发射前接好433 MHz天线。

## VS Code使用

1. 在VS Code中安装或启用`PlatformIO IDE`扩展。
2. 用`File > Open Folder`打开本目录，不要只打开单个C文件。
3. 按`Ctrl+Shift+B`运行`Voice: Build`。工程固定使用ESP-IDF 5.3.1、ESP-SR 2.0.0。
4. 当前工程路径有中文，因此使用本工程的`Voice:`任务，不要直接点PlatformIO底栏Build。任务会同步源码到英文路径缓存中编译，编辑仍在当前目录进行。
5. 用USB数据线连接开发板的USB转串口接口，运行`Terminal > Run Task > Voice: List ports`找出对应USB串口。蓝牙COM口不是开发板。
6. 运行`Voice: Upload firmware and model`并输入开发板COM口；固件和模型一同烧录。随后运行`Voice: Serial Monitor`，输入同一串口，波特率为115200。
7. 按住GPIO7按键，说出一个命令；串口会显示识别结果和LoRa报文。

也可在本目录的PowerShell终端执行：

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/voice.ps1 -Action Build
powershell -ExecutionPolicy Bypass -File ./scripts/voice.ps1 -Action Ports
# 把COM10替换为实际USB串口。
powershell -ExecutionPolicy Bypass -File ./scripts/voice.ps1 -Action Upload -Port COM10
powershell -ExecutionPolicy Bypass -File ./scripts/voice.ps1 -Action Monitor -Port COM10
```

首次构建可能下载依赖。本机优先复用已安装的临时工具链；缓存被清理后会下载到`%LOCALAPPDATA%/MineVoice/platformio`。编译成功的四个镜像会复制到本工程`artifacts/`目录。`scripts/esp_sr_model.py`负责生成模型镜像并加入上传列表，仅烧录`firmware.bin`不能完成首次部署。

## LED反馈

- 亮起：正在监听。
- 闪1次：救命。
- 闪2次：撤离。
- 闪3次：瓦斯。
- 闪4次：透水。
- 闪5次：低置信度、未知命令或AFE音频处理错误；具体原因以串口输出为准。

LED表示本地识别反馈，不表示地面端已收到报文。初始化阶段的麦克风、模型和内存错误以串口错误日志为准，可能直接停止启动；不能只靠LED判断。

## 接板验收

1. 先接ESP32-S3-N16R8、INMP441、按键和可选LED，不接LoRa也能通过串口测试语音。
2. 启动串口应显示所选模型、剩余PSRAM以及`voice node ready`。若持续重启，保留完整启动日志排查。
3. 按住按键，看到“正在监听”后说一个口令，等待串口显示“识别=救命”等结果。每次测试松开按键后再按下。
4. 四个口令各试10次，记录正确、漏识别和误识别次数；再用无关说话和静音测试。模型返回的置信分数不是实测正确率。
5. 接好Ra-02和匹配的433 MHz天线后再测试无线发送。`LoRa TX`只表示本机完成发射；地面网关仍需增加协议解析后才能验证端到端接收。

## 当前边界

- 已完成完整编译和重复增量编译；固件镜像774064字节，模型镜像3260554字节。已检查上传列表包含`0x310000`处的模型镜像。尚未烧录和进行实机语音测试。
- 当前ESP-SR 2.0.0与本机组件管理器存在依赖重新配置时的校验问题：重新下载后仍能复现，原因尚未确定。当前缓存下普通编译已通过；修改依赖或强制重新配置可能再次触发`modified on the disk`。没有关闭组件校验。旧缓存保留在英文编译目录的`esp-sr-cache-backup/`中，便于后续排查。
- 该工程只能识别配置过的命令，不能把任意一句中文转成文字。
- MultiNet官方输入要求是16 kHz、16位、单声道，本工程将INMP441的24位I2S数据转换为该格式。
- 地面网关现有程序还不认识`VOICE,...`报文，需要下一步增加解析和OLED/蜂鸣器反馈。
- 实际装板前必须核对GPIO15、16、17、7、21没有被PCB其他网络占用。
