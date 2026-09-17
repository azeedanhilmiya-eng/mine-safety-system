#!/usr/bin/env python3
"""Prove the board computes the same features as the training pipeline.

The board captures audio, runs its frontend, and dumps BOTH the raw PCM and
the int8 features it derived from it.  This tool recomputes the features from
that same PCM in Python and compares them element by element.

    python tools/device_parity.py --port /dev/ttyUSB0 -n 20

Any nonzero disagreement means the model on the board is being fed something
different from what it was trained on.  When that happens, do not reach for
the confidence threshold or for more training data -- fix the frontend.  The
usual cause is the firmware forgetting to call FrontendReset() before each
capture, which leaves the previous capture's noise estimate and PCAN gain in
place; the TF op the training features came from always starts clean.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from kws import config as C, serial_proto as sp  # noqa: E402
from kws.frontend import features  # noqa: E402

from serial_record import open_port  # noqa: E402


def compare(pcm: np.ndarray, device_feat: np.ndarray) -> dict:
    expected = features(pcm).reshape(-1).astype(np.int32)
    got = device_feat.reshape(-1).astype(np.int32)
    if got.size != expected.size:
        return {"ok": False, "reason": f"size {got.size} != {expected.size}"}

    diff = np.abs(got - expected)
    return {
        "ok": bool(diff.max() == 0),
        "max_abs": int(diff.max()),
        "mismatches": int((diff != 0).sum()),
        "total": int(diff.size),
        "worst_index": int(diff.argmax()),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("-n", "--count", type=int, default=20)
    ap.add_argument("--save", type=Path, default=None,
                    help="write the captured PCM and features to an npz")
    args = ap.parse_args()

    port = open_port(args.port, args.baud)
    print(f"requesting {args.count} parity captures ('p' command)\n")

    results, captures = [], []
    for i in range(args.count):
        port.reset_input_buffer()
        port.write(b"p\n")

        frames = {}
        try:
            for _ in range(2):
                f = sp.read_frame(port, lambda line: print("  |", line))
                frames[f.kind] = f
        except sp.FrameError as exc:
            print(f"[{i + 1}] capture failed: {exc}")
            continue

        if "wav" not in frames or "feat" not in frames:
            print(f"[{i + 1}] board sent {sorted(frames)}, expected wav + feat")
            continue

        pcm = frames["wav"].payload
        dev = frames["feat"].payload
        r = compare(pcm, dev)
        results.append(r)
        captures.append((pcm, dev))

        status = "OK" if r["ok"] else "MISMATCH"
        print(f"[{i + 1}] {status}  max|diff|={r.get('max_abs', '?')}  "
              f"mismatching={r.get('mismatches', '?')}/{r.get('total', '?')}")

    if not results:
        print("\nno captures completed")
        return 1

    ok = sum(1 for r in results if r["ok"])
    worst = max(r.get("max_abs", 10 ** 9) for r in results)
    print(f"\n{ok}/{len(results)} captures byte-identical, worst max|diff| = {worst}")

    if args.save and captures:
        np.savez_compressed(args.save,
                            pcm=np.stack([c[0][:C.CLIP_SAMPLES] for c in captures]),
                            device_features=np.stack([c[1] for c in captures]))
        print(f"saved {args.save}")

    if ok == len(results):
        print("\nPARITY CONFIRMED -- the board and the training pipeline agree.")
        return 0

    print("\nPARITY FAILED. The model is being fed different features from the\n"
          "ones it was trained on; accuracy numbers from the PC do not apply to\n"
          "this board until this passes. Check FrontendReset() first.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
