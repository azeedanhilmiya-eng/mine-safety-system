"""
SX1278 / Ra-02 (433 MHz LoRa) 的树莓派驱动, 只用 spidev + lgpio。

射频参数逐个寄存器对齐 arduino-LoRa 的 LoRa.begin() + setSyncWord(0xA3) + enableCrc(),
也就是节点1(esp32s3-kws) 和地面网关(surface_node_vscode) 实际在用的配置:
    433 MHz / SF7 / BW 125 kHz / CR 4-5 / 显式包头 / CRC 开 / 前导码 8 / 同步字 0xA3 / PA_BOOST 17 dBm
三端任何一个参数不一致都收不到包, 改这里之前先看 GPIO分配.md 里的"射频参数"一节。

与 Arduino 端的区别: 这里常驻"连续接收"模式并轮询 IRQ 寄存器, 不依赖 DIO0 中断,
所以 DIO0 可以不接; 发送完成同样靠轮询 TxDone, 带超时, 射频异常时不会卡死主循环。
"""

from __future__ import annotations

import logging
import time
from typing import Optional, Tuple

log = logging.getLogger("sx1278")

# ---- 寄存器 (与 arduino-LoRa 的 LoRa.cpp 同名) ----
REG_FIFO = 0x00
REG_OP_MODE = 0x01
REG_FRF_MSB = 0x06
REG_FRF_MID = 0x07
REG_FRF_LSB = 0x08
REG_PA_CONFIG = 0x09
REG_OCP = 0x0B
REG_LNA = 0x0C
REG_FIFO_ADDR_PTR = 0x0D
REG_FIFO_TX_BASE_ADDR = 0x0E
REG_FIFO_RX_BASE_ADDR = 0x0F
REG_FIFO_RX_CURRENT_ADDR = 0x10
REG_IRQ_FLAGS = 0x12
REG_RX_NB_BYTES = 0x13
REG_MODEM_STAT = 0x18
REG_PKT_SNR_VALUE = 0x19
REG_PKT_RSSI_VALUE = 0x1A
REG_MODEM_CONFIG_1 = 0x1D
REG_MODEM_CONFIG_2 = 0x1E
REG_PREAMBLE_MSB = 0x20
REG_PREAMBLE_LSB = 0x21
REG_PAYLOAD_LENGTH = 0x22
REG_MODEM_CONFIG_3 = 0x26
REG_SYNC_WORD = 0x39
REG_VERSION = 0x42
REG_PA_DAC = 0x4D

MODE_LONG_RANGE_MODE = 0x80
MODE_SLEEP = 0x00
MODE_STDBY = 0x01
MODE_TX = 0x03
MODE_RX_CONTINUOUS = 0x05

IRQ_TX_DONE_MASK = 0x08
IRQ_PAYLOAD_CRC_ERROR_MASK = 0x20
IRQ_RX_DONE_MASK = 0x40

RSSI_OFFSET_LF_PORT = 164   # 433 MHz 属于低频口, 与 arduino-LoRa 的 packetRssi() 一致
MAX_PKT_LENGTH = 255

# ---- 射频参数 (三端必须一致) ----
FREQUENCY_HZ = 433_000_000
SYNC_WORD = 0xA3
# ModemConfig1 = BW 125 kHz(0111) | CR 4/5(001) | 显式包头(0)       -> 0x72 (芯片复位默认值)
# ModemConfig2 = SF7(0111) | 单包模式(0) | CRC 开(1) | SymbTimeout 高位 00 -> 0x74
# ModemConfig3 = AGC 自动(0x04), arduino-LoRa begin() 里写的就是这个值
MODEM_CONFIG_1 = 0x72
MODEM_CONFIG_2 = 0x74
MODEM_CONFIG_3 = 0x04
PREAMBLE_LENGTH = 8

TX_TIMEOUT_S = 1.0   # 100 字节 SF7 空中时间约 170 ms, 1 s 足够; 超时说明射频异常


class RadioError(RuntimeError):
    pass


