#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tools_dir="${RADIOCLOCK_TOOLS_DIR:-/workspace/.radioclock-tools}"
export ARDUINO_DIRECTORIES_DATA="$tools_dir/data"
export ARDUINO_DIRECTORIES_DOWNLOADS="$tools_dir/downloads"
export ARDUINO_DIRECTORIES_USER="$tools_dir/user"
export ARDUINO_BUILD_CACHE_PATH="$tools_dir/cache"
if [[ -z "${ARDUINO_NETWORK_PROXY:-}" && -n "${HTTPS_PROXY:-${https_proxy:-}}" ]]; then
  export ARDUINO_NETWORK_PROXY="${HTTPS_PROXY:-${https_proxy:-}}"
fi
cli="$tools_dir/bin/arduino-cli"
if [[ ! -x "$cli" ]]; then
  printf '%s\n' 'Run scripts/install-toolchain.sh before compiling.' >&2
  exit 1
fi

# The generic ESP32 Dev Module is compatible with the classic Node32 target.
# The sketch-local partitions.csv selects the 3 MB app / 4 MB flash layout.
fqbn="${RADIOCLOCK_FQBN:-esp32:esp32:esp32:FlashSize=4M,PartitionScheme=huge_app}"
jobs="${RADIOCLOCK_BUILD_JOBS:-2}"
sketch="$repo_dir/firmware/RadioClock_V4_7"
build_dir="$tools_dir/build/RadioClock_V4_7"
output_dir="$tools_dir/output/RadioClock_V4_7"
mkdir -p "$build_dir" "$output_dir"

exec "$cli" compile --fqbn "$fqbn" --jobs "$jobs" \
  --build-path "$build_dir" --output-dir "$output_dir" "$sketch" "$@"
