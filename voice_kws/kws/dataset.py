"""Dataset construction: WAV files -> cached int8 feature arrays.

Layout expected under `data/raw/`:

    data/raw/<label>/<speaker>_<index>.wav      e.g. data/raw/help/lin_003.wav

The speaker id is everything before the FIRST underscore.  It is the unit the
train/val/test split works on: no speaker ever appears on two sides of the
split, because a model that has heard you before will recognise you and tell
you nothing about how it treats a stranger.

Background noise for augmentation goes in `data/noise/` as WAV files of any
length (mine machinery, fans, room tone, crowd chatter).
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf

from . import augment, config as C
from .frontend import features

DATA_DIR = Path(__file__).resolve().parent.parent / "data"
RAW_DIR = DATA_DIR / "raw"
NOISE_DIR = DATA_DIR / "noise"
CACHE_DIR = DATA_DIR / "cache"


@dataclass(frozen=True)
class Clip:
    path: Path
    label: int
    speaker: str


def read_wav(path: Path) -> np.ndarray:
    """Read a WAV as mono int16 at SAMPLE_RATE, raising on a rate mismatch."""
    data, sr = sf.read(str(path), dtype="int16", always_2d=True)
    if sr != C.SAMPLE_RATE:
        raise ValueError(
            f"{path} is {sr} Hz, expected {C.SAMPLE_RATE} Hz. "
            "Record at 16 kHz -- resampling here would hide the mismatch that "
            "will also exist on the device."
        )
    return data[:, 0]


def speaker_of(path: Path) -> str:
    stem = path.stem
    return stem.split("_", 1)[0] if "_" in stem else stem


def scan(raw_dir: Path = RAW_DIR) -> list[Clip]:
    clips: list[Clip] = []
    for idx, label in enumerate(C.LABELS):
        d = raw_dir / label
        if not d.is_dir():
            continue
        for wav in sorted(d.glob("*.wav")):
            clips.append(Clip(wav, idx, speaker_of(wav)))
    return clips


def load_noises(noise_dir: Path = NOISE_DIR) -> list[np.ndarray]:
    return [read_wav(p) for p in sorted(noise_dir.glob("*.wav"))]


# ------------------------------------------------------------- splitting ----

def _speaker_rank(speaker: str) -> str:
    """Stable per-speaker hash, used only to order speakers deterministically."""
    return hashlib.sha1(speaker.encode("utf-8")).hexdigest()


def split_speakers(clips: list[Clip], val_pct: int = 15, test_pct: int = 20,
                   overrides: dict[str, str] | None = None) -> dict[str, str]:
    """Assign every speaker to train / val / test, by quota.

    Quota rather than "hash mod 100 < 20": with a dozen speakers a plain hash
    split lands empty buckets about as often as not, and an empty validation
    set silently turns early stopping into "train until the epochs run out".
    Speakers are ordered by a stable hash, then handed out test-first so the
    held-out set is never the one that goes short.

    `overrides` pins named speakers to a split -- use it for the people you
    recorded specifically to be strangers to the model.
    """
    overrides = overrides or {}
    speakers = sorted({c.speaker for c in clips})
    assignment = {s: overrides[s] for s in speakers if s in overrides}

    free = sorted((s for s in speakers if s not in assignment), key=_speaker_rank)
    if not free:
        return assignment

    have = {"train": 0, "val": 0, "test": 0}
    for split in assignment.values():
        have[split] = have.get(split, 0) + 1

    n_total = len(speakers)
    want_test = max(round(n_total * test_pct / 100), 0 if have["test"] else 1)
    want_val = max(round(n_total * val_pct / 100), 0 if have["val"] else 1)
    n_test = max(want_test - have["test"], 0)
    n_val = max(want_val - have["val"], 0)

    # Never starve training: with very few speakers, held-out sets shrink first.
    while n_test + n_val >= len(free) and (n_test + n_val) > 0:
        if n_test >= n_val and n_test > 0:
            n_test -= 1
        elif n_val > 0:
            n_val -= 1

    for i, speaker in enumerate(free):
        if i < n_test:
            assignment[speaker] = "test"
        elif i < n_test + n_val:
            assignment[speaker] = "val"
        else:
            assignment[speaker] = "train"
    return assignment


# ------------------------------------------------------------- features ----

def build_features(clips: list[Clip], noises: list[np.ndarray], *,
                   variants: int, seed: int, spec_aug: bool) -> tuple[np.ndarray, np.ndarray, list[str]]:
    """Compute int8 features for every clip plus `variants` augmented copies.

    Returns (X [N,49,40,1] int8, y [N] int32, speakers [N]).
    `variants=0` gives the clean clip only -- that is what val and test use.
    """
    rng = np.random.default_rng(seed)
    xs, ys, sp = [], [], []
    for clip in clips:
        pcm = augment.fit_length(read_wav(clip.path))
        xs.append(features(pcm))
        ys.append(clip.label)
        sp.append(clip.speaker)
        for _ in range(variants):
            f = features(augment.augment_waveform(pcm, noises, rng))
            if spec_aug and rng.random() < 0.5:
                f = augment.spec_augment(f, rng)
            xs.append(f)
            ys.append(clip.label)
            sp.append(clip.speaker)
    if not xs:
        return (np.zeros((0, *C.FEATURE_SHAPE), np.int8),
                np.zeros((0,), np.int32), [])
    return np.stack(xs).astype(np.int8), np.asarray(ys, np.int32), sp


def build_all(*, variants: int = 8, seed: int = C.TRAIN["seed"],
              raw_dir: Path = RAW_DIR, noise_dir: Path = NOISE_DIR,
              overrides: dict[str, str] | None = None,
              cache: Path | None = None) -> dict:
    """Build train/val/test feature sets, optionally caching to an .npz."""
    if cache and cache.exists():
        z = np.load(cache, allow_pickle=True)
        return {k: (z[f"{k}_x"], z[f"{k}_y"]) for k in ("train", "val", "test")} | {
            "assignment": json.loads(str(z["assignment"]))
        }

    clips = scan(raw_dir)
    if not clips:
        raise FileNotFoundError(
            f"no WAV files under {raw_dir}. Record some with "
            "tools/serial_record.py, or run tools/make_synthetic.py to "
            "exercise the pipeline before real data exists."
        )
    noises = load_noises(noise_dir)
    assignment = split_speakers(clips, overrides=overrides)

    out: dict = {"assignment": assignment}
    # Python's str hash is salted per process, so deriving the augmentation
    # seed from hash(split) would make every run different while still
    # claiming to be seeded. The split's position is stable everywhere.
    for offset, split in enumerate(("train", "val", "test")):
        subset = [c for c in clips if assignment[c.speaker] == split]
        # Only the training set is augmented; val/test stay clean so the
        # reported numbers describe real recordings, not our noise generator.
        x, y, _ = build_features(
            subset, noises,
            variants=variants if split == "train" else 0,
            seed=seed + offset,
            spec_aug=(split == "train"),
        )
        out[split] = (x, y)

    if cache:
        cache.parent.mkdir(parents=True, exist_ok=True)
        np.savez_compressed(
            cache,
            assignment=json.dumps(assignment),
            **{f"{k}_{n}": v for k in ("train", "val", "test")
               for n, v in zip(("x", "y"), out[k])},
        )
    return out


def class_weights(y: np.ndarray) -> dict[int, float]:
    """Balance the loss -- `unknown` will outnumber the keywords several to one."""
    counts = np.bincount(y, minlength=C.NUM_CLASSES).astype(np.float64)
    counts[counts == 0] = 1.0
    w = counts.sum() / (C.NUM_CLASSES * counts)
    return {i: float(w[i]) for i in range(C.NUM_CLASSES)}


def summarise(clips: list[Clip], assignment: dict[str, str]) -> str:
    lines = ["label       train   val  test   total"]
    for idx, label in enumerate(C.LABELS):
        per = {"train": 0, "val": 0, "test": 0}
        for c in clips:
            if c.label == idx:
                per[assignment[c.speaker]] += 1
        lines.append(f"{label:<10}{per['train']:>7}{per['val']:>6}{per['test']:>6}"
                     f"{sum(per.values()):>8}")
    speakers = {s: assignment[s] for s in sorted({c.speaker for c in clips})}
    lines.append("")
    mark = {"train": "tr", "val": "va", "test": "TE"}
    lines.append(f"{len(speakers)} speakers: " +
                 ", ".join(f"{s}[{mark[v]}]" for s, v in speakers.items()))
    return "\n".join(lines)
