#!/usr/bin/env python3
"""Run the whole voice chain on your PC, without an ESP32.

    python demo.py --sample                  # a random clip from data/raw
    python demo.py --wav path/to/clip.wav    # a file you recorded
    python demo.py --mic                     # speak into the laptop microphone
    python demo.py --mic --listen            # keep listening until Ctrl-C
    python demo.py --sample --drop-ack       # watch a retransmission happen

What is real here and what is not:

  real   the audio frontend (the same C micro-frontend, via TensorFlow)
  real   the INT8 model, run by the TFLite interpreter
  real   the three-window vote and the accept rule -- demo.py loads the
         firmware's own kws_postprocess.c through ctypes rather than
         reimplementing it
  real   the LoRa packet: built, checksummed and parsed by the firmware's
         kws_packet.h, including the gateway's strict validation
  fake   the microphone is your laptop's, not an INMP441 on I2S
  fake   the radio is a Python variable, so nothing is ever corrupted in
         flight unless you ask for it
  fake   the OLED is drawn in the terminal, and the SMS and Firebase calls are
         printed rather than sent
  fake   the timings are your PC's. An ESP32-S3 is roughly two orders of
         magnitude slower; those numbers have to come from the board.
"""

from __future__ import annotations

import argparse
import ctypes
import random
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent
LIB = ROOT / "out" / "libkwssim.so"
MODEL = ROOT / "out" / "kws_int8.tflite"

BOLD, DIM, RESET = "\033[1m", "\033[2m", "\033[0m"
RED, GREEN, YELLOW, BLUE, CYAN = (
    "\033[31m", "\033[32m", "\033[33m", "\033[34m", "\033[36m")


# --------------------------------------------------------------- firmware ---

class KwsResult(ctypes.Structure):
    _fields_ = [("label", ctypes.c_int), ("confidence", ctypes.c_float),
                ("accepted", ctypes.c_int), ("votes", ctypes.c_int)]


def load_firmware() -> ctypes.CDLL:
    """Load the firmware's decision logic and packet code as a shared library."""
    if not LIB.exists():
        print(f"{DIM}building {LIB.name} from the firmware sources...{RESET}")
        subprocess.run(["make"], cwd=ROOT / "sim", check=True)

    lib = ctypes.CDLL(str(LIB))
    lib.sim_vote.argtypes = [ctypes.POINTER(ctypes.c_int8),
                             ctypes.POINTER(KwsResult)]
    lib.sim_packet_format.argtypes = [ctypes.c_char_p, ctypes.c_int,
                                      ctypes.c_char_p, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int]
    lib.sim_ack_format.argtypes = [ctypes.c_char_p, ctypes.c_int,
                                   ctypes.c_char_p, ctypes.c_int]
    lib.sim_packet_parse.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    lib.sim_status_text.restype = ctypes.c_char_p
    lib.sim_label.restype = ctypes.c_char_p
    lib.sim_label_zh.restype = ctypes.c_char_p
    lib.sim_threshold.restype = ctypes.c_float
    lib.sim_dequantize_prob.argtypes = [ctypes.c_int8]
    lib.sim_dequantize_prob.restype = ctypes.c_float
    return lib


def label_of(lib, i: int) -> str:
    return lib.sim_label(i).decode() if i >= 0 else "disagree"


def label_zh(lib, i: int) -> str:
    return lib.sim_label_zh(i).decode() if i >= 0 else "无法判定"


# ------------------------------------------------------------------ audio ---

def read_wav(path: Path, rate: int) -> np.ndarray:
    import soundfile as sf
    data, sr = sf.read(str(path), dtype="int16", always_2d=True)
    if sr != rate:
        raise SystemExit(f"{path} is {sr} Hz; record at {rate} Hz")
    return data[:, 0]


def record_mic(seconds: float, rate: int) -> np.ndarray:
    try:
        import sounddevice as sd
    except ImportError:
        raise SystemExit(
            "microphone input needs sounddevice:  pip install sounddevice\n"
            "(on Linux it also wants libportaudio2)\n"
            "Or point --wav at a file instead.")
    print(f"{BOLD}{RED}● recording {seconds:.1f}s -- speak now{RESET}", flush=True)
    audio = sd.rec(int(seconds * rate), samplerate=rate, channels=1,
                   dtype="int16")
    sd.wait()
    return audio[:, 0]


