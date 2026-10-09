#!/usr/bin/env python3
"""Verify the vendored/installed NimBLE pin and its documented local changes."""
import argparse
import hashlib
import json
from pathlib import Path


def validate(library):
    manifest = json.loads((library / "RADIOCLOCK_SOURCE_MANIFEST.json").read_text())
    assert manifest["upstream_version"] == "2.5.1"
    assert manifest["upstream_archive_sha256"] == "8fb09b1cde59f4ca5e6172d620c379968db959a883209289a9315b03eec27fe2"
    assert manifest["upstream_timer_fix_commit"] == "e0c8f5a558893197ae60c3606ded01a2dbb88863"
    originals = manifest["upstream_files_sha256"]
    modified = manifest["modified_files_sha256"]
    assert set(modified) == {"library.properties", "src/NimBLEDevice.h", "src/NimBLEDevice.cpp",
                            "src/NimBLEClient.cpp", "src/NimBLEClient.h", "src/NimBLEScan.cpp",
                            "src/nimble/nimble/host/src/ble_hs.c",
                            "src/nimble/porting/npl/freertos/include/nimble/npl_freertos.h",
                            "src/nimble/porting/npl/freertos/src/npl_os_freertos.c"}
    assert len(originals) == 475
    inventory = {path.relative_to(library).as_posix() for path in library.rglob("*") if path.is_file()}
    expected = set(originals) | {"RADIOCLOCK_SOURCE_MANIFEST.json", "README_RADIOCLOCK.md",
                                 "RADIOCLOCK_HOST_TIMER_FIX.patch"}
    assert inventory == expected, f"Unreviewed/missing NimBLE files: {sorted(inventory ^ expected)}"
    for name, expected in originals.items():
        actual = hashlib.sha256((library / name).read_bytes()).hexdigest()
        assert actual == modified.get(name, expected), f"Pinned NimBLE source differs: {name}"
    for path in library.rglob("*"):
        assert path.suffix.lower() not in {".a", ".o", ".so", ".elf", ".bin"}, f"Binary library file: {path}"
    print(f"PASS: NimBLE-Arduino 2.5.1-radioclock.1, {len(originals)} upstream files, {len(modified)} documented source changes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path, nargs="?",
                        default=Path(__file__).resolve().parents[1] / "libraries/NimBLE-Arduino")
    validate(parser.parse_args().library)
