#!/usr/bin/env python3
"""Regenerate the deterministic gzip PROGMEM asset from the editable UI."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware/RadioClock_V4_1_Casio_BLE_Reliability/RadioClock_V4_1_Casio_BLE_Reliability.ino"
html = (ROOT / "ui/radioclock.html").read_bytes()
compressed = gzip.compress(html, compresslevel=9, mtime=0)
rows = ["  " + ", ".join(f"0x{x:02x}" for x in compressed[i:i+20]) + ","
        for i in range(0, len(compressed), 20)]
asset = "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n" + "\n".join(rows) + "\n};"
source, count = re.subn(r"static const uint8_t INDEX_HTML_GZ\[\] PROGMEM = \{.*?\n\};",
                      lambda _: asset, SKETCH.read_text(), flags=re.S)
if count != 1:
    raise SystemExit("Expected exactly one embedded UI asset")
if source != SKETCH.read_text():
    SKETCH.write_text(source)
assert gzip.decompress(compressed) == html
print(f"V4.1 UI: {len(html):,} bytes -> {len(compressed):,} bytes gzip")