class SX1278:
    """spidev 驱动的 SX1278。RESET 脚可选: 不接/没有 lgpio 时跳过硬件复位。"""

    def __init__(self, spi_bus: int = 0, spi_dev: int = 0, reset_gpio: Optional[int] = 25,
                 gpiochip: Optional[int] = None, spi_hz: int = 1_000_000):
        self.spi_bus = spi_bus
        self.spi_dev = spi_dev
        self.reset_gpio = reset_gpio
        self.gpiochip = gpiochip
        self.spi_hz = spi_hz
        self._spi = None
        self._gpio = None       # lgpio 句柄
        self._lgpio = None

    # ---------------- 底层 SPI ----------------
    def _read(self, reg: int) -> int:
        return self._spi.xfer2([reg & 0x7F, 0x00])[1]

    def _write(self, reg: int, value: int) -> None:
        self._spi.xfer2([reg | 0x80, value & 0xFF])

    def _mode(self, mode: int) -> None:
        self._write(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode)

    # ---------------- 初始化 ----------------
    def _hardware_reset(self) -> None:
        if self.reset_gpio is None:
            return
        try:
            import lgpio  # 树莓派 OS Bookworm 自带 python3-lgpio, 树莓派 5 上 RPi.GPIO 不可用
        except ImportError:
            log.warning("未安装 lgpio, 跳过 RESET 硬件复位 (sudo apt install python3-lgpio)")
            return

        # 树莓派 5 的 40 针排针在 RP1 上: 新内核是 gpiochip0(另有 gpiochip4 软链接),
        # 早期内核是 gpiochip4; 树莓派 4 及以前是 gpiochip0。依次尝试即可。
        chips = [self.gpiochip] if self.gpiochip is not None else [4, 0]
        last_err = None
        for chip in chips:
            try:
                handle = lgpio.gpiochip_open(chip)
            except Exception as e:  # lgpio.error
                last_err = e
                continue
            try:
                lgpio.gpio_claim_output(handle, self.reset_gpio, 1)
            except Exception as e:
                lgpio.gpiochip_close(handle)
                last_err = e
                continue
            self._lgpio, self._gpio = lgpio, handle
            lgpio.gpio_write(handle, self.reset_gpio, 0)
            time.sleep(0.01)
            lgpio.gpio_write(handle, self.reset_gpio, 1)   # 之后一直保持高电平
            time.sleep(0.01)
            log.info("RESET 复位完成 (gpiochip%d, GPIO%d)", chip, self.reset_gpio)
            return
        log.warning("RESET 引脚 GPIO%s 申请失败, 跳过硬件复位: %s", self.reset_gpio, last_err)

    def begin(self) -> None:
        import spidev  # sudo apt install python3-spidev, 并在 raspi-config 里打开 SPI

        self._hardware_reset()
        self._spi = spidev.SpiDev()
        try:
            self._spi.open(self.spi_bus, self.spi_dev)
        except FileNotFoundError as e:
            raise RadioError(
                f"找不到 /dev/spidev{self.spi_bus}.{self.spi_dev}: 先在 /boot/firmware/config.txt "
                f"加 dtparam=spi=on 并重启") from e
        self._spi.max_speed_hz = self.spi_hz
        self._spi.mode = 0

        version = self._read(REG_VERSION)
        if version != 0x12:
            hint = {0x00: "MISO 一直为低: 模块没供电或 MISO 线接错",
                    0xFF: "MISO 悬空: 模块没响应, 检查 3.3V/GND/NSS(CE0)/SCK"}.get(
                        version, "不是 SX1276/77/78")
            raise RadioError(f"未检测到 SX1278 (REG_VERSION=0x{version:02X}), {hint}")

        # LongRangeMode 位只能在 sleep 模式下切换
        self._mode(MODE_SLEEP)
        time.sleep(0.01)

        frf = (FREQUENCY_HZ << 19) // 32_000_000
        self._write(REG_FRF_MSB, frf >> 16)
        self._write(REG_FRF_MID, frf >> 8)
        self._write(REG_FRF_LSB, frf)

        self._write(REG_FIFO_TX_BASE_ADDR, 0)
        self._write(REG_FIFO_RX_BASE_ADDR, 0)
        self._write(REG_LNA, self._read(REG_LNA) | 0x03)        # LNA boost
        self._write(REG_MODEM_CONFIG_1, MODEM_CONFIG_1)
        self._write(REG_MODEM_CONFIG_2, MODEM_CONFIG_2)
        self._write(REG_MODEM_CONFIG_3, MODEM_CONFIG_3)
        self._write(REG_PREAMBLE_MSB, PREAMBLE_LENGTH >> 8)
        self._write(REG_PREAMBLE_LSB, PREAMBLE_LENGTH & 0xFF)
        self._write(REG_SYNC_WORD, SYNC_WORD)

        # setTxPower(17): PA_BOOST, PA_DAC 默认, OCP 100 mA
        self._write(REG_PA_DAC, 0x84)
        self._write(REG_OCP, 0x20 | ((100 - 45) // 5))
        self._write(REG_PA_CONFIG, 0x80 | (17 - 2))

        self._mode(MODE_STDBY)
        self._start_rx()
        log.info("SX1278 就绪: 433 MHz, SF7/125k/4-5, CRC 开, sync=0x%02X, 连续接收", SYNC_WORD)

    def close(self) -> None:
        if self._spi is not None:
            try:
                self._mode(MODE_SLEEP)
            finally:
                self._spi.close()
                self._spi = None
        if self._gpio is not None:
            self._lgpio.gpiochip_close(self._gpio)
            self._gpio = None

    # ---------------- 收 ----------------
    def _start_rx(self) -> None:
        self._write(REG_IRQ_FLAGS, 0xFF)
        self._mode(MODE_RX_CONTINUOUS)

    def receive(self) -> Optional[Tuple[bytes, int, float]]:
        """轮询一次; 收到完整且 CRC 正确的包时返回 (负载, RSSI dBm, SNR dB)。"""
        flags = self._read(REG_IRQ_FLAGS)
        if not flags & IRQ_RX_DONE_MASK:
            return None
        self._write(REG_IRQ_FLAGS, flags)
        if flags & IRQ_PAYLOAD_CRC_ERROR_MASK:
            log.debug("收到 CRC 错误包, 已丢弃")
            return None

        length = self._read(REG_RX_NB_BYTES)
        self._write(REG_FIFO_ADDR_PTR, self._read(REG_FIFO_RX_CURRENT_ADDR))
        data = bytes(self._spi.xfer2([REG_FIFO] + [0x00] * length)[1:]) if length else b""
        rssi = self._read(REG_PKT_RSSI_VALUE) - RSSI_OFFSET_LF_PORT
        snr_raw = self._read(REG_PKT_SNR_VALUE)
        snr = (snr_raw - 256 if snr_raw > 127 else snr_raw) / 4.0
        return data, rssi, snr

    def channel_busy(self) -> bool:
        """先听后发: 调制解调器检测到前导码/正在同步/包头有效时认为信道被占用。"""
        return bool(self._read(REG_MODEM_STAT) & 0x0B)

    # ---------------- 发 ----------------
    def send(self, payload: bytes) -> bool:
        """阻塞发送一包, 发完(或超时)后回到连续接收。返回是否确认发送完成。"""
        if not 0 < len(payload) <= MAX_PKT_LENGTH:
            raise ValueError(f"负载长度 {len(payload)} 超出 1..{MAX_PKT_LENGTH}")

        self._mode(MODE_STDBY)
        self._write(REG_IRQ_FLAGS, 0xFF)
        self._write(REG_FIFO_ADDR_PTR, 0)
        self._spi.xfer2([REG_FIFO | 0x80] + list(payload))
        self._write(REG_PAYLOAD_LENGTH, len(payload))
        self._mode(MODE_TX)

        deadline = time.monotonic() + TX_TIMEOUT_S
        ok = False
        while time.monotonic() < deadline:
            if self._read(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK:
                ok = True
                break
            time.sleep(0.002)
        if not ok:
            log.error("发送超时: TxDone 未置位, 检查天线与 3.3V 供电")
            self._mode(MODE_STDBY)
        self._start_rx()
        return ok
