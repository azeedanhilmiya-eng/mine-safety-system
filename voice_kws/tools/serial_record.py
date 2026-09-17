#!/usr/bin/env python3
"""Record training clips through the board's own microphone.

Recording on the device that will run the model matters more than it sounds:
a phone's microphone has a different frequency response and a different noise
floor from the INMP441, and the gap shows up as a few points of accuracy you
cannot get back by training harder.

    python tools/serial_record.py --port /dev/ttyUSB0 --label help --speaker lin
    python tools/serial_record.py --port COM5 --label unknown --speaker lin -n 30

ENTER records one clip, `u` undoes the last one, `q` quits.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from kws import config as C, serial_proto as sp  # noqa: E402

RAW = Path(__file__).resolve().parent.parent / "data" / "raw"


def open_port(port: str, baud: int):
    try:
        import serial
    except ImportError:
        raise SystemExit("pyserial missing -- pip install pyserial")
    return serial.Serial(port, baud, timeout=10)


def next_index(out_dir: Path, speaker: str) -> int:
    existing = sorted(out_dir.glob(f"{speaker}_*.wav"))
    if not existing:
        return 0
    return max(int(p.stem.split("_", 1)[1]) for p in existing) + 1


def rms(pcm: np.ndarray) -> float:
    return float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)))


def check_quality(pcm: np.ndarray) -> list[str]:
    """Catch the recordings that would poison the dataset, at capture time."""
    warnings = []
    peak = int(np.max(np.abs(pcm))) if pcm.size else 0
    level = rms(pcm)
    if peak >= 32000:
        warnings.append(f"CLIPPING (peak {peak}) -- lower the I2S gain shift")
    if level < 300:
        warnings.append(f"very quiet (rms {level:.0f}) -- speak closer")
    if level > 12000:
        warnings.append(f"very loud (rms {level:.0f}) -- back off")
    dc = float(np.mean(pcm))
    if abs(dc) > 500:
        warnings.append(f"DC offset {dc:.0f} -- the high-pass filter is not running")
    return warnings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--label", required=True, choices=C.LABELS)
    ap.add_argument("--speaker", required=True,
                    help="short id, no underscores (it is the split key)")
    ap.add_argument("-n", "--count", type=int, default=20)
    ap.add_argument("--out", type=Path, default=RAW)
    args = ap.parse_args()

    if "_" in args.speaker:
        raise SystemExit("speaker id must not contain '_' -- it separates the index")

    out_dir = args.out / args.label
    out_dir.mkdir(parents=True, exist_ok=True)
    index = next_index(out_dir, args.speaker)

    port = open_port(args.port, args.baud)
    print(f"recording '{args.label}' as speaker '{args.speaker}' into {out_dir}")
    print("ENTER = record, u = undo last, q = quit\n")

    written: list[Path] = []
    while len(written) < args.count:
        key = input(f"[{len(written) + 1}/{args.count}] > ").strip().lower()
        if key == "q":
            break
        if key == "u":
            if written:
                written.pop().unlink()
                index -= 1
                print("  removed")
            continue

        port.reset_input_buffer()
        port.write(b"r\n")
        try:
            frame = sp.read_frame(port, lambda line: print("  |", line))
        except sp.FrameError as exc:
            print(f"  capture failed: {exc}")
            continue
        if frame.kind != "wav":
            print(f"  unexpected frame '{frame.kind}', ignoring")
            continue
        if frame.sample_rate != C.SAMPLE_RATE:
            raise SystemExit(f"board sent {frame.sample_rate} Hz, expected "
                             f"{C.SAMPLE_RATE} Hz -- fix the firmware, not this tool")

        pcm = frame.payload
        for w in check_quality(pcm):
            print(f"  ! {w}")

        path = out_dir / f"{args.speaker}_{index:04d}.wav"
        sf.write(str(path), pcm, C.SAMPLE_RATE, subtype="PCM_16")
        written.append(path)
        index += 1
        print(f"  saved {path.name}  ({pcm.size / C.SAMPLE_RATE:.2f}s, "
              f"rms {rms(pcm):.0f})")

    print(f"\n{len(written)} clips written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
