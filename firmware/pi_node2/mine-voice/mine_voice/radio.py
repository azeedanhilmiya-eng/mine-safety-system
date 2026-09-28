from __future__ import annotations

import logging
import random
import time
from typing import Protocol

from .protocol import packet_identity, parse_ack


LOGGER = logging.getLogger(__name__)


class RadioLike(Protocol):
    def send(self, data: bytes) -> None: ...
    def receive(self, *, timeout: float, keep_listening: bool = True): ...


class LoRaRadio:
    def __init__(self, config: dict) -> None:
        try:
            import adafruit_rfm9x
            import board
            import busio
            import digitalio
        except ImportError as exc:
            raise RuntimeError("未安装 Blinka/RFM9x 库，请先执行 pip install -r requirements.txt") from exc

        try:
            cs_pin = getattr(board, str(config["cs_pin"]))
            reset_pin = getattr(board, str(config["reset_pin"]))
        except AttributeError as exc:
            raise ValueError("cs_pin 或 reset_pin 名称无效，请检查 config.json") from exc

        spi = busio.SPI(board.SCK, MOSI=board.MOSI, MISO=board.MISO)
        cs = digitalio.DigitalInOut(cs_pin)
        reset = digitalio.DigitalInOut(reset_pin)
        self.radio = adafruit_rfm9x.RFM9x(
            spi, cs, reset, float(config["frequency_mhz"])
        )
        self.radio.spreading_factor = int(config["spreading_factor"])
        self.radio.signal_bandwidth = int(config["signal_bandwidth"])
        self.radio.coding_rate = int(config["coding_rate"])
        self.radio.preamble_length = int(config["preamble_length"])
        self.radio.enable_crc = bool(config["enable_crc"])
        self.radio.tx_power = int(config["tx_power"])
        # Adafruit RFM9x v2 always adds a 4-byte RadioHead header to send()
        # and strips one from receive(). Our Arduino gateway uses raw LoRa
        # payloads, so use the chip FIFO directly for both directions.
        self.radio.idle()
        self.radio._write_u8(0x39, int(config["sync_word"]))  # RegSyncWord
        if self.radio._read_u8(0x39) != int(config["sync_word"]):
            raise RuntimeError("SX1278 同步字写入失败")
        self.radio.listen()

    def send(self, data: bytes) -> None:
        if not 0 < len(data) <= 255:
            raise ValueError(f"LoRa 报文过长：{len(data)} 字节")
        self.radio.idle()
        self.radio._write_u8(0x12, 0xFF)  # clear IRQ flags
        self.radio._write_u8(0x0D, 0x00)  # FIFO address pointer
        self.radio._write_from(0x00, data)
        self.radio._write_u8(0x22, len(data))
        self.radio.transmit()
        deadline = time.monotonic() + float(self.radio.xmit_timeout)
        while time.monotonic() < deadline:
            if self.radio.tx_done():
                self.radio._write_u8(0x12, 0xFF)
                self.radio.listen()
                return
            time.sleep(0.002)
        self.radio.idle()
        self.radio._write_u8(0x12, 0xFF)
        self.radio.listen()
        raise TimeoutError("SX1278 发射超时，TxDone 未置位")

    def receive(self, *, timeout: float, keep_listening: bool = True):
        self.radio.listen()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.radio.rx_done():
                flags = self.radio._read_u8(0x12)
                self.radio.idle()
                packet = None
                if not flags & 0x20:  # PayloadCrcError
                    size = self.radio._read_u8(0x13)  # RxNbBytes
                    address = self.radio._read_u8(0x10)  # FifoRxCurrentAddr
                    self.radio._write_u8(0x0D, address)
                    packet = bytearray(size)
                    self.radio._read_into(0x00, packet)
                self.radio._write_u8(0x12, 0xFF)
                if keep_listening:
                    self.radio.listen()
                return bytes(packet) if packet is not None else None
            time.sleep(0.002)
        if not keep_listening:
            self.radio.idle()
        return None


class ReliableSender:
    def __init__(self, radio: RadioLike, config: dict) -> None:
        self.radio = radio
        self.config = config

    def send_packet(self, packet: str) -> bool:
        payload = packet.encode("utf-8")
        if not self.config["ack_enabled"]:
            self.radio.send(payload)
            LOGGER.info("已发送：%s", packet)
            return True

        node_id, sequence, part = packet_identity(packet)
        attempts = int(self.config["max_attempts"])
        for attempt in range(1, attempts + 1):
            self.radio.send(payload)
            LOGGER.info("发送 %d/%d：%s", attempt, attempts, packet)
            deadline = time.monotonic() + float(self.config["ack_timeout_seconds"])
            while time.monotonic() < deadline:
                remaining = max(0.01, deadline - time.monotonic())
                response = self.radio.receive(timeout=remaining, keep_listening=True)
                if response is None:
                    break
                ack = parse_ack(bytes(response))
                if ack and (ack.node_id, ack.sequence, ack.part) == (
                    node_id,
                    sequence,
                    part,
                ):
                    LOGGER.info("收到 ACK：seq=%d part=%d", sequence, part)
                    return True
            if attempt < attempts:
                base = int(self.config["retry_backoff_ms"]) / 1000.0
                time.sleep(base + random.uniform(0, base))
        LOGGER.error("重试后仍未收到 ACK：seq=%d part=%d", sequence, part)
        return False
