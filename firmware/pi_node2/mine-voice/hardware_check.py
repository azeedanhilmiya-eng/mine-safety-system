from __future__ import annotations

import argparse

import sounddevice as sd

from mine_voice.config import load_config
from mine_voice.radio import LoRaRadio


def main() -> None:
    parser = argparse.ArgumentParser(description="检查麦克风、SPI 和 SX1278，不发射报文")
    parser.add_argument("--config", default="config.json")
    args = parser.parse_args()
    config, _ = load_config(args.config)

    device = config["audio"].get("device")
    info = sd.query_devices(device, "input")
    print(f"麦克风正常：device={device}, name={info['name']}, inputs={info['max_input_channels']}")

    try:
        radio = LoRaRadio(config["lora"])
        radio.radio.idle()
    except Exception as exc:
        raise SystemExit(
            "SX1278 未识别。请确认 VCC=3.3V、共地、SCK=GPIO11、MISO=GPIO9、"
            "MOSI=GPIO10、NSS=GPIO5、RST=GPIO25，并先接好 433 MHz 天线。\n"
            f"底层错误：{exc}"
        ) from exc
    print("SX1278 正常：SPI 通信与 LoRa 参数初始化成功（未发送报文）")


if __name__ == "__main__":
    main()
