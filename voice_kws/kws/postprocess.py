"""Python mirror of firmware/voice_node/kws_postprocess.c.

Both files implement one decision rule.  Keeping a copy on each side is a
duplication, but the alternative -- describing the rule in prose and hoping
two people implement it the same way -- is how a board ends up behaving
differently from the report that describes it.  `tools/export_test_vectors.py`
plus `firmware/voice_node/test/test_postprocess.c` pin the two together.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import config as C

OUTPUT_SCALE = 1.0 / 256.0
OUTPUT_ZERO_POINT = -128


@dataclass
class Result:
    label: int          # winning class, or -1 when the windows disagreed
    confidence: float
    accepted: bool      # True = raise a LoRa event
    votes: int


def quantize_feature(raw: int) -> int:
    v = (int(raw) * C.VALUE_SCALE + C.VALUE_DIV // 2) // C.VALUE_DIV
    return int(np.clip(v, 0, 255)) - 128


def dequantize_prob(q: int, scale: float = OUTPUT_SCALE,
                    zero_point: int = OUTPUT_ZERO_POINT) -> float:
    return (float(q) - zero_point) * scale


def vote(window_out: np.ndarray, threshold: float,
         scale: float = OUTPUT_SCALE,
         zero_point: int = OUTPUT_ZERO_POINT) -> Result:
    """Majority vote across overlapping windows.

    `window_out` is [num_windows, num_classes] of raw int8 output values.
    """
    w = np.asarray(window_out, dtype=np.int8)
    picks = [int(np.argmax(row)) for row in w]          # ties -> lower index

    counts = np.bincount(picks, minlength=C.NUM_CLASSES)
    best_votes = int(counts.max())
    best_label = int(counts.argmax())                   # ties -> lower index

    if best_votes < 2:
        return Result(label=-1, confidence=0.0, accepted=False, votes=best_votes)

    conf = max(dequantize_prob(int(w[i, best_label]), scale, zero_point)
               for i, p in enumerate(picks) if p == best_label)

    accepted = best_label not in C.NON_COMMAND_IDS and conf >= threshold
    return Result(label=best_label, confidence=conf, accepted=accepted,
                  votes=best_votes)
