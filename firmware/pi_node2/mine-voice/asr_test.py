from __future__ import annotations

import argparse
import wave

import numpy as np

from mine_voice.asr import ParaformerAsr
from mine_voice.config import load_config


def read_wav(path: str, expected_rate: int) -> np.ndarray:
    with wave.open(path, "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise ValueError("WAV 必须是 16-bit 单声道")
        if handle.getframerate() != expected_rate:
            raise ValueError(f"WAV 采样率必须是 {expected_rate} Hz")
        return np.frombuffer(handle.readframes(handle.getnframes()), dtype="<i2").copy()


def main() -> None:
    parser = argparse.ArgumentParser(description="离线 Paraformer ASR 测试")
    parser.add_argument("wav")
    parser.add_argument("--config", default="config.json")
    args = parser.parse_args()
    config, project_dir = load_config(args.config)
    recognizer = ParaformerAsr(config["asr"], project_dir)
    audio = read_wav(args.wav, int(config["asr"]["sample_rate"]))
    print(recognizer.transcribe(audio))


if __name__ == "__main__":
    main()
