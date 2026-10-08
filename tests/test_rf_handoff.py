#!/usr/bin/env python3
"""Check actual firmware RF handoff code against LEDC/controller failure mocks.

These validate software decisions and ordering, not the physical PWM waveform.
Run with python3 tests/test_rf_handoff.py.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V4_11/RadioClock_V4_11.ino'


def extract_function(source, name):
    match = re.search(r'^(?:static )?(?:bool|void) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'Function {name} missing')
    depth, end = 1, match.end()
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[match.start():end]


def extract_radio_command_tick(source):
    """Run the task's real command block once instead of its infinite loop."""
    task = extract_function(source, 'radioTask')
    prefix, loop = task.split('for (;;) {', 1)
    # Preserve task-owned acknowledgement state across these single-tick calls.
    persistent = re.findall(r'^\s*bool (\w+) = false;.*$', prefix, re.M)
    declarations = '\n'.join(f'static bool {name} = false;' for name in persistent)
    commands = loop.split('// Derive the frame phase', 1)[0]
    return f'void radioCommandTick() {{\n{declarations}\nfor (int tick=0;tick<1;++tick) {{\n{commands}\n}}\n}}'


MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include "firmware/RadioClock_V4_11/RadioBleArbiter.h"
#define PIN_RADIO 12
#define PIN_LED 25
#define PIN_BUZZ 26
#define OUTPUT 1
#define LOW 0
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define MAX_SCHEDULES 24
#define ROTATION_INTERVAL_MINUTES 5
struct TimeSchedule { unsigned station; int start_min, end_min; };
TimeSchedule schedules[MAX_SCHEDULES]{};
int schedule_count = 0;
std::atomic<int> applicable_schedules[MAX_SCHEDULES]{};
std::atomic<int> applicable_count{0};
int current_schedule_index = -1;
unsigned long last_rotation_time = 0;
std::atomic<int> last_station{-1};
std::atomic<bool> full_time_tx{false};
std::atomic<int> full_time_station{0};
std::atomic<bool> btRadioSuspendRequested{false};
RadioBleArbiter radioBleArbiter;
struct tm nowtm{};
bool trusted = true;
bool carrierReady = false;
std::atomic<bool> rfSilenceFailed{false};
int ampmod = 0;
int buzzout = 0;
std::atomic<bool> radioPauseRequested{false}, radioResumeRequested{false};
std::atomic<bool> radioPaused{false}, radioPauseAck{false}, encodingRefreshRequested{false};
unsigned ulTaskNotifyTake(bool, unsigned) { return 1; }
void (*makebitpattern)(void) = nullptr;
unsigned long clockMillis = 0;
unsigned long millis() { return clockMillis; }
bool clockTrusted() { return trusted; }
bool dutyWriteSucceeds = true, detachSucceeds = true;
unsigned duty = 0, dutyWrites = 0, detachCalls = 0, forcedLowWrites = 0;
unsigned stationCalls = 0;
bool ledcWrite(int pin, unsigned value) {
  assert(pin == PIN_RADIO);
  ++dutyWrites;
  if (!dutyWriteSucceeds) return false;
  duty = value;
  return true;
}
bool ledcDetach(int pin) {
  assert(pin == PIN_RADIO);
  ++detachCalls;
  return detachSucceeds;
}
void pinMode(int pin, int mode) { assert(pin == PIN_RADIO && mode == OUTPUT); }
void digitalWrite(int pin, int value) {
  if (pin == PIN_RADIO) {
    assert(value == LOW);
    ++forcedLowWrites;
    duty = 0;
  }
}
void setstation(int station) {
  // This stub checks that real scheduler code acquires ownership first.
  assert(radioBleArbiter.rfOwned());
  ++stationCalls;
  carrierReady = true;
  last_station = station;
  duty = 512;
}
void reset() {
  duty = 0;
  if (radioBleArbiter.bleOwned()) assert(radioBleArbiter.finishBleShutdown(true));
  radioBleArbiter.releaseRf();
  trusted = true;
  carrierReady = false;
  rfSilenceFailed = false;
  dutyWriteSucceeds = detachSucceeds = true;
  dutyWrites = detachCalls = forcedLowWrites = stationCalls = 0;
  last_station = -1;
  full_time_tx = false;
  full_time_station = 0;
  btRadioSuspendRequested = false;
  schedule_count = applicable_count = 0;
  current_schedule_index = -1;
  nowtm = {};
}
void transmitting() {
  assert(radioBleArbiter.requestRf());
  carrierReady = true;
  last_station = 0;
  duty = 512;
}
'''


CASES = r'''
int main() {
  reset();
  transmitting();
  applyCurrentSchedule(); // no schedule: ordinary RF session finish
  assert(duty == 0 && !rfSilenceFailed && !radioBleArbiter.rfOwned());
  assert(radioBleArbiter.acquireBle());

  reset();
  transmitting();
  dutyWriteSucceeds = detachSucceeds = false;
  applyCurrentSchedule();
  assert(duty == 512 && rfSilenceFailed && radioBleArbiter.rfOwned());
  assert(!radioBleArbiter.acquireBle());
  assert(detachCalls == 1 && forcedLowWrites == 0);
  detachSucceeds = true;
  applyCurrentSchedule();
  assert(duty == 0 && !carrierReady && !rfSilenceFailed);
  assert(forcedLowWrites == 1 && radioBleArbiter.acquireBle());

  reset();
  transmitting();
  trusted = false; // clock confidence lost during RF transmission
  dutyWriteSucceeds = detachSucceeds = false;
  applyCurrentSchedule();
  assert(radioBleArbiter.rfOwned() && !radioBleArbiter.acquireBle());
  assert(rfSilenceFailed && duty == 512);
  dutyWriteSucceeds = true;
  applyCurrentSchedule();
  assert(duty == 0 && radioBleArbiter.acquireBle());

  for (bool fullTime : {false, true}) {
    reset();
    full_time_tx = fullTime;
    if (!fullTime) {
      schedules[0] = {0, 0, 1440};
      schedule_count = 1;
    }
    assert(radioBleArbiter.acquireBle());
    carrierReady = true; // channel retained from an earlier, silenced session
    applyCurrentSchedule();
    assert(btRadioSuspendRequested && radioBleArbiter.bleOwned());
    assert(duty == 0 && stationCalls == 0);
    assert(!radioBleArbiter.finishBleShutdown(false));
    applyCurrentSchedule();
    assert(duty == 0 && stationCalls == 0);
    assert(radioBleArbiter.finishBleShutdown(true));
    applyCurrentSchedule();
    assert(radioBleArbiter.rfOwned() && duty == 512 && stationCalls == 1);
    assert(!radioBleArbiter.acquireBle());
    duty = 0; // zero-carrier slot is still inside the RF session
    applyCurrentSchedule();
    assert(radioBleArbiter.rfOwned() && !radioBleArbiter.acquireBle());

    // A pause recovered by detach must reattach even with the same station.
    dutyWriteSucceeds = false;
    assert(silenceRfCarrier());
    assert(!carrierReady && duty == 0);
    dutyWriteSucceeds = true;
    applyCurrentSchedule();
    assert(carrierReady && duty == 512 && stationCalls == 2);
  }

  reset();
  transmitting();
  dutyWriteSucceeds = detachSucceeds = false;
  radioPauseRequested = true;
  radioCommandTick();
  assert(radioPaused && !radioPauseAck && rfSilenceFailed);
  assert(radioBleArbiter.rfOwned());
  dutyWriteSucceeds = true;
  radioCommandTick(); // failed pause can acknowledge after a successful retry
  assert(radioPaused && radioPauseAck && duty == 0);

  // Preempt loopTask between Ack=false and ResumeRequested=true. An old paused
  // tick must not acknowledge the next request before it was even published.
  radioPauseAck = false;
  radioCommandTick();
  assert(radioPaused && !radioPauseAck);
  radioResumeRequested = true;
  radioCommandTick();
  assert(!radioPaused && radioPauseAck && encodingRefreshRequested);
  puts("Real RF handoff tests passed: silence failure, detach recovery, lost confidence, scheduled/full-time preemption");
}
'''


class RfHandoffTests(unittest.TestCase):
    def test_real_rf_scheduler_and_failure_paths(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ is required for host RF handoff tests')
        source = FIRMWARE.read_text()
        code = MOCKS + '\n' + extract_function(source, 'silenceRfCarrier')
        code += '\n' + extract_radio_command_tick(source)
        code += '\n' + extract_function(source, 'applyCurrentSchedule') + '\n' + CASES
        with tempfile.TemporaryDirectory(prefix='radio-rf-test-') as directory:
            path = Path(directory)
            (path / 'rf-test.cpp').write_text(code)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(ROOT), str(path / 'rf-test.cpp'), '-o', str(path / 'rf-test')],
                           check=True)
            subprocess.run([str(path / 'rf-test')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
