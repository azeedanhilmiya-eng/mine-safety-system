from __future__ import annotations

import collections
import logging
import math
import time
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd


LOGGER = logging.getLogger(__name__)


def list_devices() -> None:
    print(sd.query_devices())


def _rms(samples: np.ndarray) -> float:
    if samples.size == 0:
        return 0.0
    values = samples.astype(np.float64)
    return math.sqrt(float(np.mean(values * values)))


class EnergyVadRecorder:
    """Small energy-based VAD suitable for project prototyping."""

    def __init__(self, config: dict) -> None:
        self.config = config
        self.sample_rate = int(config["sample_rate"])
        self.capture_sample_rate = int(config.get("capture_sample_rate", self.sample_rate))
        self.block_ms = int(config["block_ms"])
        self.frames_per_block = self.capture_sample_rate * self.block_ms // 1000

    def capture_phrase(self) -> np.ndarray | None:
        cfg = self.config
        pre_roll_blocks = max(1, int(cfg["pre_roll_ms"]) // self.block_ms)
        silence_blocks = max(1, int(cfg["silence_ms"]) // self.block_ms)
        min_blocks = max(1, int(cfg["min_speech_ms"]) // self.block_ms)
        max_blocks = max(1, int(float(cfg["max_speech_seconds"]) * 1000) // self.block_ms)
        wait_deadline = time.monotonic() + float(cfg["wait_timeout_seconds"])
        pre_roll: collections.deque[np.ndarray] = collections.deque(maxlen=pre_roll_blocks)
        captured: list[np.ndarray] = []
        high_blocks = 0
        quiet_blocks = 0
        speaking = False

        with sd.InputStream(
            samplerate=self.capture_sample_rate,
            device=cfg.get("device"),
            channels=1,
            dtype="int16",
            blocksize=self.frames_per_block,
        ) as stream:
            while True:
                block, overflowed = stream.read(self.frames_per_block)
                if overflowed:
                    LOGGER.warning("音频输入发生 overflow")
                mono = np.asarray(block[:, 0], dtype=np.int16).copy()
                level = _rms(mono)

                if not speaking:
                    pre_roll.append(mono)
                    high_blocks = high_blocks + 1 if level >= float(cfg["start_rms"]) else 0
                    if high_blocks >= int(cfg["start_blocks"]):
                        speaking = True
                        captured.extend(pre_roll)
                        LOGGER.info("检测到语音，RMS=%.0f", level)
                    elif time.monotonic() >= wait_deadline:
                        return None
                    continue

                captured.append(mono)
                quiet_blocks = quiet_blocks + 1 if level <= float(cfg["stop_rms"]) else 0
                if quiet_blocks >= silence_blocks or len(captured) >= max_blocks:
                    break

        if len(captured) < min_blocks:
            LOGGER.info("语音过短，已忽略")
            return None
        captured_samples = np.concatenate(captured)
        LOGGER.info(
            "录音结束，时长 %.2f 秒，%d Hz -> %d Hz",
            captured_samples.size / self.capture_sample_rate,
            self.capture_sample_rate,
            self.sample_rate,
        )
        return _resample_linear(captured_samples, self.capture_sample_rate, self.sample_rate)


class ButtonRecorder:
    """Active-low push-to-talk button, with the other button terminal at GND."""

    def __init__(self, config: dict) -> None:
        self.config = config
        self.sample_rate = int(config["sample_rate"])
        self.capture_sample_rate = int(config.get("capture_sample_rate", self.sample_rate))
        self.block_ms = int(config["block_ms"])
        self.frames_per_block = self.capture_sample_rate * self.block_ms // 1000
        self.button_gpio = int(config["button_gpio"])
        self.debounce_seconds = int(config["button_debounce_ms"]) / 1000.0
        self._lgpio = None
        self._gpio_handle = None

    def _ensure_button(self) -> None:
        if self._gpio_handle is not None:
            return
        try:
            import lgpio
        except ImportError as exc:
            raise RuntimeError("按键模式需要 python3-lgpio，请运行 sudo apt install python3-lgpio") from exc

        errors = []
        for chip in (0, 4):
            try:
                handle = lgpio.gpiochip_open(chip)
            except Exception as exc:
                errors.append(f"gpiochip{chip}: {exc}")
                continue
            try:
                lgpio.gpio_claim_input(handle, self.button_gpio, lgpio.SET_PULL_UP)
            except Exception as exc:
                lgpio.gpiochip_close(handle)
                errors.append(f"gpiochip{chip}/GPIO{self.button_gpio}: {exc}")
                continue
            self._lgpio = lgpio
            self._gpio_handle = handle
            LOGGER.info("按键就绪：GPIO%d，内部上拉，按下接地", self.button_gpio)
            return
        raise RuntimeError("无法申请录音按键引脚：" + "; ".join(errors))

    def _pressed(self) -> bool:
        return self._lgpio.gpio_read(self._gpio_handle, self.button_gpio) == 0

    def close(self) -> None:
        if self._gpio_handle is not None:
            self._lgpio.gpiochip_close(self._gpio_handle)
            self._gpio_handle = None

    def capture_phrase(self) -> np.ndarray | None:
        self._ensure_button()
        while True:
            if self._pressed():
                time.sleep(self.debounce_seconds)
                if self._pressed():
                    break
            time.sleep(0.01)

        cfg = self.config
        min_blocks = max(1, int(cfg["min_speech_ms"]) // self.block_ms)
        max_blocks = max(1, int(float(cfg["max_speech_seconds"]) * 1000) // self.block_ms)
        captured: list[np.ndarray] = []
        released_since = None
        LOGGER.info("按键按下，开始录音")
        with sd.InputStream(
            samplerate=self.capture_sample_rate,
            device=cfg.get("device"),
            channels=1,
            dtype="int16",
            blocksize=self.frames_per_block,
        ) as stream:
            if not self._pressed():
                LOGGER.info("按键在麦克风启动前已松开，本次忽略")
                return None
            while len(captured) < max_blocks:
                block, overflowed = stream.read(self.frames_per_block)
                if overflowed:
                    LOGGER.warning("音频输入发生 overflow")
                captured.append(np.asarray(block[:, 0], dtype=np.int16).copy())
                if self._pressed():
                    released_since = None
                elif released_since is None:
                    released_since = time.monotonic()
                elif time.monotonic() - released_since >= self.debounce_seconds:
                    break

        if len(captured) >= max_blocks:
            LOGGER.warning("按键录音达到 %.0f 秒上限，等待松开", float(cfg["max_speech_seconds"]))
            while self._pressed():
                time.sleep(0.02)
        else:
            LOGGER.info("按键松开，录音结束")

        if len(captured) < min_blocks:
            LOGGER.info("按键录音过短，已忽略")
            return None
        samples = np.concatenate(captured)
        LOGGER.info(
            "录音结束，时长 %.2f 秒，%d Hz -> %d Hz",
            samples.size / self.capture_sample_rate,
            self.capture_sample_rate,
            self.sample_rate,
        )
        return _resample_linear(samples, self.capture_sample_rate, self.sample_rate)


def _resample_linear(samples: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
    """Resample mono PCM16 with linear interpolation for ASR input."""
    if source_rate == target_rate or samples.size == 0:
        return samples.astype(np.int16, copy=False)
    output_size = max(1, round(samples.size * target_rate / source_rate))
    source_positions = np.arange(samples.size, dtype=np.float64)
    target_positions = np.arange(output_size, dtype=np.float64) * source_rate / target_rate
    target_positions = np.minimum(target_positions, samples.size - 1)
    output = np.interp(target_positions, source_positions, samples.astype(np.float64))
    return np.clip(np.rint(output), -32768, 32767).astype(np.int16)


def save_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(sample_rate)
        handle.writeframes(samples.astype("<i2", copy=False).tobytes())
