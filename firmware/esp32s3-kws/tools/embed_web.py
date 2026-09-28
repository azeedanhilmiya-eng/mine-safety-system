Import("env")

import gzip
from pathlib import Path

project = Path(env.subst("$PROJECT_DIR"))
web_dir = project / "web"
output = project / "src" / "web_assets.h"

assets = [
    ("INDEX_HTML_GZ", web_dir / "index.html"),
    ("APP_JS_GZ", web_dir / "app.js"),
    ("FAVICON_JPEG_GZ", web_dir / "favicon.jpeg"),
]


def byte_array(data):
    rows = []
    for start in range(0, len(data), 16):
        rows.append("  " + ", ".join(f"0x{value:02x}" for value in data[start:start + 16]))
    return ",\n".join(rows)


parts = ["#pragma once", "", "#include <Arduino.h>", ""]
for name, source in assets:
    if not source.is_file():
        raise FileNotFoundError(source)
    compressed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
    parts.extend([
        f"static const uint8_t {name}[] PROGMEM = {{",
        byte_array(compressed),
        "};",
        f"static const size_t {name}_LEN = sizeof({name});",
        "",
    ])

content = "\n".join(parts)
if not output.exists() or output.read_text(encoding="utf-8") != content:
    output.write_text(content, encoding="utf-8")
    print(f"[web] Embedded web assets -> {output}")

