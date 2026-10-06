#!/usr/bin/env python3
"""Validate the shipped asset, browser syntax, routes, versions and partitions."""
from pathlib import Path
from html.parser import HTMLParser
import gzip
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sketch = root / "firmware/RadioClock_V4_1_Casio_BLE_Reliability"
source = (sketch / f"{sketch.name}.ino").read_text()
html = (root / "ui/radioclock.html").read_text()
assert f"// {sketch.name}.ino" in source
assert '#define FIRMWARE_VERSION "V4.1"' in source
assert "V4.1" in html and "V3.2.2" not in html
assert 'id="btTime"' in html and 'id="btTimezone"' in html and 'id="btTimeOffset"' in html
for required in ('bt_timezone', 'bt_time_offset_minutes', 'bluetoothLocalTime'):
    assert required in source, f"Bluetooth time setting missing: {required}"
assert 'String btTimezoneName = DEFAULT_BT_TIMEZONE' in source
assert 'transmission_offset_minutes' in source and 'stationTime(' in source
asset = re.search(r"INDEX_HTML_GZ\[\] PROGMEM = \{(.*?)\n\};", source, re.S)
assert asset
compressed = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", asset[1]))
assert gzip.decompress(compressed) == html.encode(), "Embedded UI is stale"

class Elements(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []
    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key == "id":
                self.ids.append(value)

elements = Elements()
elements.feed(html)
assert len(elements.ids) == len(set(elements.ids)), "Duplicate DOM IDs"
refs = set(re.findall(r"\$\(['\"]([^'\"]+)['\"]\)", html))
assert refs <= set(elements.ids), f"Missing DOM IDs: {refs-set(elements.ids)}"
routes = set(re.findall(r'server\.on\("(/api/[^"?]+)"', source))
ui_routes = set(re.findall(r"['\"](/api/[^'\"?]+)['\"]", html))
assert ui_routes <= routes, f"Missing routes: {ui_routes-routes}"
for script in re.findall(r"<script[^>]*>(.*?)</script>", html, re.S):
    with tempfile.NamedTemporaryFile(suffix=".js") as js:
        js.write(script.encode()); js.flush()
        subprocess.run(["node", "--check", js.name], check=True)

parts = []
for line in (sketch / "partitions.csv").read_text().splitlines():
    if line.strip() and not line.startswith("#"):
        name, kind, subtype, offset, size, *rest = [x.strip() for x in line.split(",")]
        parts.append((name, kind, int(offset, 0), int(size, 0)))
for previous, current in zip(parts, parts[1:]):
    assert previous[2]+previous[3] <= current[2], "Partitions overlap"
assert max(offset+size for _,_,offset,size in parts) <= 4*1024*1024
assert any(kind == "app" and size == 3*1024*1024 for _,kind,_,size in parts)
print(f"PASS: gzip round-trip ({len(compressed):,} bytes), JavaScript syntax, "
      f"{len(elements.ids)} DOM IDs, {len(ui_routes)} UI routes, V4.1 versions, 4 MB partitions")
