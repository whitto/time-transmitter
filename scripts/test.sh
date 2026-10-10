#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
python3 scripts/validate-project.py
node scripts/test-ui.mjs
python3 tests/test_ble_lifecycle.py
python3 tests/test_nimble_pin.py
python3 tests/test_ble_scan_control.py
python3 tests/test_clock_and_encoders.py
python3 tests/test_wifi_ntp_recovery.py
python3 tests/test_network_reliability.py
python3 tests/test_wifi_access_schedule.py
python3 tests/test_bluetooth_workflow.py
python3 tests/test_bt_history.py
python3 tests/test_bt_recent_syncs.py
python3 tests/test_recent_sync_ui.py
python3 tests/test_crash_dump_gate.py
python3 tests/test_crash_dump_config.py
python3 tests/test_schedule_persistence.py
python3 tests/test_storage_reliability.py
python3 tests/test_activity_led.py
python3 tests/test_json_writer.py
python3 tests/test_api_reliability.py
python3 tests/test_http_bounds.py
python3 tests/test_watch_options.py
python3 tests/test_watch_battery.py
python3 tests/test_rf_handoff.py
python3 tests/test_radio_reliability.py
python3 tests/test_timing_diagnostics.py
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread tests/test_radio_arbiter.cpp -o "$test_dir/radio-arbiter"
"$test_dir/radio-arbiter"
g++ -std=c++17 -O2 -Wall -Wextra -Werror tests/test_bx_protocol.cpp -o "$test_dir/bx-protocol"
"$test_dir/bx-protocol"
