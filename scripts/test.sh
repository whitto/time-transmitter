#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
python3 scripts/validate-project.py
node scripts/test-ui.mjs
python3 tests/test_ble_lifecycle.py
python3 tests/test_bluetooth_workflow.py
python3 tests/test_rf_handoff.py
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread tests/test_radio_arbiter.cpp -o "$test_dir/radio-arbiter"
"$test_dir/radio-arbiter"
g++ -std=c++17 -O2 -Wall -Wextra -Werror tests/test_bx_protocol.cpp -o "$test_dir/bx-protocol"
"$test_dir/bx-protocol"