def pick_sample() -> Path:
    clips = sorted((ROOT / "data" / "raw").glob("*/*.wav"))
    if not clips:
        raise SystemExit(
            "no clips in data/raw. Record some with tools/serial_record.py, or "
            "run tools/make_synthetic.py for the rehearsal set.")
    return random.choice(clips)


# ----------------------------------------------------------------- render ---

OLED_COLS = 21          # 128 px / 6 px per character
OLED_ROWS = 6           # 64 px / 10 px per line


def section(title: str) -> None:
    print(f"\n{BOLD}{BLUE}── {title} {'─' * max(0, 56 - len(title))}{RESET}")


def draw_oled(lines: list[str]) -> None:
    print(f"  {DIM}┌{'─' * (OLED_COLS + 2)}┐{RESET}")
    for i in range(OLED_ROWS):
        text = lines[i] if i < len(lines) else ""
        print(f"  {DIM}│{RESET} {text:<{OLED_COLS}} {DIM}│{RESET}")
    print(f"  {DIM}└{'─' * (OLED_COLS + 2)}┘{RESET}")


# ------------------------------------------------------------------- node ---

def run_node(lib, interpreter, pcm: np.ndarray, verbose: bool,
             threshold_override: float | None = None,
             place_ms: int = 0) -> tuple:
    """Everything the ESP32-S3 does between the button and the radio."""
    from kws.frontend import features

    rate = lib.sim_sample_rate()
    clip = lib.sim_clip_samples()
    capture = lib.sim_capture_samples()
    n_win = lib.sim_num_windows()
    n_cls = lib.sim_num_classes()

    # Nobody presses a button and starts talking in the same instant, so a
    # recorded clip is placed part-way into the capture buffer. At offset 0 the
    # later windows would miss the word entirely and the vote would look far
    # worse here than on a board.
    buf = np.zeros(capture, dtype=np.int16)
    start = min(rate * place_ms // 1000, max(capture - len(pcm), 0))
    take = min(len(pcm), capture - start)
    buf[start:start + take] = pcm[:take]

    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]

    raw = np.zeros((n_win, n_cls), dtype=np.int8)
    t_feat = t_inf = 0.0
    for w in range(n_win):
        offset = rate * lib.sim_window_offset_ms(w) // 1000
        window = buf[offset:offset + clip]

        t0 = time.perf_counter()
        feat = features(window)
        t1 = time.perf_counter()
        interpreter.set_tensor(inp["index"],
                               feat.reshape(1, *feat.shape).astype(inp["dtype"]))
        interpreter.invoke()
        raw[w] = interpreter.get_tensor(out["index"])[0]
        t2 = time.perf_counter()
        t_feat += t1 - t0
        t_inf += t2 - t1

    result = KwsResult()
    lib.sim_vote(raw.ctypes.data_as(ctypes.POINTER(ctypes.c_int8)),
                 ctypes.byref(result))

    if verbose:
        for w in range(n_win):
            pick = int(np.argmax(raw[w]))
            prob = lib.sim_dequantize_prob(int(raw[w][pick]))
            print(f"  window @{lib.sim_window_offset_ms(w):>4} ms   "
                  f"{label_of(lib, pick):<10} {prob:.3f}")

    # Three decimals, not two: confidence comes off an int8 tensor in steps of
    # 1/256, so a value can print as "0.99" and still sit below a 0.99
    # threshold. Rounding it made a correct rejection look like a bug.
    firmware_threshold = lib.sim_threshold()
    accepted = bool(result.accepted)
    threshold = firmware_threshold

    if threshold_override is not None:
        threshold = threshold_override
        accepted = (result.label >= 0
                    and bool(lib.sim_is_command(result.label))
                    and result.confidence >= threshold)

    verdict = f"{GREEN}ACCEPTED{RESET}" if accepted else f"{YELLOW}rejected{RESET}"
    print(f"  vote: {BOLD}{label_of(lib, result.label)}{RESET} "
          f"({result.votes}/{n_win} windows), confidence {result.confidence:.3f}")
    print(f"  threshold {threshold:.3f} -> {verdict}")
    if threshold_override is not None:
        print(f"  {YELLOW}threshold overridden for this run{RESET}"
              f"{DIM}; the firmware has {firmware_threshold:.3f} compiled in, "
              f"which evaluate.py chose{RESET}")
    result.accepted = int(accepted)
    print(f"  {DIM}host timing: frontend {t_feat * 1000:.1f} ms, "
          f"{n_win} inferences {t_inf * 1000:.1f} ms "
          f"(an ESP32-S3 is far slower){RESET}")

    if not accepted and result.label >= 0 and not lib.sim_is_command(result.label):
        print(f"  {DIM}'{label_of(lib, result.label)}' is not a command class, "
              f"so nothing is transmitted{RESET}")
    elif (not accepted and result.votes >= 2 and result.label >= 0
          and threshold_override is None
          and result.confidence < firmware_threshold):
        print(f"  {DIM}the word was recognised but fell short of the "
              f"threshold. To see the rest of the\n  chain anyway, rerun with "
              f"--threshold {max(result.confidence - 0.05, 0.3):.2f}{RESET}")
    return result


