# voice_kws — 井下安全关键词识别模型

为 `firmware/voice_node` 训练一个 4 类离线关键词识别模型：`silence / unknown / 救命 / 撤离`，
INT8 量化后部署到 ESP32-S3，识别结果以指令编号经现有 LoRa 链路发往地面网关。

## 为什么这样搭

整个流程只围绕一件事：**让开发板看到的特征和训练时看到的特征完全相同**。

训练侧的特征来自 TensorFlow 的 `audio_microfrontend`，而它和 tflite-micro 编进固件的
micro frontend 是**同一份 C 代码**。不是"两边都实现一遍 MFCC 然后希望它们一致"，
是同一份实现跑两次。`tools/device_parity.py` 会在真板子上验证这一点。

所有共享参数只有一个出处：`kws/config.py`。固件头文件 `kws_config.h` 由它生成，
所以两边不可能偷偷分叉。

## 环境

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

TensorFlow 版本是**锁死的**。`audio_microfrontend` 的可用性和参数签名在不同版本间变动过
（它没有 `enable_noise_reduction` 这样的开关，降噪由 `smoothing_bits` 那组参数控制），
2.15.1 是本流程实际验证过的版本。

## 先跑一遍空转

在录到任何真实数据之前，先确认工具链是通的：

```bash
./run_smoke_test.sh
```

它用合成音频跑完 训练 → 量化 → 评估 → 导出 C 数组 → 主机端测试 的全过程，约一分钟。
产物会被打上 `PLACEHOLDER` 标记，**不可能被误当成真模型**：
`data/SYNTHETIC` 标记文件会一路传到 `kws_config.h` 和 `kws_model_data.cc` 的注释里。

## 在电脑上体验整条链路

不需要开发板：

```bash
python demo.py --sample                   # 随机播一条 data/raw 里的音频
python demo.py --wav path/to/clip.wav     # 播你自己录的
python demo.py --mic                      # 对着笔记本麦克风说话
python demo.py --mic --listen             # 连续模式，Ctrl-C 退出
python demo.py --sample --drop-ack        # 故意丢掉 ACK，看重传和去重
```

输出会依次走完：麦克风 → 三个窗的识别结果 → 投票与判决 → LoRa 报文 → ACK →
网关 OLED（终端里画出 128×64 的版面）→ 蜂鸣器/短信/Firebase。

**哪些是真的**：

| | |
|---|---|
| 音频前端 | 真的，就是固件用的那份 C micro-frontend |
| INT8 模型 | 真的，TFLite 解释器跑同一个 `.tflite` |
| 三窗投票和判决 | **真的**，通过 ctypes 加载固件的 `kws_postprocess.c`，不是 Python 重写 |
| LoRa 报文 | **真的**，构造/校验/解析都由固件的 `kws_packet.h` 完成，含网关的严格校验 |
| 麦克风 | 假的，是笔记本麦不是 I2S 上的 INMP441 |
| 电台 | 假的，是个 Python 变量，不会自己出错 |
| OLED / 短信 / Firebase | 假的，打印出来而不是真发 |
| 耗时数字 | **是你电脑的**，ESP32-S3 大约慢两个数量级，这个数只能从板子上取 |

`--mic` 需要 `pip install sounddevice`（Linux 还要 `libportaudio2`）。没装会提示你改用 `--wav`。

`--threshold` 可以临时覆盖固件里编译进去的阈值，**只影响这次演示，不改板子**。
占位模型的阈值是 0.99（扫出来的极端值），不覆盖的话你看不到网关那一段。
程序在"词认出来了但差一点"时会主动提示你该用什么值。

第一次运行会自动把固件的 C 代码编译成 `out/libkwssim.so`。

## 真实流程

### 1. 采集

```bash
python tools/serial_record.py --port /dev/ttyUSB0 --label help     --speaker lin -n 15
python tools/serial_record.py --port /dev/ttyUSB0 --label evacuate --speaker lin -n 15
python tools/serial_record.py --port /dev/ttyUSB0 --label unknown  --speaker lin -n 30
python tools/serial_record.py --port /dev/ttyUSB0 --label silence  --speaker lin -n 10
```

**用板子上那支 INMP441 录，不要用手机。** 手机麦克风的频响和底噪和 INMP441 不一样，
这个差异会直接吃掉几个百分点，而且训练时看不出来，上板才暴露。

工具会在每条录完后当场检查削顶、过轻、直流偏置，问题录音当场重录比事后筛查便宜得多。

说话人 id 不能含下划线（下划线是索引分隔符），它是训练/测试划分的唯一依据。

目录结构：

```
data/raw/<label>/<speaker>_<index>.wav      16 kHz 单声道 int16
data/noise/*.wav                            机械噪声、风机、环境底噪，任意长度
```

**说话人数量是这个项目最大的风险。** 方案文档写的 5 人，按说话人划分后训练集只剩 4 人，
模型会记住这 4 个人的音色。评委现场随便叫一个人试，大概率翻车。**至少 10 人。**

`unknown` 类比关键词本身更重要，它决定误触发率。必须包含音近词
（救火、求求你、救护车 / 撤退、彻底、车里），只放"日常聊天"是不够的——
那样模型学到的只是"有没有人在大声说话"。

### 2. 训练

```bash
python train.py --variants 8 --test-speakers zhang,wang
```

`--test-speakers` 用来把"专门录来当陌生人"的同学钉死在测试集。没指定时按稳定哈希配额划分，
保证 train/val/test 都不为空。

划分结果写在 `out/report/split.json`，训练是可复现的（固定 seed 跑两次结果逐字节相同）。

### 3. 量化

```bash
python quantize.py
```

