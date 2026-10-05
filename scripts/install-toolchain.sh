#!/usr/bin/env bash
set -euo pipefail

# Keep all packages, tools, downloads and sketches inside the cloud workspace.
tools_dir="${RADIOCLOCK_TOOLS_DIR:-/workspace/.radioclock-tools}"
export ARDUINO_DIRECTORIES_DATA="$tools_dir/data"
export ARDUINO_DIRECTORIES_DOWNLOADS="$tools_dir/downloads"
export ARDUINO_DIRECTORIES_USER="$tools_dir/user"
export ARDUINO_BUILD_CACHE_PATH="$tools_dir/cache"
if [[ -z "${ARDUINO_NETWORK_PROXY:-}" && -n "${HTTPS_PROXY:-${https_proxy:-}}" ]]; then
  export ARDUINO_NETWORK_PROXY="${HTTPS_PROXY:-${https_proxy:-}}"
fi

cli_version=1.3.1
cli="$tools_dir/bin/arduino-cli"
esp32_index=https://espressif.github.io/arduino-esp32/package_esp32_index.json
mkdir -p "$tools_dir/bin" "$ARDUINO_DIRECTORIES_DATA" "$ARDUINO_DIRECTORIES_DOWNLOADS" "$ARDUINO_DIRECTORIES_USER"

if [[ ! -x "$cli" ]] || ! "$cli" version | grep -q "Version: $cli_version "; then
  case "$(uname -s):$(uname -m)" in
    Linux:x86_64) cli_archive="arduino-cli_${cli_version}_Linux_64bit.tar.gz" ;;
    *) printf '%s\n' 'This cloud installer supports Linux x86_64.' >&2; exit 1 ;;
  esac
  release_url="https://github.com/arduino/arduino-cli/releases/download/v${cli_version}"
  staging_dir="$(mktemp -d "$tools_dir/cli-install.XXXXXX")"
  trap 'rm -rf "$staging_dir"' EXIT
  curl --fail --location --silent --show-error --retry 2 --max-time 180 \
    "$release_url/$cli_archive" --output "$staging_dir/$cli_archive"
  curl --fail --location --silent --show-error --retry 2 --max-time 60 \
    "$release_url/${cli_version}-checksums.txt" --output "$staging_dir/checksums.txt"
  # The release publisher's checksum is checked before the executable is used.
  (
    cd "$staging_dir"
    awk -v archive="$cli_archive" '$2 == archive { print; found=1 } END { if (!found) exit 1 }' checksums.txt \
      | sha256sum --check --strict
    tar -xzf "$cli_archive" arduino-cli
  )
  install -m 0755 "$staging_dir/arduino-cli" "$cli"
fi

# Arduino CLI verifies the Arduino index signatures and package checksums.
# Retain default signature, checksum and TLS validation.
"$cli" core update-index --additional-urls "$esp32_index"
"$cli" core install esp32:esp32@3.3.11 --additional-urls "$esp32_index"
"$cli" lib install 'NimBLE-Arduino@2.5.1' 'ArduinoJson@6.21.5'
"$cli" version
"$cli" core list
"$cli" lib list