# ---------------------------------------------------------------- gateway ---

class Gateway:
    """Mirrors processVoiceEvent() in surface_node.ino, in the same order."""

    DEDUP_MS = 10000

    def __init__(self, lib):
        self.lib = lib
        self.last_seq: int | None = None
        self.last_seq_ms = 0.0
        self.alert = None

    def receive(self, packet: str, rssi: int, now_ms: float) -> None:
        node = ctypes.create_string_buffer(8)
        cmd, conf, seq = (ctypes.c_int() for _ in range(3))
        status = self.lib.sim_packet_parse(packet.encode(), node,
                                           ctypes.byref(cmd),
                                           ctypes.byref(conf),
                                           ctypes.byref(seq))
        if status != 0:
            reason = self.lib.sim_status_text(status).decode()
            print(f"  {RED}rejected{RESET} ({reason}): {packet}")
            return

        node_id = node.value.decode()

        # Acknowledge before anything slow. The node waits 900 ms.
        ack = ctypes.create_string_buffer(24)
        self.lib.sim_ack_format(ack, 24, node_id.encode(), seq.value)
        print(f"  {CYAN}<-{RESET} {ack.value.decode()}   {DIM}(sent first: the "
              f"node only waits 900 ms){RESET}")

        duplicate = (self.last_seq == seq.value
                     and now_ms - self.last_seq_ms < self.DEDUP_MS)
        self.last_seq, self.last_seq_ms = seq.value, now_ms
        if duplicate:
            print(f"  {YELLOW}retransmission of seq={seq.value}{RESET} -- "
                  f"acked again, not alarmed on twice")
            return

        if not self.lib.sim_is_command(cmd.value):
            print(f"  non-command id {cmd.value} ignored")
            return

        self.alert = (node_id, cmd.value, conf.value, seq.value, rssi)
        self.show()

    def show(self) -> None:
        node_id, cmd, conf, seq, rssi = self.alert
        lib = self.lib
        section("GATEWAY OLED  128x64")
        draw_oled([
            "! VOICE ALERT !",
            f"Node: {node_id}",
            lib.sim_label(cmd).decode(),
            f"conf {conf}%  rssi {rssi}",
        ])
        print(f"  {DIM}with VOICE_OLED_CHINESE=1 the third line reads "
              f"'{label_zh(lib, cmd)}'{RESET}")
        print(f"\n  buzzer  : {RED}ON{RESET} for 15 s")
        print(f"  SMS     : MINE VOICE ALERT! Mine {node_id[1:]}: miner said "
              f'"{lib.sim_label(cmd).decode()}"')
        print(f"  firebase: PUT /voice_events/{node_id.lower()}/latest.json")


# ------------------------------------------------------------------- main ---

