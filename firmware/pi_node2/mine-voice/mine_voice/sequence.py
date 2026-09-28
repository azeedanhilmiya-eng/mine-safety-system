from __future__ import annotations

import os
from pathlib import Path


class SequenceStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def next(self) -> int:
        current = 0
        try:
            current = int(self.path.read_text(encoding="ascii").strip())
        except (FileNotFoundError, ValueError):
            pass
        value = 1 if current >= 2_147_483_647 else current + 1
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(str(value), encoding="ascii")
        os.replace(temporary, self.path)
        return value