全整型 INT8。代表性数据集从**训练特征**分层采样，覆盖所有类别和所有噪声档——
只用干净样本是经典错误，激活范围会取窄，风机一开就崩。

INT8 相对 float 掉幅超过 2 个百分点会直接失败退出，不会让你带着一个悄悄变差的模型往下走。

当前模型的量化参数：输入 `scale=1.0, zero_point=0`，所以前端算出的 int8 特征
**直接拷进输入张量即可，不需要任何重新缩放**；输出 `scale=1/256, zero_point=-128`，
概率就是 `(q + 128) / 256`。

### 4. 评估与定阈值

```bash
python evaluate.py --max-false-trigger-rate 0.01 --min-keyword-recall 0.60
```

**评估用的是固件真正在跑的规则**：把每条测试音频放进 1.4 秒采集缓冲的随机位置
（模拟"按下按键到开口说话"的时间差），切出 3 个窗，走和 `kws_postprocess.c`
相同的投票判决。

这件事必须做对。单窗 argmax 和三窗投票是**两条不同的规则**：投票取的是投给获胜类别的
那些窗中的最高置信度，必然 ≥ 单窗值，所以按单窗标定的阈值拿到板上会更容易触发，
报告里的误触发率会低于实际。在占位模型上实测两者召回差 15 个百分点。

单窗结果仍会作为诊断项一并打印，但明确标注了它不是设备规则。

阈值不是拍脑袋定的，是在测试集上扫出来的：在满足误触发预算的阈值里取**最小**的那个
（最小的保留最多召回，而召回才是救人的那一半）。

两个条件都要满足才算通过。只看误触发预算是不够的——**一个从不触发的模型误触发率是 0**，
所以必须同时要求最低召回。任何一个不满足，输出会打出醒目的 `NO USABLE OPERATING POINT`
并说明是"太吵"还是"太聋"，退出码非零。

产物在 `out/report/`：`confusion.txt/png`、`threshold_sweep.csv/png`、`threshold.json`。
这些就是提交文档里要用的图表。

`out/` 整个被 gitignore，包括 report。原因是合成数据跑出来的图和真实数据跑出来的
长得一模一样，留在仓库里迟早会有人把占位图贴进提交文档。等真实数据训完之后，
需要的话用 `git add -f out/report` 显式加进来。

### 5. 导出给固件

```bash
python tools/export_c_array.py
python tools/export_test_vectors.py
make -C ../firmware/voice_node/test
```

生成 `kws_model_data.cc`（模型 C 数组）、`kws_config.h`（全部共享参数 + 选定阈值）
和决策逻辑的测试向量。主机端测试会校验固件的 C 实现和 Python 逐位一致。

**每次重训都要重跑这三条。** 测试里有一条专门检查 `kws_config.h` 的阈值和测试向量的阈值
是否一致，忘记重新导出会当场失败。

### 6. 上板一致性校验（不可跳过）

```bash
python tools/device_parity.py --port /dev/ttyUSB0 -n 20
```

板子采一段音频，同时吐出原始 PCM 和它自己算的特征；工具用同一段 PCM 在 Python 里重算，
逐元素比对。**必须 20/20 逐字节相同。**

对不上就说明模型吃到的东西和训练时不一样，这时候不要去调阈值、不要去加数据——
去查前端。最常见的原因是固件忘了在每次采集前调 `FrontendReset()`，
把上一次的噪声估计和 PCAN 增益带了进来，而训练侧的 TF op 每次都是从干净状态开始的。

这一关不过，PC 上的准确率对这块板子就是无效的。

## 文件

```
kws/config.py        所有共享参数的唯一出处；改这里就要重新导出固件头文件
kws/frontend.py      micro frontend 封装 + int8 量化（与固件 C 代码逐位一致）
kws/augment.py       时移、噪声混合、增益、变速、SpecAugment
kws/dataset.py       扫描、按说话人配额划分、特征缓存
kws/model.py         DS-CNN，23,812 参数，5.31 M MAC
kws/metrics.py       混淆矩阵、阈值扫描、误触发统计、绘图
kws/postprocess.py   三窗投票与判决规则（固件 kws_postprocess.c 的 Python 镜像）
kws/serial_proto.py  串口帧格式（可脱离硬件测试）

tools/serial_record.py      通过板子采集训练音频
tools/device_parity.py      端侧特征一致性校验
tools/make_synthetic.py     合成数据，仅用于空转验证
tools/export_c_array.py     生成 kws_model_data.cc / kws_config.h
tools/export_test_vectors.py 生成主机端测试向量
```

## 当前状态

`run_smoke_test.sh` 全流程已验证可跑通。`out/` 和 `firmware/voice_node/` 里现有的模型
是**合成数据训练的占位模型，认不出任何人**，已被打上 PLACEHOLDER 标记。
录到真实数据后重跑第 1–6 步替换掉。

## 提交材料对应关系

| 大赛要求 | 产物 |
|---|---|
| 关键词数据说明 | `out/report/dataset.txt`、`split.json`、`provenance.json` |
| 训练配置 | `kws/config.py`、`out/report/` 下各 json |
| INT8 模型 | `out/kws_int8.tflite` |
| 模型 C 数组 | `firmware/voice_node/kws_model_data.cc` |
| 类别表 | `kws/config.py: LABELS`、`kws_config.h: kKwsLabels` |
| 精度测试 | `out/report/confusion.txt/png`、`threshold_sweep.csv/png` |
| 开发板内存报告 | 固件 `'i'` 命令输出的 `arena_used_bytes` 等 |

第三方语料（若用于扩充 `unknown` 类）的名称、版本、许可证需在提交材料中如实声明。
