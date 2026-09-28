from __future__ import annotations

import argparse

from mine_voice.config import load_config
from mine_voice.protocol import build_text_packets
from mine_voice.radio import LoRaRadio, ReliableSender


def main() -> None:
    parser = argparse.ArgumentParser(description="SX1278 固定中文文本发送测试")
    parser.add_argument("--config", default="config.json")
    parser.add_argument("--text", default="三号巷道风机有异响")
    parser.add_argument("--sequence", type=int, default=1)
    args = parser.parse_args()
    config, _ = load_config(args.config)
    radio = LoRaRadio(config["lora"])
    sender = ReliableSender(radio, config["protocol"])
    packets = build_text_packets(
        config["node_id"],
        args.sequence,
        args.text,
        int(config["protocol"]["text_bytes_per_fragment"]),
    )
    for packet in packets:
        print(packet)
        if not sender.send_packet(packet):
            raise SystemExit(1)


if __name__ == "__main__":
    main()
