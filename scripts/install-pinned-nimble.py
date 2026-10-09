#!/usr/bin/env python3
"""Install the source-verified RadioClock NimBLE pin into the retained toolchain."""
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile

repo = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nimble_pin_validator", repo / "scripts/validate-nimble-source.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
source = repo / "libraries/NimBLE-Arduino"
validator.validate(source)
tools = Path(os.environ.get("RADIOCLOCK_TOOLS_DIR", "/workspace/.radioclock-tools"))
parent = tools / "user/libraries"
parent.mkdir(parents=True, exist_ok=True)
destination = parent / "NimBLE-Arduino"
if destination.exists():
    try:
        validator.validate(destination)
        # The pin manifest itself must also be the reviewed repository version.
        if (destination / "RADIOCLOCK_SOURCE_MANIFEST.json").read_bytes() == (source / "RADIOCLOCK_SOURCE_MANIFEST.json").read_bytes():
            print("Pinned NimBLE source already installed; retained toolchain reused")
            raise SystemExit(0)
    except (AssertionError, FileNotFoundError, ValueError):
        pass
with tempfile.TemporaryDirectory(prefix=".radioclock-nimble-", dir=parent) as temporary:
    staged = Path(temporary) / "NimBLE-Arduino"
    shutil.copytree(source, staged)
    validator.validate(staged)
    backup = Path(temporary) / "previous-NimBLE-Arduino"
    if destination.exists():
        destination.rename(backup)
    try:
        staged.rename(destination)
    except BaseException:
        if backup.exists():
            backup.rename(destination)
        raise
    # TemporaryDirectory removes the previous dependency only after replacement
    # is verified and atomically installed. No board flash is touched.
print("Installed NimBLE-Arduino 2.5.1-radioclock.1 source")
