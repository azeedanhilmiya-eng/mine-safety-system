from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path
from typing import Any


DEFAULT_CONFIG: dict[str, Any] = {
    "node_id": "M2",
    "audio": {
        "device": None,
        "trigger_mode": "vad",
        "button_gpio": 17,
        "button_debounce_ms": 30,
        "capture_sample_rate": 44100,
        "sample_rate": 16000,
        "block_ms": 30,
        "start_rms": 650,
        "stop_rms": 420,
        "start_blocks": 3,
        "silence_ms": 900,
        "pre_roll_ms": 300,
        "min_speech_ms": 350,
        "max_speech_seconds": 15,
        "wait_timeout_seconds": 5,
    },
    "asr": {
        "model": "models/model.int8.onnx",
        "tokens": "models/tokens.txt",
        "num_threads": 4,
        "sample_rate": 16000,
        "feature_dim": 80,
        "decoding_method": "greedy_search",
    },
    "lora": {
        "enabled": False,
        "frequency_mhz": 433.0,
        "cs_pin": "D5",
        "reset_pin": "D25",
        "spreading_factor": 7,
        "signal_bandwidth": 125000,
        "coding_rate": 5,
        "preamble_length": 8,
        "sync_word": 0xA3,
        "tx_power": 17,
        "enable_crc": True,
    },
    "protocol": {
        "text_bytes_per_fragment": 80,
        "inter_fragment_delay_ms": 180,
        "ack_enabled": False,
        "ack_timeout_seconds": 1.5,
        "max_attempts": 3,
        "retry_backoff_ms": 250,
    },
    "runtime": {
        "sequence_file": "state/sequence.txt",
        "log_file": "logs/mine_voice.log",
        "save_audio": False,
        "audio_directory": "recordings",
        "heartbeat_seconds": 60,
        "transcript_file": "logs/transcripts.jsonl",
    },
    "onenet": {
        "enabled": False,
        "secrets_file": "onenet_secrets.json",
    },
}


def _merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    result = deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = _merge(result[key], value)
        else:
            result[key] = value
    return result


def load_config(path: str | Path) -> tuple[dict[str, Any], Path]:
    config_path = Path(path).expanduser().resolve()
    with config_path.open("r", encoding="utf-8") as handle:
        user_config = json.load(handle)
    config = _merge(DEFAULT_CONFIG, user_config)
    _validate(config)
    return config, config_path.parent


def resolve_path(project_dir: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else (project_dir / path).resolve()


def _validate(config: dict[str, Any]) -> None:
    if not config["node_id"] or "," in config["node_id"]:
        raise ValueError("node_id 不能为空，也不能包含逗号")
    audio = config["audio"]
    if audio["trigger_mode"] not in ("vad", "button"):
        raise ValueError("audio.trigger_mode 只能是 vad 或 button")
    if int(audio["button_gpio"]) < 0:
        raise ValueError("audio.button_gpio 必须是非负 GPIO 编号")
    if not 0 <= int(audio["button_debounce_ms"]) <= 250:
        raise ValueError("audio.button_debounce_ms 必须在 0 到 250 毫秒之间")
    if float(audio["max_speech_seconds"]) <= 0:
        raise ValueError("audio.max_speech_seconds 必须大于 0")
    if audio["sample_rate"] != config["asr"]["sample_rate"]:
        raise ValueError("audio.sample_rate 必须与 asr.sample_rate 相同")
    if int(audio.get("capture_sample_rate", audio["sample_rate"])) <= 0:
        raise ValueError("capture_sample_rate 必须大于 0")
    if audio["stop_rms"] > audio["start_rms"]:
        raise ValueError("stop_rms 不能大于 start_rms")
    if not 1 <= config["protocol"]["text_bytes_per_fragment"] <= 180:
        raise ValueError("text_bytes_per_fragment 应在 1 到 180 之间")
    lora = config["lora"]
    if not 5 <= lora["spreading_factor"] <= 12:
        raise ValueError("spreading_factor 应在 5 到 12 之间")
