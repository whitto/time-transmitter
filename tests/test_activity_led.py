#!/usr/bin/env python3
"""Exercise shipped BT-only LED flashing, 24-hour success state and settings."""
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

FIRMWARE = workflow.FIRMWARE

LED_MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <vector>
#include "RadioBleArbiter.h"
#include "BtSyncLed.h"
constexpr int NUM_STATIONS = 7, PIN_ONBOARD_LED = 2, HIGH = 1, LOW = 0;
std::atomic<bool> carrierReady{false}, radioPaused{false}, rfSilenceFailed{false};
std::atomic<bool> btBleBusy{false}, activityLedEnabled{true};
std::atomic<uint32_t> btLastSyncEpoch{0};
std::atomic<bool> btLastOutcomeSuccessful{false};
std::atomic<int> last_station{-1};
RadioBleArbiter radioBleArbiter;
time_t fakeUtc = 1700000000;
time_t mockTime(time_t *out) { if (out) *out = fakeUtc; return fakeUtc; }
#define time mockTime
uint32_t tick = 0;
uint32_t millis() { return tick; }
struct Edge { uint32_t when; bool high; };
std::vector<Edge> edges;
int pinLevel = LOW;
void digitalWrite(int pin, int level) {
  assert(pin == PIN_ONBOARD_LED); pinLevel = level;
  edges.push_back({tick, level == HIGH});
}
#define pdMS_TO_TICKS(ms) (ms)
std::function<void()> duringDelay;
int delays = 0, stopAfter = 20;
struct TaskStopped {};
void vTaskDelay(uint32_t ms) {
  assert(ms == 50); tick += ms;
  if (duringDelay) duringDelay();
  if (++delays == stopAfter) throw TaskStopped{};
}
'''

LED_DRIVER = r'''
static void runTask() {
  edges.clear(); delays = 0; pinLevel = LOW;
  try { activityLedTask(nullptr); } catch (const TaskStopped&) {}
  assert(delays == stopAfter);
}
int main() {
  constexpr int64_t success = 1700000000;
  assert(BtSyncLed::successEligible(success, success, true));
  assert(BtSyncLed::successEligible(success + 86399, success, true));
  assert(!BtSyncLed::successEligible(success + 86400, success, true));
  assert(!BtSyncLed::successEligible(success + 86401, success, true));
  assert(!BtSyncLed::successEligible(success + 1, success, false));
  assert(!BtSyncLed::successEligible(success - 1, success, true));
  assert(!BtSyncLed::successEligible(0, success, true));
  assert(!BtSyncLed::successEligible(success, 0, true));
  assert(!BtSyncLed::successEligible(1577836799, 1577836799, true));
  assert(BtSyncLed::successEligible(1577836800, 1577836800, true));
  assert(!BtSyncLed::successEligible(INT64_MAX, success, true));

  uint32_t anchor = 0; bool level = false, wasActive = false;
  assert(!activityLedLevel(100, true, false, false, anchor, level, wasActive));
  assert(activityLedLevel(120, true, true, false, anchor, level, wasActive)); // Immediate visible short-BT pulse.
  assert(activityLedLevel(219, true, true, false, anchor, level, wasActive));
  assert(!activityLedLevel(220, true, false, false, anchor, level, wasActive));
  assert(activityLedLevel(300, true, true, false, anchor, level, wasActive));
  assert(!activityLedLevel(550, true, true, false, anchor, level, wasActive));
  assert(activityLedLevel(800, true, true, false, anchor, level, wasActive));
  assert(activityLedLevel(1300, true, true, false, anchor, level, wasActive)); // Two elapsed phases retain phase.
  assert(!activityLedLevel(1301, false, true, true, anchor, level, wasActive));
  assert(activityLedLevel(1302, true, true, true, anchor, level, wasActive)); // Re-enable during activity.
  assert(!activityLedLevel(1303, true, false, false, anchor, level, wasActive));
  assert(activityLedLevel(1400, true, false, true, anchor, level, wasActive)); // Successful idle state is solid.
  assert(activityLedLevel(4000, true, false, true, anchor, level, wasActive));
  assert(!wasActive && level);
  assert(!activityLedLevel(4001, false, false, true, anchor, level, wasActive));
  assert(activityLedLevel(4002, true, false, true, anchor, level, wasActive));
  assert(activityLedLevel(4003, true, true, true, anchor, level, wasActive));
  assert(!activityLedLevel(4253, true, true, true, anchor, level, wasActive)); // New sync flashes even after success.
  assert(activityLedLevel(4254, true, false, true, anchor, level, wasActive)); // Success restores solid.
  assert(!activityLedLevel(4255, true, false, false, anchor, level, wasActive)); // Failure cancels success hold.
  wasActive = false;
  assert(activityLedLevel(UINT32_MAX - 100U, true, true, false, anchor, level, wasActive));
  assert(!activityLedLevel(149U, true, true, false, anchor, level, wasActive));
  assert(anchor == 149U);
  assert(activityLedLevel(399U, true, true, false, anchor, level, wasActive));
  assert(!activityLedLevel(1149U, true, true, false, anchor, level, wasActive));

  assert(!activityLedSyncActive()); // Wi-Fi/AP or passive listening has no activity flags.
  btBleBusy = true; assert(activityLedSyncActive());
  btBleBusy = false; assert(!activityLedSyncActive());
  assert(radioBleArbiter.requestRf());
  assert(!activityLedSyncActive()); // Ownership alone is not a configured transmission.
  last_station = 0; assert(!activityLedSyncActive());
  carrierReady = true; assert(!activityLedSyncActive()); // Actual RF never flashes the blue LED.
  radioPaused = true; assert(!activityLedSyncActive());
  radioPaused = false; rfSilenceFailed = true; assert(!activityLedSyncActive());
  rfSilenceFailed = false; last_station = NUM_STATIONS; assert(!activityLedSyncActive());
  last_station = 0; carrierReady = false; assert(!activityLedSyncActive());
  carrierReady = true; radioBleArbiter.releaseRf(); assert(!activityLedSyncActive());

  // Run the actual separate task without calling Arduino loop() at all:
  // busy Bluetooth still flashes throughout a simulated blocked GATT write.
  tick = 0; btBleBusy = true; runTask();
  assert(edges.size() == 4 && edges[0].when == 0 && edges[0].high);
  assert(edges[1].when == 250 && !edges[1].high && edges[2].when == 500 && edges[2].high && edges[3].when == 750 && !edges[3].high);
  btBleBusy = false; tick = 0; runTask(); assert(edges.empty()); // Listening remains dark.
  assert(radioBleArbiter.requestRf()); tick = 0; runTask(); assert(edges.empty());
  btLastSyncEpoch = static_cast<uint32_t>(success); btLastOutcomeSuccessful = true;
  fakeUtc = success + 60; assert(activityLedSuccessEligible());
  tick = 0; runTask(); assert(edges.size() == 1 && edges[0].high && pinLevel == HIGH); // Solid continues throughout RF.
  assert(radioBleArbiter.rfOwned() && carrierReady && last_station == 0);
  activityLedEnabled = false; tick = 0; runTask(); assert(edges.empty()); // RF is unaffected.
  assert(radioBleArbiter.rfOwned() && carrierReady && last_station == 0);
  activityLedEnabled = true;
  duringDelay = [] { if (tick == 100) activityLedEnabled = false; };
  tick = 0; runTask();
  assert(edges.size() == 2 && edges[0].when == 0 && edges[0].high);
  assert(edges[1].when == 100 && !edges[1].high && pinLevel == LOW);
  assert(radioBleArbiter.rfOwned());
  activityLedEnabled = true;
  duringDelay = [] { if (tick == 100) radioPaused = true; };
  tick = 0; runTask(); assert(edges.size() == 1 && edges[0].high); // RF pause also leaves the success indicator steady.
  radioPaused = false; radioBleArbiter.releaseRf();
  btBleBusy = true;
  duringDelay = [] { if (tick == 100) { btBleBusy = false; btLastOutcomeSuccessful = false; } };
  tick = 0; runTask(); assert(edges.size() == 2 && edges[1].when == 100 && !edges[1].high);
  btBleBusy = true;
  duringDelay = [] { if (tick == 100) { btLastSyncEpoch = static_cast<uint32_t>(fakeUtc); btLastOutcomeSuccessful = true; btBleBusy = false; } };
  tick = 0; runTask(); assert(edges.size() == 1 && edges[0].high); // Success becomes solid without an off pulse.
  duringDelay = [] { if (tick == 100) fakeUtc = static_cast<time_t>(btLastSyncEpoch.load()) + 86400; };
  tick = 0; runTask(); assert(edges.size() == 2 && edges[1].when == 100 && !edges[1].high);
  fakeUtc = 0; assert(!activityLedSuccessEligible());
  duringDelay = [] { if (tick == 100) fakeUtc = static_cast<time_t>(btLastSyncEpoch.load()) + 60; };
  tick = 0; runTask(); assert(edges.size() == 1 && edges[0].when == 100 && edges[0].high); // Reboot waits for a plausible UTC clock.
  duringDelay = nullptr;
  for (const char *zone : {"JST-9", "AEST-10", "EST5EDT"}) {
    assert(setenv("TZ", zone, 1) == 0); tzset();
    assert(activityLedSuccessEligible());
  }
  std::puts("Actual LED task: BT-only flashing, 24-hour UTC success, failure/off override, RF isolation and rollover passed");
}
'''

PERSISTENCE_DRIVER = r'''
int main() {
  registerRoutes();
  activityLedEnabled = false; btAlwaysWaitEnabled = false;
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  loadConfig();
  assert(activityLedEnabled && btAlwaysWaitEnabled);
  for (bool enabled : btSyncEnabled) assert(enabled);
  assert(!btPairRequested && !btManualSyncRequested);
  // A legacy/truncated file has no optional flags: defaults are explicitly On.
  flash[CONFIG_FILE] = "old Wi-Fi\npassword\nAsia/Tokyo\n0\n0\n0\n0\n";
  activityLedEnabled = false; btAlwaysWaitEnabled = false;
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  loadConfig(); assert(activityLedEnabled && btAlwaysWaitEnabled);
  for (bool enabled : btSyncEnabled) assert(enabled);

  // Saved explicit Off values override the defaults after simulated reboot.
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  btAlwaysWaitEnabled = false;
  server.post("/api/config", {{"activity_led_enabled", "false"}});
  assert(server.code == 200 && !activityLedEnabled && !configDirty);
  const auto offConfig = flash.at(CONFIG_FILE);
  activityLedEnabled = true; btAlwaysWaitEnabled = true;
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), true);
  loadConfig(); assert(!activityLedEnabled && !btAlwaysWaitEnabled);
  for (bool enabled : btSyncEnabled) assert(!enabled);
  assert(flash.at(CONFIG_FILE) == offConfig);
  server.post("/api/config", {{"activity_led_enabled", "true"}});
  assert(server.code == 200 && activityLedEnabled && !configDirty);
  activityLedEnabled = false; loadConfig(); assert(activityLedEnabled);

  // V4.5 config without the newly appended LED line keeps all preceding
  // timezone/watch fields and uses the default flashing preference.
  btTimezoneName = "Asia/Tokyo"; btTimeOffsetMinutes = 30;
  activityLedEnabled = false; assert(writeConfigNow());
  auto newConfig = flash.at(CONFIG_FILE);
  const std::string lastLegacyFields = "\nAsia/Tokyo\n30\n";
  const auto legacyEnd = newConfig.rfind(lastLegacyFields);
  assert(legacyEnd != std::string::npos);
  newConfig.erase(legacyEnd + lastLegacyFields.size());
  flash[CONFIG_FILE] = newConfig;
  activityLedEnabled = false; btTimezoneName = "Europe/London"; btTimeOffsetMinutes = 0;
  loadConfig(); assert(activityLedEnabled && btTimezoneName == "Asia/Tokyo" && btTimeOffsetMinutes == 30);

  assert(writeConfigNow());
  const auto priorConfig = flash.at(CONFIG_FILE);
  const auto priorRefreshes = radioRefreshes;
  // Rejected values and every flash-failure mode retain the previous setting.
  for (const char* bad : {"", "yes", "2", "-1"}) {
    server.post("/api/config", {{"activity_led_enabled", bad}});
    assert(server.code == 400 && activityLedEnabled && flash.at(CONFIG_FILE) == priorConfig);
  }
  mockRenameOk = false;
  server.post("/api/config", {{"activity_led_enabled", "false"}});
  assert(server.code == 500 && activityLedEnabled && flash.at(CONFIG_FILE) == priorConfig);
  mockRenameOk = true; mockOpenOk = false;
  server.post("/api/config", {{"activity_led_enabled", "0"}});
  assert(server.code == 500 && activityLedEnabled && flash.at(CONFIG_FILE) == priorConfig);
  mockOpenOk = true; mockShortWrite = true;
  server.post("/api/config", {{"activity_led_enabled", "false"}});
  assert(server.code == 500 && activityLedEnabled && flash.at(CONFIG_FILE) == priorConfig);
  mockShortWrite = false;
  loadConfig(); assert(activityLedEnabled);
  server.post("/api/config", {{"activity_led_enabled", "0"}});
  assert(server.code == 200 && !activityLedEnabled && !configDirty);
  activityLedEnabled = true; loadConfig(); assert(!activityLedEnabled);
  assert(radioRefreshes == priorRefreshes && !btPairRequested && !btManualSyncRequested);
  std::puts("LED saved before ACK, legacy defaults, explicit Off reboot retention and all flash-failure rollbacks passed");
}
'''


class ActivityLedTest(unittest.TestCase):
    def compile_and_run(self, unit_source):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for LED host tests')
        with tempfile.TemporaryDirectory(prefix='radioclock-led-test-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(unit_source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_real_led_task_and_activity(self):
        source = FIRMWARE.read_text()
        functions = '\n'.join(workflow.extract_function(source, name) for name in
                              ['activityLedLevel', 'activityLedSyncActive', 'activityLedSuccessEligible', 'activityLedTask'])
        self.compile_and_run(LED_MOCKS + functions + LED_DRIVER)

    def test_saved_led_and_default_bt_settings(self):
        source = FIRMWARE.read_text()
        unit_source = workflow.BluetoothWorkflowTest().unit_source(source)
        mocks = workflow.MOCKS
        mocks = mocks.replace('struct LittleFsMock {',
                              'bool mockRenameOk = true, mockOpenOk = true, mockShortWrite = false;\nstruct LittleFsMock {')
        mocks = mocks.replace("if (*mode == 'w') {", "if (*mode == 'w') { if (!mockOpenOk) return {}; ")
        mocks = mocks.replace('flash[to] = flash.at(from);', 'if (!mockRenameOk) return false; flash[to] = flash.at(from);')
        mocks = mocks.replace('struct File {', 'extern bool mockShortWrite;\nstruct File {')
        mocks = mocks.replace('contents->append(reinterpret_cast<const char*>(data), count); return count;',
                              'contents->append(reinterpret_cast<const char*>(data), count); return mockShortWrite ? 0 : count;')
        unit_source = unit_source.replace(workflow.MOCKS, mocks, 1).replace(workflow.DRIVER, PERSISTENCE_DRIVER, 1)
        self.compile_and_run(unit_source)

    def test_shared_c3_envelope_pin_has_no_activity_owner(self):
        source = FIRMWARE.read_text()
        self.assertIn('#define PIN_ONBOARD_LED (5)', source)
        self.assertIn('#define PIN_LED    (5)', source)
        # All blue-LED writes belong to the dedicated task or guarded boot
        # initializer. Wi-Fi setup/wake/loop have no additional writers.
        task = workflow.extract_function(source, 'activityLedTask')
        self.assertEqual(task.count('digitalWrite(PIN_ONBOARD_LED,'), 1)
        self.assertIn('#if PIN_ONBOARD_LED != PIN_LED\nstatic void activityLedTask', source)
        setup = workflow.extract_function(source, 'setup')
        self.assertIn('#if PIN_ONBOARD_LED != PIN_LED\n  pinMode(PIN_ONBOARD_LED, OUTPUT);\n  digitalWrite(PIN_ONBOARD_LED, LOW);\n#endif', setup)
        self.assertEqual(source.count('digitalWrite(PIN_ONBOARD_LED,'), 2)
        self.assertIn('loadConfig();', setup)
        self.assertLess(setup.index('loadConfig();'), setup.index('ledTaskCreated ='))


if __name__ == '__main__':
    unittest.main()
