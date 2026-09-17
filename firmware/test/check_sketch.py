#!/usr/bin/env python3
"""Type-check an .ino on a PC, the way the Arduino IDE would build it.

The IDE does two things a plain C++ compiler does not: it prepends Arduino.h,
and it generates a prototype for every function so definition order does not
matter. This reproduces both against the stubs in stubs/, then hands the result
to g++ with -fsyntax-only.

It cannot link, run, or check anything about the real hardware libraries. What
it does catch is the whole class of mistakes that otherwise only show up when
someone finally opens the IDE: typos, wrong argument counts, wrong types, a
method that does not exist on the object it is called on.

    python3 check_sketch.py ../surface_node.ino
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Top-level definitions: "Type name(args) {" starting at column 0.
SIGNATURE = re.compile(
    r"^((?:static\s+|inline\s+)*[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{)]*)\)\s*\{",
    re.MULTILINE)

SKIP = {"setup", "loop", "if", "for", "while", "switch", "else", "do", "return"}


def prototypes(source: str) -> list[str]:
    out = []
    for ret, name, args in SIGNATURE.findall(source):
        if name in SKIP or ret.strip().endswith("="):
            continue
        if ret.strip() in {"struct", "class", "enum", "typedef", "namespace"}:
            continue
        out.append(f"{ret.strip()} {name}({args.strip()});")
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    sketch = Path(sys.argv[1]).resolve()
    source = sketch.read_text(encoding="utf-8")

    # Prototypes go after the includes and type definitions, which is where the
    # IDE puts them: right before the first function body.
    first = SIGNATURE.search(source)
    cut = first.start() if first else 0
    generated = (source[:cut]
                 + "\n// --- generated prototypes (mimics the Arduino IDE) ---\n"
                 + "\n".join(prototypes(source))
                 + "\n\n"
                 + source[cut:])

    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / (sketch.stem + ".cpp")
        cpp.write_text(generated, encoding="utf-8")

        cmd = [
            "g++", "-fsyntax-only", "-std=gnu++17",
            f"-I{HERE / 'stubs'}",
            f"-I{sketch.parent}",
            "-include", "Arduino.h",
            "-include", "stubs_misc.h",
            "-include", "stubs_esp.h",
            "-Wall", "-Wno-unused-parameter", "-Wno-unused-variable",
            str(cpp),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode == 0:
        print(f"{sketch.name}: syntax and types OK"
              + (f"\n{result.stderr}" if result.stderr.strip() else ""))
        return 0

    print(f"{sketch.name}: FAILED\n")
    print(result.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
