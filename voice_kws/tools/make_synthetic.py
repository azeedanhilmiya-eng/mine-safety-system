#!/usr/bin/env python3
"""Generate synthetic speech-like clips so the pipeline can be exercised today.

This is NOT training data.  It is a formant-synthesiser stand-in whose only
job is to prove that scan -> split -> augment -> frontend -> train -> quantise
-> evaluate -> C array runs end to end and produces a model the board can
load.  Delete `data/raw` and record the real thing before you report any
number from it.

    python tools/make_synthetic.py --speakers 12 --per-class 40
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

import numpy as np
import soundfile as sf

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from kws import config as C  # noqa: E402

SR = C.SAMPLE_RATE

# Each keyword is a syllable sequence; a syllable is (F1, F2, duration_ms).
# The two commands are deliberately made distinguishable but not trivially so:
# they share a similar rhythm and overlap in F1.
WORDS: dict[str, list[tuple[float, float, int]]] = {
    "help":     [(730, 1090, 190), (400, 2100, 230)],      # jiu - ming
    "evacuate": [(600, 1700, 200), (350, 2300, 220)],      # che - li
}

# Pool used for `unknown`: ordinary words, some of them close to the commands.
CONFUSABLE = [
    [(700, 1100, 180), (450, 1900, 210)],   # near "help"
    [(620, 1650, 190), (380, 2150, 200)],   # near "evacuate"
    [(500, 1500, 200), (600, 1200, 190)],
    [(800, 1300, 170), (300, 2400, 230)],
    [(450, 900, 220)],
    [(650, 1800, 160), (520, 1400, 180), (400, 2000, 160)],
]


def _resonator(x: np.ndarray, freq: float, bw: float = 90.0) -> np.ndarray:
    """Two-pole resonator, the cheapest thing that sounds vaguely vocal."""
    r = np.exp(-np.pi * bw / SR)
    theta = 2 * np.pi * freq / SR
    a1, a2 = -2 * r * np.cos(theta), r * r
    y = np.zeros_like(x)
    for n in range(len(x)):
        y[n] = x[n] - a1 * (y[n - 1] if n >= 1 else 0.0) \
                    - a2 * (y[n - 2] if n >= 2 else 0.0)
    return y


def _glottal(n: int, f0: float, rng: np.random.Generator) -> np.ndarray:
    """Pulse train with a little jitter, plus breath noise."""
    x = np.zeros(n, dtype=np.float32)
    pos = 0.0
    while pos < n:
        x[int(pos)] = 1.0
        pos += SR / (f0 * float(rng.uniform(0.97, 1.03)))
    return x + 0.02 * rng.standard_normal(n).astype(np.float32)


def syllable(f1: float, f2: float, ms: int, f0: float, scale: float,
             rng: np.random.Generator) -> np.ndarray:
    n = int(SR * ms / 1000)
    src = _glottal(n, f0, rng)
    y = _resonator(src, f1 * scale) + 0.6 * _resonator(src, f2 * scale)
    env = np.hanning(n).astype(np.float32) ** 0.4
    return (y * env).astype(np.float32)


def utterance(seq: list[tuple[float, float, int]], f0: float, scale: float,
              rng: np.random.Generator) -> np.ndarray:
    parts = []
    for f1, f2, ms in seq:
        jitter = float(rng.uniform(0.85, 1.15))
        parts.append(syllable(f1, f2, int(ms * jitter), f0, scale, rng))
        parts.append(np.zeros(int(SR * rng.uniform(0.01, 0.05)), dtype=np.float32))
    x = np.concatenate(parts)

    out = np.zeros(C.CLIP_SAMPLES, dtype=np.float32)
    start = int(rng.integers(0, max(C.CLIP_SAMPLES - len(x), 1)))
    take = min(len(x), C.CLIP_SAMPLES - start)
    out[start : start + take] = x[:take]

    peak = np.max(np.abs(out)) + 1e-9
    out = out / peak * float(rng.uniform(0.25, 0.7))
    out += 0.004 * rng.standard_normal(C.CLIP_SAMPLES).astype(np.float32)
    return np.clip(out * 32767, -32768, 32767).astype(np.int16)


def write(path: Path, pcm: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), pcm, SR, subtype="PCM_16")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--speakers", type=int, default=12)
    ap.add_argument("--per-class", type=int, default=40,
                    help="clips per keyword per... total, split across speakers")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent.parent / "data")
    ap.add_argument("--clean", action="store_true",
                    help="wipe data/raw and data/noise first")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    raw, noise_dir = args.out / "raw", args.out / "noise"
    if args.clean:
        shutil.rmtree(raw, ignore_errors=True)
        shutil.rmtree(noise_dir, ignore_errors=True)

    speakers = [f"spk{i:02d}" for i in range(args.speakers)]
    # Per-speaker voice: pitch and vocal-tract length.
    voice = {s: (float(rng.uniform(95, 230)), float(rng.uniform(0.88, 1.14)))
             for s in speakers}

    per_speaker = max(args.per_class // args.speakers, 1)
    counter = {label: 0 for label in C.LABELS}

    for speaker in speakers:
        f0, scale = voice[speaker]
        for label, seq in WORDS.items():
            for _ in range(per_speaker):
                pcm = utterance(seq, f0, scale, rng)
                write(raw / label / f"{speaker}_{counter[label]:04d}.wav", pcm)
                counter[label] += 1

        # unknown: roughly twice as many as a single keyword, as the plan asks
        for _ in range(per_speaker * 2):
            seq = CONFUSABLE[int(rng.integers(len(CONFUSABLE)))]
            pcm = utterance(seq, f0, scale, rng)
            write(raw / "unknown" / f"{speaker}_{counter['unknown']:04d}.wav", pcm)
            counter["unknown"] += 1

        for _ in range(per_speaker):
            pcm = (rng.standard_normal(C.CLIP_SAMPLES) *
                   rng.uniform(20, 180)).astype(np.int16)
            write(raw / "silence" / f"{speaker}_{counter['silence']:04d}.wav", pcm)
            counter["silence"] += 1

    # Background noise: broadband rumble plus a couple of machine tones.
    for i in range(4):
        n = SR * 5
        base = rng.standard_normal(n).astype(np.float32)
        base = np.convolve(base, np.ones(48, np.float32) / 48, mode="same")
        t = np.arange(n) / SR
        tone = sum(np.sin(2 * np.pi * f * t) for f in
                   rng.uniform(60, 400, size=3)).astype(np.float32)
        mix = base * 3000 + tone * float(rng.uniform(200, 900))
        write(noise_dir / f"machine_{i}.wav",
              np.clip(mix, -32768, 32767).astype(np.int16))

    (args.out / "SYNTHETIC").write_text(
        "This dataset was generated by tools/make_synthetic.py.\n"
        "It is a pipeline rehearsal, not speech. Any model trained on it is a\n"
        "placeholder. Delete this file once data/raw holds real recordings.\n",
        encoding="utf-8")

    print("synthetic data written to", raw)
    for label in C.LABELS:
        print(f"  {label:<10} {counter[label]:>5} clips")
    print(f"  noise      {4:>5} files")
    print("\nThis is a pipeline rehearsal, not a dataset. Replace it before "
          "reporting any metric.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
