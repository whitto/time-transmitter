#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tools_dir="${RADIOCLOCK_TOOLS_DIR:-/workspace/.radioclock-tools}"
library_src="$repo_dir/libraries/RadioCrashDumpGate"
library_dest="$tools_dir/user/libraries/RadioCrashDumpGate"
mkdir -p "$library_dest/src/esp32"
cp "$library_src/library.properties" "$library_dest/library.properties"
cp "$library_src/README.md" "$library_dest/README.md"
cp "$library_src/src/RadioCrashDumpGate.h" "$library_dest/src/RadioCrashDumpGate.h"
cp "$library_src/src/RadioCrashDumpGate.cpp" "$library_dest/src/RadioCrashDumpGate.cpp"
cp "$library_src/src/esp32/README.md" "$library_dest/src/esp32/README.md"
python3 "$repo_dir/scripts/install-pinned-nimble.py"
