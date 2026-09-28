from __future__ import annotations

from pathlib import Path

import numpy as np
import sherpa_onnx


class ParaformerAsr:
    def __init__(self, config: dict, project_dir: Path) -> None:
        model = _resolve(project_dir, config["model"])
        tokens = _resolve(project_dir, config["tokens"])
        if not model.is_file():
            raise FileNotFoundError(f"找不到 ASR 模型：{model}")
        if not tokens.is_file():
            raise FileNotFoundError(f"找不到 tokens 文件：{tokens}")
        self.sample_rate = int(config["sample_rate"])
        self.recognizer = sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=str(model),
            tokens=str(tokens),
            num_threads=int(config["num_threads"]),
            sample_rate=self.sample_rate,
            feature_dim=int(config["feature_dim"]),
            decoding_method=str(config["decoding_method"]),
        )

    def transcribe(self, pcm16: np.ndarray) -> str:
        samples = pcm16.astype(np.float32) / 32768.0
        stream = self.recognizer.create_stream()
        stream.accept_waveform(self.sample_rate, samples)
        self.recognizer.decode_stream(stream)
        return stream.result.text.strip()


def _resolve(project_dir: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (project_dir / path).resolve()
