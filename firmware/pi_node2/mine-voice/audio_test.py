from __future__ import annotations

import argparse
from pathlib import Path

from mine_voice.audio import ButtonRecorder, EnergyVadRecorder, list_devices, save_wav
from mine_voice.config import load_config


def main() -> None:
    parser = argparse.ArgumentParser(description="USB 麦克风与能量 VAD 测试")
    parser.add_argument("--config", default="config.json")
    parser.add_argument("--list", action="store_true", help="列出录音设备")
    parser.add_argument("--output", default="audio-test.wav")
    args = parser.parse_args()
    if args.list:
        list_devices()
        return
    config, _ = load_config(args.config)
    button_mode = config["audio"]["trigger_mode"] == "button"
    recorder = ButtonRecorder(config["audio"]) if button_mode else EnergyVadRecorder(config["audio"])
    print("按住录音按键说话，松开后保存……" if button_mode else "请说一句话……")
    try:
        samples = recorder.capture_phrase()
    finally:
        close_recorder = getattr(recorder, "close", None)
        if close_recorder is not None:
            close_recorder()
    if samples is None:
        raise SystemExit("未录到足够长的语音；请检查按键或麦克风设备")
    output = Path(args.output).resolve()
    save_wav(output, samples, int(config["audio"]["sample_rate"]))
    print(f"已保存：{output}")


if __name__ == "__main__":
    main()
