from __future__ import annotations

import re
from dataclasses import dataclass


def normalize_text(text: str) -> str:
    """Make ASR output safe for one-line CSV-like LoRa packets."""
    text = text.replace("\r", " ").replace("\n", " ")
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def split_utf8(text: str, max_bytes: int = 80) -> list[str]:
    """Split text without cutting a multi-byte UTF-8 character."""
    if max_bytes < 1:
        raise ValueError("max_bytes 必须大于 0")

    parts: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for character in text:
        size = len(character.encode("utf-8"))
        if size > max_bytes:
            raise ValueError(f"单个字符无法放入 {max_bytes} 字节的分片")
        if current and current_bytes + size > max_bytes:
            parts.append("".join(current))
            current = []
            current_bytes = 0
        current.append(character)
        current_bytes += size
    if current:
        parts.append("".join(current))
    return parts


def build_text_packets(
    node_id: str, sequence: int, text: str, max_text_bytes: int = 80
) -> list[str]:
    normalized = normalize_text(text)
    if not normalized:
        return []
    fragments = split_utf8(normalized, max_text_bytes)
    total = len(fragments)
    return [
        f"TXT,{node_id},{sequence},{index}/{total},{fragment}"
        for index, fragment in enumerate(fragments, start=1)
    ]


def build_status_packet(node_id: str, state: str, sequence: int) -> str:
    safe_state = normalize_text(state).replace(",", "_")
    return f"STATUS,{node_id},{safe_state},{sequence}"


@dataclass(frozen=True)
class Ack:
    node_id: str
    sequence: int
    part: int


def parse_ack(packet: bytes | str) -> Ack | None:
    try:
        text = packet.decode("utf-8") if isinstance(packet, bytes) else packet
        fields = text.strip().split(",")
        if len(fields) != 4 or fields[0] != "ACK":
            return None
        return Ack(fields[1], int(fields[2]), int(fields[3]))
    except (UnicodeDecodeError, ValueError):
        return None


def packet_identity(packet: str) -> tuple[str, int, int]:
    """Return node, sequence and part for a TXT packet."""
    fields = packet.split(",", 4)
    if len(fields) != 5 or fields[0] != "TXT":
        raise ValueError("不是有效的 TXT 报文")
    part_text, _ = fields[3].split("/", 1)
    return fields[1], int(fields[2]), int(part_text)
