from __future__ import annotations

import logging
import json
import threading
import time
from datetime import datetime
from pathlib import Path

from .asr import ParaformerAsr
from .audio import ButtonRecorder, EnergyVadRecorder, save_wav
from .config import resolve_path
from .protocol import build_status_packet, build_text_packets, normalize_text
from .radio import LoRaRadio, ReliableSender
from .sequence import SequenceStore
from .onenet import OneNetPublisher


LOGGER = logging.getLogger(__name__)


class MineVoiceApp:
    def __init__(self, config: dict, project_dir: Path) -> None:
        self.config = config
        self.project_dir = project_dir
        self.node_id = str(config["node_id"])
        self.recorder = (
            ButtonRecorder(config["audio"])
            if config["audio"]["trigger_mode"] == "button"
            else EnergyVadRecorder(config["audio"])
        )
        self.asr = ParaformerAsr(config["asr"], project_dir)
        self.radio = None
        self.sender = None
        if bool(config["lora"].get("enabled", True)):
            self.radio = LoRaRadio(config["lora"])
            self.sender = ReliableSender(self.radio, config["protocol"])
        sequence_path = resolve_path(project_dir, config["runtime"]["sequence_file"])
        self.sequences = SequenceStore(sequence_path)
        self.sequence_lock = threading.Lock()
        self.radio_lock = threading.Lock()
        self.heartbeat_stop = threading.Event()
        self.onenet = (
            OneNetPublisher(resolve_path(project_dir, config["onenet"]["secrets_file"]))
            if config["onenet"]["enabled"] else None
        )

    def run(self) -> None:
        LOGGER.info(
            "M2 节点启动，等待语音；LoRa=%s",
            "开启" if self.radio is not None else "关闭",
        )
        if self.radio is not None:
            self._send_heartbeat("ASR_READY")
        interval = int(self.config["runtime"]["heartbeat_seconds"])
        heartbeat_thread = None
        if self.radio is not None and interval > 0:
            heartbeat_thread = threading.Thread(
                target=self._heartbeat_loop, args=(interval,), daemon=True
            )
            heartbeat_thread.start()
        try:
            while True:
                samples = self.recorder.capture_phrase()
                if samples is None:
                    continue
                self._save_audio_if_enabled(samples)
                started = time.monotonic()
                text = normalize_text(self.asr.transcribe(samples))
                LOGGER.info("识别耗时 %.2f 秒，结果：%s", time.monotonic() - started, text or "<空>")
                if not text:
                    continue
                sequence = self._next_sequence()
                self._save_transcript(sequence, text)
                if self.onenet is not None:
                    self.onenet.publish_text(text)
                if self.radio is None or self.sender is None:
                    LOGGER.info("LoRa 已关闭，识别文字只保存到本地：%s", text)
                    continue
                packets = build_text_packets(
                    self.node_id,
                    sequence,
                    text,
                    int(self.config["protocol"]["text_bytes_per_fragment"]),
                )
                all_ok = True
                with self.radio_lock:
                    for index, packet in enumerate(packets):
                        if not self.sender.send_packet(packet):
                            all_ok = False
                            break
                        if index + 1 < len(packets):
                            time.sleep(int(self.config["protocol"]["inter_fragment_delay_ms"]) / 1000.0)
                LOGGER.info("消息 seq=%d 发送%s，共 %d 片", sequence, "完成" if all_ok else "失败", len(packets))
        finally:
            self.heartbeat_stop.set()
            if heartbeat_thread is not None:
                heartbeat_thread.join(timeout=2)
            close_recorder = getattr(self.recorder, "close", None)
            if close_recorder is not None:
                close_recorder()
            if self.onenet is not None:
                self.onenet.close()

    def _next_sequence(self) -> int:
        with self.sequence_lock:
            return self.sequences.next()

    def _heartbeat_loop(self, interval: int) -> None:
        while not self.heartbeat_stop.wait(interval):
            try:
                with self.radio_lock:
                    self._send_heartbeat("ASR_READY")
            except Exception:
                LOGGER.exception("LoRa 心跳发送失败")

    def _send_heartbeat(self, state: str) -> None:
        if self.radio is None:
            return
        sequence = self._next_sequence()
        packet = build_status_packet(self.node_id, state, sequence)
        self.radio.send(packet.encode("utf-8"))
        LOGGER.info("已发送心跳：%s", packet)

    def _save_audio_if_enabled(self, samples) -> None:
        if not self.config["runtime"]["save_audio"]:
            return
        directory = resolve_path(self.project_dir, self.config["runtime"]["audio_directory"])
        filename = datetime.now().strftime("%Y%m%d_%H%M%S_%f.wav")
        save_wav(directory / filename, samples, int(self.config["audio"]["sample_rate"]))

    def _save_transcript(self, sequence: int, text: str) -> None:
        path = resolve_path(
            self.project_dir,
            self.config["runtime"].get("transcript_file", "logs/transcripts.jsonl"),
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
            "node_id": self.node_id,
            "sequence": sequence,
            "text": text,
        }
        with path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