def one_capture(lib, interpreter, gateway, pcm, seq, source, args) -> int:
    section("MICROPHONE")
    rate = lib.sim_sample_rate()
    rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)))
    print(f"  source : {source}")
    print(f"  {len(pcm) / rate:.2f} s @ {rate} Hz, rms {rms:.0f}, "
          f"peak {int(np.max(np.abs(pcm)))}")
    if np.max(np.abs(pcm)) >= 32000:
        print(f"  {RED}clipping{RESET} -- on the board, lower the I2S gain shift")
    if rms < 300:
        print(f"  {YELLOW}very quiet{RESET} -- the model will hear near-silence")

    section("UNDERGROUND NODE M1  (ESP32-S3)")
    # A live recording already contains the speaker's own reaction time; a
    # stored clip does not, so it is placed where a spoken word would land.
    place_ms = 0 if args.mic else 200
    result = run_node(lib, interpreter, pcm, verbose=True,
                      threshold_override=args.threshold, place_ms=place_ms)
    if not result.accepted:
        print(f"\n  {DIM}nothing is transmitted; the gateway stays quiet{RESET}")
        return seq

    seq = seq % 255 + 1
    packet = ctypes.create_string_buffer(48)
    lib.sim_packet_format(packet, 48, b"M1", result.label,
                          int(result.confidence * 100), seq)
    text = packet.value.decode()

    section("LoRa 433 MHz")
    rssi = -71
    now = time.time() * 1000
    print(f"  {CYAN}->{RESET} {text}   {DIM}(try 1){RESET}")

    if args.drop_ack:
        print(f"  {YELLOW}...ack lost in flight{RESET}")
        gateway.receive(text, rssi, now)
        print(f"\n  {DIM}node saw no ack within 900 ms, retransmits{RESET}")
        print(f"  {CYAN}->{RESET} {text}   {DIM}(try 2){RESET}")
        gateway.receive(text, rssi, now + 1100)
    else:
        gateway.receive(text, rssi, now)
    return seq


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--wav", type=Path, help="a 16 kHz mono WAV file")
    src.add_argument("--mic", action="store_true", help="record from this PC")
    src.add_argument("--sample", action="store_true",
                     help="a random clip from data/raw (the default)")
    ap.add_argument("--listen", action="store_true",
                    help="keep going until Ctrl-C")
    ap.add_argument("--drop-ack", action="store_true",
                    help="lose the first ack, to show the retransmission")
    ap.add_argument("--model", type=Path, default=MODEL)
    ap.add_argument("--threshold", type=float, default=None,
                    help="override the threshold compiled into the firmware, "
                         "to explore the chain; it does not change the board")
    args = ap.parse_args()

    if not args.model.exists():
        raise SystemExit(f"{args.model} not found -- run ./run_smoke_test.sh "
                         "or train a real model first")

    lib = load_firmware()

    print(f"{DIM}loading TensorFlow...{RESET}", flush=True)
    import tensorflow as tf
    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()

    print(f"\n{BOLD}mine voice node -- host simulation{RESET}")
    print(f"{DIM}model {args.model.name}, {args.model.stat().st_size / 1024:.1f} KB, "
          f"{lib.sim_num_classes()} classes, threshold "
          f"{lib.sim_threshold():.2f}{RESET}")
    labels = ", ".join(f"{i}={label_of(lib, i)}"
                       for i in range(lib.sim_num_classes()))
    print(f"{DIM}{labels}{RESET}")

    provenance = ROOT / "data" / "SYNTHETIC"
    if provenance.exists():
        print(f"\n{YELLOW}{BOLD}This model was trained on synthetic data.{RESET}"
              f"{YELLOW} It will not recognise your\nvoice. Playing a clip from "
              f"data/raw shows the chain working; real\nrecognition needs real "
              f"recordings.{RESET}")

    gateway = Gateway(lib)
    seq = 0
    rate = lib.sim_sample_rate()
    capture_s = lib.sim_capture_samples() / rate

    try:
        while True:
            if args.mic:
                pcm, source = record_mic(capture_s, rate), "laptop microphone"
            elif args.wav:
                pcm, source = read_wav(args.wav, rate), str(args.wav)
            else:
                path = pick_sample()
                pcm = read_wav(path, rate)
                source = str(path.relative_to(ROOT))

            seq = one_capture(lib, interpreter, gateway, pcm, seq, source, args)

            if not args.listen:
                break
            input(f"\n{DIM}ENTER for another, Ctrl-C to stop{RESET}")
    except KeyboardInterrupt:
        print("\nstopped")

    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
