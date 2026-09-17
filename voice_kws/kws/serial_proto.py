"""Framing for the board's serial dumps.

The node shares one UART between human-readable logs and binary dumps, so the
dumps are framed and everything else is passed through as log text:

    #WAV <sample_rate> <num_samples>\\n   then num_samples*2 bytes LE int16, then #END\\n
    #FEAT <num_elements>\\n               then num_elements bytes int8,      then #END\\n

The reader works on any object with `read(n)`, which is what lets the parser be
tested without a board attached.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

WAV_MARKER = b"#WAV"
FEAT_MARKER = b"#FEAT"
END_MARKER = b"#END"


class FrameError(RuntimeError):
    pass


@dataclass
class Frame:
    kind: str                 # "wav" | "feat"
    payload: np.ndarray       # int16 for wav, int8 for feat
    sample_rate: int | None = None


def _read_exact(stream, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise FrameError(f"stream ended after {len(buf)} of {n} bytes")
        buf += chunk
    return bytes(buf)


def _read_line(stream) -> bytes:
    line = bytearray()
    while True:
        c = stream.read(1)
        if not c:
            raise FrameError("stream ended mid-line")
        if c == b"\n":
            return bytes(line).rstrip(b"\r")
        line += c


def read_frame(stream, on_log=None) -> Frame:
    """Consume log lines until a frame arrives, then return it.

    `on_log` receives each non-frame line, so the caller can keep showing the
    board's ordinary output while waiting.
    """
    while True:
        line = _read_line(stream)
        parts = line.split()
        if not parts:
            continue

        if parts[0] == WAV_MARKER:
            if len(parts) != 3:
                raise FrameError(f"malformed WAV header: {line!r}")
            rate, count = int(parts[1]), int(parts[2])
            raw = _read_exact(stream, count * 2)
            _expect_end(stream)
            return Frame("wav", np.frombuffer(raw, dtype="<i2").copy(), rate)

        if parts[0] == FEAT_MARKER:
            if len(parts) != 2:
                raise FrameError(f"malformed FEAT header: {line!r}")
            count = int(parts[1])
            raw = _read_exact(stream, count)
            _expect_end(stream)
            return Frame("feat", np.frombuffer(raw, dtype=np.int8).copy())

        if on_log is not None:
            on_log(line.decode("utf-8", "replace"))


def _expect_end(stream) -> None:
    line = _read_line(stream)
    if not line.startswith(END_MARKER):
        raise FrameError(f"expected {END_MARKER.decode()}, got {line!r}")


def encode_wav_frame(pcm: np.ndarray, sample_rate: int) -> bytes:
    """Inverse of read_frame, used by the tests and by a fake board."""
    pcm = np.asarray(pcm, dtype="<i2")
    return (f"{WAV_MARKER.decode()} {sample_rate} {pcm.size}\n".encode()
            + pcm.tobytes() + b"#END\n")


def encode_feat_frame(feat: np.ndarray) -> bytes:
    feat = np.asarray(feat, dtype=np.int8).reshape(-1)
    return (f"{FEAT_MARKER.decode()} {feat.size}\n".encode()
            + feat.tobytes() + b"#END\n")
