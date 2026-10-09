#!/usr/bin/env python3
"""Exercise real schedule HTTP/flash code with pinned ArduinoJson and RF mocks."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from . import test_bluetooth_workflow as workflow
except ImportError:
    import test_bluetooth_workflow as workflow

DRIVER, FIRMWARE, MOCKS = workflow.DRIVER, workflow.FIRMWARE, workflow.MOCKS
extract_function, extract_route = workflow.extract_function, workflow.extract_route

SCHEDULE_DRIVER = r'''
int main() {
  setenv("TZ", "UTC0", 1); tzset();
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  btAlwaysWaitEnabled = false;
  bind(3, BT_PROTOCOL_ANALOGUE);
  btSyncTimes[2] = 1318; btSyncProfile[2] = 3; btSyncProtocol[2] = BT_PROTOCOL_ANALOGUE;
  registerRoutes(); initBluetoothSync();
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 200 && btSyncEnabled[2]);
  // A successful toggle response is already durable, without waiting for
  // the normal background config debounce.
  btSyncEnabled[2] = false; loadConfig(); assert(btSyncEnabled[2]);
  const String rfJson = "[{\"station\":0,\"start\":713,\"end\":718}]";
  configDirty = false;
  server.post("/api/schedules", {{"plain", rfJson}});
  assert(server.code == 200 && btSyncEnabled[2] && !rfPaused && configDirty);
  assert(schedule_count == 1 && schedules[0].start_min == 713 && schedules[0].end_min == 718);
  assert(bluetoothTimeSlotConflicts(1318));
  assert(flash.at(STATION_CONFIG_FILE) == rfJson.s && !flash.count(SCHEDULE_TEMP_FILE));
  writeConfigNow(); btSyncEnabled[2] = false; loadConfig(); assert(btSyncEnabled[2]);

  // Earlier delivery today is informational: every enabled daily time must
  // still listen, including another slot for the same watch later that day.
  struct tm today; bluetoothLocalTime(fakeEpoch, today);
  btProfileSuccessYear[3] = today.tm_year + 1900; btProfileSuccessYday[3] = today.tm_yday;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 200 && !btSyncEnabled[2]);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 200 && btSyncEnabled[2]);
  btSyncTimes[0] = 1340; btSyncProfile[0] = 3; btSyncProtocol[0] = BT_PROTOCOL_ANALOGUE;
  server.post("/api/config", {{"bt_slot", "0"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 200 && btSyncEnabled[0]);
  assert(shouldWifiBeOnForSchedule());
  serviceBluetoothSync(); assert(btWindowActive && btSyncActiveSlot == 2 && btProfileDoneToday(3));
  discover("MTG-B1000"); serviceBluetoothSync();
  assert(!btWindowActive && btSyncEnabled[2] && btSyncEnabled[0]);
  serviceBluetoothSync(); assert(!btWindowActive); // Same occurrence is not reopened after success.
  fakeEpoch += 20 * 60;
  assert(shouldWifiBeOnForSchedule());
  serviceBluetoothSync(); assert(btWindowActive && btSyncActiveSlot == 0 && btProfileDoneToday(3));
  discover("MTG-B1000"); serviceBluetoothSync();
  assert(!btWindowActive && btSyncEnabled[2] && btSyncEnabled[0]);
  server.post("/api/schedules", {{"plain", rfJson}});
  assert(server.code == 200 && btSyncEnabled[2]);
  fakeEpoch += 86400 - 20 * 60; serviceBluetoothSync();
  assert(btWindowActive && btSyncActiveSlot == 2 && !btProfileDoneToday(3));

  // Failed flash commits never acknowledge a changed setting or cancel the
  // current scan. Successful edits are all durable before their HTTP 200.
  const auto listeningEnd = btWindowEndMillis;
  const auto savedConfig = flash.at(CONFIG_FILE);
  mockRenameOk = false;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_time", "1319"}});
  assert(server.code == 500 && btSyncTimes[2] == 1318 && btWindowActive && btWindowEndMillis == listeningEnd);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_profile", "1"}});
  assert(server.code == 500 && btSyncProfile[2] == 3 && btWindowActive);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_protocol", "1"}});
  assert(server.code == 409); // Another enabled slot still requires this profile's Analogue protocol.
  btSyncEnabled[0] = false;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_protocol", "1"}});
  assert(server.code == 500 && btSyncProtocol[2] == BT_PROTOCOL_ANALOGUE && btWindowActive);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 500 && btSyncEnabled[2] && btWindowActive && btSyncActiveSlot == 2);
  server.post("/api/config", {{"bt_slot", "3"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 500 && !btSyncEnabled[3]);
  assert(flash.at(CONFIG_FILE) == savedConfig);
  mockRenameOk = true; btSyncEnabled[0] = true;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_time", "1319"}});
  assert(server.code == 200 && !btWindowActive);
  btSyncTimes[2] = 700; loadConfig(); assert(btSyncTimes[2] == 1319);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_profile", "1"}});
  assert(server.code == 200); btSyncProfile[2] = 0; loadConfig(); assert(btSyncProfile[2] == 1);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_protocol", "1"}});
  assert(server.code == 200); btSyncProtocol[2] = 2; loadConfig(); assert(btSyncProtocol[2] == 1);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 200); btSyncEnabled[2] = true; loadConfig(); assert(!btSyncEnabled[2]);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 200); btSyncEnabled[2] = false; loadConfig(); assert(btSyncEnabled[2]);

  // RF-only saves never rewrite Bluetooth toggles, even if legacy overlapping
  // Bluetooth settings overlap. Boot reporting also retains these switches.
  btSyncEnabled[3] = true; btSyncTimes[3] = 1319;
  server.post("/api/schedules", {{"plain", rfJson}});
  assert(server.code == 200 && btSyncEnabled[2] && btSyncEnabled[3]);
  writeConfigNow(); btSyncEnabled[2] = false; btSyncEnabled[3] = false;
  loadConfig(); assert(btSyncEnabled[2] && btSyncEnabled[3]);
  mockRenameOk = false;
  server.post("/api/schedules", {{"plain", "[{\"station\":0,\"start\":710,\"end\":719}]"}});
  assert(server.code == 500 && btSyncEnabled[2] && btSyncEnabled[3] && !rfPaused);
  assert(schedules[0].start_min == 713 && schedules[0].end_min == 718);
  mockRenameOk = true;
  normalizeBluetoothSlots(); assert(btSyncEnabled[2] && btSyncEnabled[3]);

  // Every UI settings response acknowledges durable storage, including
  // independent station/BT zones and offsets, Wi-Fi and manual selections.
  server.post("/api/config", {{"ssid", "Durable Wi-Fi"}, {"password", "sample-pass"},
                            {"timezone", "Australia/Sydney"}, {"full_time_tx", "false"},
                            {"full_time_station", "3"}, {"transmission_offset_minutes", "15"},
                            {"bt_timezone", "Asia/Tokyo"}, {"bt_time_offset_minutes", "30"},
                            {"wifi_power_mode", "1"}});
  assert(server.code == 200 && timezoneApplications == 1 && !configDirty);
  timezone_name = "Europe/London"; btTimezoneName = "Europe/London";
  transmission_offset_minutes = 0; btTimeOffsetMinutes = 0; wifiPowerMode = 0;
  ssid[0] = '\0'; passwd[0] = '\0'; full_time_station = 0;
  loadConfig();
  assert(timezone_name == "Australia/Sydney" && btTimezoneName == "Asia/Tokyo");
  assert(transmission_offset_minutes == 15 && btTimeOffsetMinutes == 30 && wifiPowerMode == 1);
  assert(std::strcmp(ssid, "Durable Wi-Fi") == 0 && std::strcmp(passwd, "sample-pass") == 0 && full_time_station == 3);
  server.post("/api/config", {{"bt_manual_protocol", "2"}}); assert(server.code == 200);
  server.post("/api/config", {{"bt_manual_profile", "3"}}); assert(server.code == 200);
  server.post("/api/settings", {{"bt_always_wait", "true"}}); assert(server.code == 200);
  btManualProtocol = 0; btManualProfile = 0; btAlwaysWaitEnabled = false;
  loadConfig(); assert(btManualProtocol == 2 && btManualProfile == 3 && btAlwaysWaitEnabled);
  const auto uiSavedConfig = flash.at(CONFIG_FILE);
  mockRenameOk = false;
  server.post("/api/config", {{"bt_manual_protocol", "1"}});
  assert(server.code == 500 && btManualProtocol == 2);
  server.post("/api/config", {{"bt_manual_profile", "1"}});
  assert(server.code == 500 && btManualProfile == 3);
  server.post("/api/settings", {{"bt_always_wait", "false"}});
  assert(server.code == 500 && btAlwaysWaitEnabled);
  server.post("/api/config", {{"bt_time_offset_minutes", "120"}});
  assert(server.code == 500 && btTimeOffsetMinutes == 120 && server.response.indexOf("not saved") >= 0);
  assert(flash.at(CONFIG_FILE) == uiSavedConfig);
  mockRenameOk = true; loadConfig(); assert(btTimeOffsetMinutes == 30);
  std::puts("Real RF-save preserves Bluetooth settings; every daily slot listens, durable edits/rollback and reboot retention passed");
}
'''


class SchedulePersistenceTest(unittest.TestCase):
    def test_rf_save_preserves_bt_settings(self):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for host workflow tests')
        tools_dir = Path(os.environ.get('RADIOCLOCK_TOOLS_DIR', '/workspace/.radioclock-tools'))
        json_headers = tools_dir / 'user/libraries/ArduinoJson/src'
        self.assertTrue((json_headers / 'ArduinoJson.h').is_file(),
                        'Run scripts/install-toolchain.sh for the pinned ArduinoJson RF-save integration test')
        source = FIRMWARE.read_text()
        unit_source = workflow.BluetoothWorkflowTest().unit_source(source)
        mocks = MOCKS
        mocks = mocks.replace('#include "RadioBleArbiter.h"', '#include "ArduinoJson.h"\n#include "RadioBleArbiter.h"\n#include "RadioReliability.h"')
        mocks = mocks.replace('struct SerialMock {', '''
DeserializationError deserializeJson(JsonDocument& doc, String json) {
  return ArduinoJson::deserializeJson(doc, json.c_str());
}
struct SerialMock {''')
        mocks = mocks.replace('struct Schedule { int start_min, end_min; } schedules[4]{};', '''
constexpr int MAX_SCHEDULES = 24, NUM_STATIONS = 7;
struct TimeSchedule {
  int station = 0, start_min = 0, end_min = 0;
  TimeSchedule() = default;
  TimeSchedule(int start, int end) : start_min(start), end_min(end) {}
};
TimeSchedule schedules[MAX_SCHEDULES]{};
bool rfPaused = false;
bool radioSetPaused(bool pause) { rfPaused = pause; return true; }
bool radioAcquirePause(RadioPauseLease& lease) {
  lease.wasPaused=rfPaused; lease.acquired=true; rfPaused=true; return true;
}
bool radioReleasePause(RadioPauseLease& lease) {
  assert(lease.acquired); lease.acquired=false; rfPaused=lease.wasPaused; return true;
}''')
        mocks = mocks.replace('  void flush() {}', '''
  size_t write(uint8_t value) { contents->push_back(char(value)); return 1; }
  void flush() {}''')
        mocks = mocks.replace('struct LittleFsMock {', 'bool mockRenameOk = true;\nstruct LittleFsMock {')
        mocks = mocks.replace('flash[to] = flash.at(from);', 'if (!mockRenameOk) return false; flash[to] = flash.at(from);')
        mocks = mocks.replace('struct WiFiMock { int status() const { return WL_CONNECTED; } } WiFi;', '''
struct WiFiMock {
  int status() const { return WL_CONNECTED; }
  void mode(int) {}
  void setSleep(bool) {}
  void begin(const char*, const char*) {}
} WiFi;''')
        mocks += '\nconstexpr const char *STATION_CONFIG_FILE = "schedules", *SCHEDULE_TEMP_FILE = "schedules.tmp";\n'
        mocks += '''
constexpr int WIFI_STA = 1;
int last_station = 0, timezoneApplications = 0;
unsigned long wifi_connect_start = 0;
void applyTimezone() { ++timezoneApplications; }
'''
        for name in ['WIFI_PRE_SCHEDULE_LEAD_MIN', 'WIFI_PERIODIC_WAKE_INTERVAL_MS', 'WIFI_PERIODIC_WAKE_DURATION_MS']:
            mocks += '\n' + re.search(r'^#define ' + name + r'\s+[^\n]+', source, re.M).group(0) + '\n'
        unit_source = unit_source.replace(MOCKS, mocks, 1)
        schedule_functions = '\n'.join(extract_function(source, name) for name in
                                       ['validScheduleJson', 'normalizeBluetoothSlots', 'saveSchedules', 'shouldWifiBeOnForSchedule'])
        schedule_route = extract_route(source, '/api/schedules')
        unit_source = unit_source.replace('void registerRoutes() {',
                                          schedule_functions + '\nvoid registerRoutes() {\n' + schedule_route, 1)
        # Use the complete shipped config handler, not only its slot branches.
        unit_source = unit_source.replace(extract_route(unit_source, '/api/config'),
                                          extract_route(source, '/api/config'), 1)
        bind_helper = DRIVER[:DRIVER.index('static void manualRetry(')]
        unit_source = unit_source.replace(DRIVER, bind_helper + SCHEDULE_DRIVER)
        with tempfile.TemporaryDirectory(prefix='radioclock-schedule-workflow-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(unit_source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), '-I', str(json_headers), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
