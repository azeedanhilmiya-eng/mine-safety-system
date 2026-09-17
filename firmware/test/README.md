# firmware/test — 不用开发板就能跑的检查

```bash
cd firmware/test && make
```

两类检查：

## 1. sketch 类型检查（`check_sketch.py`）

用 `stubs/` 下的桩头文件模拟 Arduino / ESP-IDF / U8g2 / LoRa / TFLite-Micro，
再自动生成函数原型（Arduino IDE 编译时就是这么做的），交给 `g++ -fsyntax-only`。

**能抓到**：拼写错误、参数个数或类型不对、调用了对象上不存在的方法、
`String node = x.toLowerCase();` 这类返回值类型错误。

**抓不到**：桩的签名是按真实 API 写的，但如果某个真实签名和桩不一致，
这里会通过而实际编译失败。它验证的是**代码本身**，不是我们对库 API 的假设。
真正的编译验证只能在 Arduino IDE 或 PlatformIO 上做。

新增一个用到的库函数时，要同步在 `stubs/` 里加声明，否则检查会因为找不到符号而失败。

## 2. 单元测试（`../voice_node/test/`）

纯 C，不依赖任何硬件：

- `test_postprocess` — 特征量化映射、三窗投票、判决规则，对照 Python 训练流程
  导出的 525 条向量逐位比对
- `test_packet` — 语音事件报文的构造、解析、CRC，以及**对一个合法报文的所有
  单字符变异都必须拒绝**（电台迟早会给你翻转一个 bit，网关不能因此喊一次没人叫的救援）

## 自检

改坏代码后这些检查必须变红，否则它们没在工作。验证过能抓到的三类错误：

```bash
# 返回值类型错误 -> conversion from 'void' to non-scalar type 'String'
# 参数个数错误   -> too many arguments to function
# 函数名拼错     -> was not declared in this scope; did you mean ...
```
