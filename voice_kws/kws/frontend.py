"""Audio frontend: int16 PCM -> 49x40 int8 feature map.

This wraps TensorFlow's `audio_microfrontend` op, which is the *same C source*
that tflite-micro compiles into the firmware.  Using it on both sides removes
the whole class of "my Python MFCC is not your C MFCC" failures.

Two things must hold on the device for the parity to be real:

1. The firmware calls `FrontendReset()` before every push-to-talk capture.  The
   frontend carries noise-estimation and PCAN gain state; the TF op always
   starts from a clean state, so the device must too.
2. The firmware feeds exactly CLIP_SAMPLES samples, in order, from that clean
   state -- no warm-up frames, no leftovers from the previous capture.

`tools/device_parity.py` checks that this actually holds on your board.
"""

from __future__ import annotations

import numpy as np
import tensorflow as tf
from tensorflow.lite.experimental.microfrontend.python.ops import (
    audio_microfrontend_op as _fe,
)

from . import config as C


def raw_frontend(pcm_int16: np.ndarray) -> np.ndarray:
    """Run the micro frontend, returning the raw uint16 output [frames, 40]."""
    pcm = np.asarray(pcm_int16, dtype=np.int16)
    if pcm.ndim != 1:
        raise ValueError(f"expected mono 1-D PCM, got shape {pcm.shape}")
    out = _fe.audio_microfrontend(
        tf.convert_to_tensor(pcm, tf.int16),
        out_scale=1,
        out_type=tf.uint16,
        **C.FRONTEND,
    )
    return out.numpy().astype(np.int32)


def quantize(raw_uint16: np.ndarray) -> np.ndarray:
    """uint16 frontend output -> int8, byte-identical to micro_speech's C code.

        value = ((v * 256) + 333) / 666, clamped to 0..255, then biased to int8.

    Integer arithmetic throughout, so there is no float rounding to disagree
    about between x86 and Xtensa.
    """
    v = np.asarray(raw_uint16, dtype=np.int64)
    v = (v * C.VALUE_SCALE + C.VALUE_DIV // 2) // C.VALUE_DIV
    v = np.clip(v, 0, 255)
    return (v - 128).astype(np.int8)


def features(pcm_int16: np.ndarray) -> np.ndarray:
    """int16 PCM (any length >= one window) -> int8 features [49, 40, 1].

    Clips shorter than 1 s are zero-padded on the right, longer clips are
    truncated, so the caller never has to think about off-by-one frames.
    """
    pcm = np.asarray(pcm_int16, dtype=np.int16)
    if pcm.shape[0] < C.CLIP_SAMPLES:
        pcm = np.pad(pcm, (0, C.CLIP_SAMPLES - pcm.shape[0]))
    elif pcm.shape[0] > C.CLIP_SAMPLES:
        pcm = pcm[: C.CLIP_SAMPLES]

    q = quantize(raw_frontend(pcm))

    if q.shape[0] < C.NUM_FRAMES:                       # defensive, should not fire
        q = np.pad(q, ((0, C.NUM_FRAMES - q.shape[0]), (0, 0)), constant_values=-128)
    q = q[: C.NUM_FRAMES]
    return q.reshape(C.FEATURE_SHAPE)


def features_batch(pcm_batch) -> np.ndarray:
    """Convenience wrapper: list of clips -> [N, 49, 40, 1] int8."""
    return np.stack([features(p) for p in pcm_batch]).astype(np.int8)
