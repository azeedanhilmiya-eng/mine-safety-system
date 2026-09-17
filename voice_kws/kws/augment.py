"""Waveform augmentation, applied to int16 PCM *before* the frontend.

With only a few hundred real recordings the model will happily memorise the
handful of people who made them.  Augmentation is not a nice-to-have here, it
is what makes the model survive a stranger walking up to the board at the
demo.  Everything runs on the waveform so the frontend still sees realistic
input (noise reduction and PCAN get a chance to do their job).
"""

from __future__ import annotations

import numpy as np

from . import config as C

INT16_MAX = 32767


def _as_float(pcm: np.ndarray) -> np.ndarray:
    return np.asarray(pcm, dtype=np.float32)


def _to_int16(x: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(x), -INT16_MAX - 1, INT16_MAX).astype(np.int16)


def fit_length(pcm: np.ndarray, n: int = C.CLIP_SAMPLES) -> np.ndarray:
    """Pad with zeros or centre-crop to exactly n samples."""
    pcm = np.asarray(pcm)
    if pcm.shape[0] == n:
        return pcm
    if pcm.shape[0] < n:
        pad = n - pcm.shape[0]
        return np.pad(pcm, (pad // 2, pad - pad // 2))
    start = (pcm.shape[0] - n) // 2
    return pcm[start : start + n]


def time_shift(pcm: np.ndarray, rng: np.random.Generator, max_ms: int = 100) -> np.ndarray:
    """Slide the clip inside its 1 s window; zero-fill what slides in."""
    max_shift = C.SAMPLE_RATE * max_ms // 1000
    s = int(rng.integers(-max_shift, max_shift + 1))
    if s == 0:
        return pcm
    out = np.zeros_like(pcm)
    if s > 0:
        out[s:] = pcm[:-s]
    else:
        out[:s] = pcm[-s:]
    return out


def speed_perturb(pcm: np.ndarray, rate: float) -> np.ndarray:
    """Resample by `rate` (linear interpolation), then refit to 1 s."""
    if abs(rate - 1.0) < 1e-6:
        return pcm
    n = pcm.shape[0]
    idx = np.arange(0, n, rate, dtype=np.float32)
    idx = idx[idx < n - 1]
    lo = idx.astype(np.int32)
    frac = idx - lo
    x = _as_float(pcm)
    out = x[lo] * (1.0 - frac) + x[lo + 1] * frac
    return fit_length(_to_int16(out))


def gain(pcm: np.ndarray, db: float) -> np.ndarray:
    return _to_int16(_as_float(pcm) * (10.0 ** (db / 20.0)))


def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(_as_float(x) ** 2)) + 1e-6)


def mix_noise(pcm: np.ndarray, noise: np.ndarray, snr_db: float,
              rng: np.random.Generator) -> np.ndarray:
    """Mix a random slice of `noise` into `pcm` at the requested SNR."""
    n = pcm.shape[0]
    if noise.shape[0] < n:
        noise = np.tile(noise, int(np.ceil(n / max(noise.shape[0], 1))))
    start = int(rng.integers(0, noise.shape[0] - n + 1))
    seg = _as_float(noise[start : start + n])

    speech_rms, noise_rms = _rms(pcm), _rms(seg)
    target_noise_rms = speech_rms / (10.0 ** (snr_db / 20.0))
    return _to_int16(_as_float(pcm) + seg * (target_noise_rms / noise_rms))


def spec_augment(feat: np.ndarray, rng: np.random.Generator,
                 max_t: int = 6, max_f: int = 5) -> np.ndarray:
    """Mask one time block and one frequency band on the int8 feature map.

    Masked cells are set to -128, the frontend's "no energy" value, so the
    model sees something it could plausibly see in the field.
    """
    out = feat.copy()
    t = int(rng.integers(1, max_t + 1))
    t0 = int(rng.integers(0, max(out.shape[0] - t, 1)))
    out[t0 : t0 + t, :, :] = -128

    f = int(rng.integers(1, max_f + 1))
    f0 = int(rng.integers(0, max(out.shape[1] - f, 1)))
    out[:, f0 : f0 + f, :] = -128
    return out


SNR_CHOICES = (0.0, 5.0, 10.0, 20.0)
SPEED_CHOICES = (0.9, 0.95, 1.0, 1.05, 1.1)
GAIN_DB_RANGE = (-6.0, 6.0)


def augment_waveform(pcm: np.ndarray, noises: list[np.ndarray],
                     rng: np.random.Generator) -> np.ndarray:
    """One randomly augmented variant of a clip, still int16 and 1 s long."""
    out = speed_perturb(fit_length(pcm), float(rng.choice(SPEED_CHOICES)))
    out = time_shift(out, rng)
    out = gain(out, float(rng.uniform(*GAIN_DB_RANGE)))
    if noises and rng.random() < 0.8:
        out = mix_noise(out, noises[int(rng.integers(len(noises)))],
                        float(rng.choice(SNR_CHOICES)), rng)
    return out
