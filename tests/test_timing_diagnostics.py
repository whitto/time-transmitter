#!/usr/bin/env python3
"""Fault-inject the production timing diagnostic helper and radio-task hook.

The native driver uses the shipped header and the complete, unmodified
radioTask body. GPIO, task wakeups, clock samples and schedule ownership are
mocked; this validates diagnostic decisions, not physical RF edges.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_clock_and_encoders import function

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware/RadioClock_V4_16/RadioClock_V4_16.ino"

HELPER_DRIVER = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include "RadioReliability.h"

using Timing = RadioBoundaryDiagnostics;
static_assert(sizeof(Timing) <= 96, "diagnostics need bounded task-local memory");
static_assert(std::is_trivially_destructible<Timing>::value, "no heap-owned members");

void initialClock() {
  Timing d;
  d.observe(549, 0, 549000000ULL, false, 0);
  d.observe(1791590127LL, 468378, 550000000ULL, false, 2);
  assert(1791590127LL - 549 - 1 == 1791589577LL); // Actual old failure.
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().worstUs == 0);
  d.observe(1791590128LL, 999000, 551000000ULL, true, 2);
  assert(d.snapshot().delayUs == 0 && d.snapshot().worstUs == 0);
  d.observe(1791590129LL, 373, 551001373ULL, true, 2);
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 373);
  assert(d.snapshot().worstUs == 373 && d.snapshot().processedSeconds == 1);
}

void steps() {
  Timing d;
  d.observe(100, 100, 1000000, true, 2);
  d.observe(101, 373, 2000273, true, 2);
  assert(d.snapshot().worstUs == 373);
  // A large correction is rejected even without the clock-step marker.
  d.observe(7302, 468378, 3000273, true, 2);
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 0);
  assert(d.snapshot().worstUs == 373 && d.snapshot().processedSeconds == 1);
  d.observe(7303, 100, 3531995, true, 2);
  assert(d.snapshot().delayUs == 100 && d.snapshot().worstUs == 373);
  d.observe(103, 600000, 4531995, true, 4); // Backwards accepted correction.
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 0);
  assert(d.snapshot().worstUs == 373 && d.snapshot().processedSeconds == 2);
  d.observe(104, 200, 4932195, true, 4);
  assert(d.snapshot().delayUs == 200 && d.snapshot().worstUs == 373);
  // Even an accepted sub-millisecond adjustment starts a fresh baseline.
  d.observe(105, 250, 5932195, true, 6);
  assert(d.snapshot().delayUs == 0 && d.snapshot().worstUs == 373);
}

void concurrentStep() {
  Timing d;
  d.observe(100, 0, 1000000, true, 2);
  d.observe(101, 200, 2000200, true, 2);
  const uint32_t processed = d.snapshot().processedSeconds;
  d.observe(7200, 999999, 2000300, true, 3, false);
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 0);
  assert(d.snapshot().worstUs == 200 && d.snapshot().processedSeconds == processed);
  d.observe(7201, 500000, 2500301, true, 4);
  assert(d.snapshot().delayUs == 0 && d.snapshot().worstUs == 200);
  d.observe(7202, 100, 3000401, true, 4);
  assert(d.snapshot().delayUs == 100 && d.snapshot().worstUs == 200);
  // The 32-bit sequence counter rolling over is still a changed generation.
  d.observe(7203, 500, 4000801, true, UINT32_MAX - 1);
  assert(d.snapshot().delayUs == 0);
  d.observe(7204, 600, 5000901, true, 0);
  assert(d.snapshot().delayUs == 0 && d.snapshot().missedBoundaries == 0);
}

void suspendAndIdle() {
  Timing d;
  d.observe(100, 0, 1000000, true, 2);
  d.observe(101, 500, 2000500, true, 2);
  d.suspend();
  assert(d.snapshot().delayUs == 0 && d.snapshot().worstUs == 500);
  d.observe(90000, 900000, 89901900000ULL, true, 2);
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 0);
  d.observe(90001, 600, 89902000600ULL, true, 2);
  assert(d.snapshot().delayUs == 600 && d.snapshot().worstUs == 600);
  d.observe(90002, 900000, 89903800000ULL, false, 2);
  assert(d.snapshot().delayUs == 0 && d.snapshot().missedBoundaries == 0);
  d.observe(100000, 950000, 99899150000ULL, true, 2);
  assert(d.snapshot().delayUs == 0 && d.snapshot().worstUs == 600);
}

void genuineMisses() {
  Timing d;
  d.observe(100, 900000, 1000000, true, 2);
  d.observe(101, 100, 1100100, true, 2); // Crossed a second in only 100100us.
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 100);
  d.observe(105, 900, 5100900, true, 2); // Three real unobserved seconds.
  assert(d.snapshot().missedBoundaries == 3 && d.snapshot().delayUs == 900);
  assert(d.snapshot().worstUs == 900 && d.snapshot().processedSeconds == 2);
  d.observe(105, 1900, 5101900, true, 2); // Same second is not counted again.
  assert(d.snapshot().missedBoundaries == 3 && d.snapshot().processedSeconds == 2);
  d.observe(106, 373, 6100373, true, 2);
  assert(d.snapshot().missedBoundaries == 3 && d.snapshot().worstUs == 900);
}

void rollovers() {
  for (uint64_t start : {uint64_t(UINT32_MAX) - 1000000ULL,
                       uint64_t(UINT32_MAX) * 1000ULL - 1000000ULL,
                       20ULL * 365ULL * 86400ULL * 1000000ULL}) {
    Timing d;
    d.observe(1791590127LL, 0, start, true, 2);
    d.observe(1791590128LL, 373, start + 1000373, true, 2);
    d.observe(1791590132LL, 100, start + 5000100, true, 2);
    assert(d.snapshot().missedBoundaries == 3 && d.snapshot().delayUs == 100);
    assert(d.snapshot().worstUs == 373);
  }
  Timing d;
  d.observe(1791590399LL, 900000, 1, true, 2); // UTC date boundary.
  d.observe(1791590400LL, 100, 100101, true, 2);
  assert(d.snapshot().missedBoundaries == 0 && d.snapshot().delayUs == 100);
  d.observe(1791590401LL, 200, 100, true, 2); // Invalid backwards monotonic source.
  assert(d.snapshot().delayUs == 0 && d.snapshot().missedBoundaries == 0);
}

void saturation() {
  Timing d;
  d.observe(100, 0, 1000000, true, 2);
  const uint64_t skipped = uint64_t(UINT32_MAX) + 10ULL;
  d.observe(100 + int64_t(skipped), 100, 1000000 + skipped * 1000000 + 100, true, 2);
  assert(d.snapshot().missedBoundaries == UINT32_MAX);
  d.observe(105 + int64_t(skipped), 200, 6000000 + skipped * 1000000 + 200, true, 2);
  assert(d.snapshot().missedBoundaries == UINT32_MAX);
  assert(d.snapshot().delayUs == 200 && d.snapshot().worstUs == 200);
  d.suspend();
  assert(d.snapshot().missedBoundaries == UINT32_MAX && d.snapshot().delayUs == 0);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  if (!std::strcmp(argv[1], "initial")) initialClock();
  else if (!std::strcmp(argv[1], "steps")) steps();
  else if (!std::strcmp(argv[1], "concurrent")) concurrentStep();
  else if (!std::strcmp(argv[1], "suspend")) suspendAndIdle();
  else if (!std::strcmp(argv[1], "misses")) genuineMisses();
  else if (!std::strcmp(argv[1], "rollovers")) rollovers();
  else if (!std::strcmp(argv[1], "saturation")) saturation();
  else assert(false);
  std::printf("Production timing helper %s passed (%zu fixed bytes)\n", argv[1], sizeof(Timing));
}
'''


TASK_MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <sys/time.h>
#include "RadioReliability.h"
#include "RadioBleArbiter.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define PIN_LED 25
#define PIN_BUZZ 27
#define LOW 0
#define HIGH 1
#define MAX_SCHEDULES 24
#define SN_BPC 6
#define NUM_STATIONS 7
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

struct Sample {
  int64_t second;
  uint32_t usec;
  uint64_t monoUs;
  bool active = true;
  bool paused = false;
  bool fault = false;
  bool timer = true;
  bool trusted = true;
  uint32_t generation = 2;
};
std::vector<Sample> samples;
size_t sampleIndex = 0;
Sample current{};
struct TaskEnded {};
int progressMux=0, radioLogMux=0;
uint64_t radioTaskLastUs=0, radioTimerLastUs=0;
uint32_t radioTimerTicks=0, radioTimerStartTick=0;
std::atomic<bool> radioTimerQualified{true}, radioSafetyMonitorReady{true};
std::atomic<bool> radioFaultLatched{false}, radioPaused{false}, carrierReady{false};
std::atomic<int> istimerstarted{1}, last_station{-1};
std::atomic<uint32_t> radioStackMinimum{UINT32_MAX};
std::atomic<int> radioBoundaryErrorUs{0};
std::atomic<uint32_t> radioBoundaryWorstUs{0}, radioMissedBoundaries{0}, radioProcessedSecond{0};
std::atomic<uint32_t> radioClockStepGeneration{0};
std::atomic<bool> encodingRefreshRequested{false};
RadioBleArbiter radioBleArbiter;
int buzzout=0, ampmod=0, buzzsw=0, tssec=0;
struct tm nowtm{};
int applicable_count=0, applicable_schedules[MAX_SCHEDULES]={};
int radioApplicableCountSnapshot=0, radioApplicableSchedulesSnapshot[MAX_SCHEDULES]={};
void (*makebitpattern)(void) = nullptr;

unsigned ulTaskNotifyTake(bool, unsigned) {
  if (sampleIndex == samples.size()) throw TaskEnded{};
  current = samples[sampleIndex++];
  radioTimerLastUs=current.monoUs; radioTimerTicks+=1000;
  istimerstarted=current.timer; radioFaultLatched=current.fault;
  radioPaused=current.paused; radioClockStepGeneration=current.generation;
  return 1;
}
int64_t esp_timer_get_time() { return int64_t(current.monoUs); }
unsigned uxTaskGetStackHighWaterMark(void*) { return 3000; }
void latchRadioFault(RadioReliabilityFault) { radioFaultLatched=true; }
void radioProcessPauseCommand() {}
void emergencyRfOff() { carrierReady=false; last_station=-1; }
void digitalWrite(int,int) {}
int fakeGetTime(struct timeval* tv, void*) {
  tv->tv_sec=current.second; tv->tv_usec=current.usec; return 0;
}
#define gettimeofday fakeGetTime
bool clockTrusted() { return current.trusted; }
void getlocaltime() {
  time_t second=current.second;
  gmtime_r(&second,&nowtm);
}
void applyCurrentSchedule() {
  if (current.active && current.trusted) {
    assert(radioBleArbiter.requestRf()); last_station=0; carrierReady=true;
  } else {
    carrierReady=false; last_station=-1; radioBleArbiter.releaseRf();
  }
}
void queuePatternLog(int) {}
void queueSecondLog(int) {}
void ampchange() {}
void radioTask(void*);
void runTask() { try {radioTask(nullptr);} catch(const TaskEnded&) {} }
'''


TASK_CASES = r'''
int main(int argc, char** argv) {
  assert(argc == 2);
  if (!std::strcmp(argv[1], "startup")) {
  // Replay the reported startup NTP jump. Idle work and initial RF entry must
  // not contaminate any delay or missed-second diagnostic.
  samples={
    {549,0,549000000,false},
    {1791590127LL,468378,550000000,false,false,false,true,true,4},
    {1791590128LL,900000,551000000,true,false,false,true,true,4},
    {1791590129LL,373,551100373,true,false,false,true,true,4},
  };
  runTask();
  assert(radioMissedBoundaries==0 && radioBoundaryWorstUs==373);
  assert(radioBoundaryErrorUs==373 && radioProcessedSecond==1);
  puts("Actual radioTask startup NTP jump and first RF entry excluded; stable 373us measured");
  } else if (!std::strcmp(argv[1], "pauses")) {
    samples={
      {100,0,1000000},
      {101,373,2000373},
      {102,900000,3900000,true,true}, // Explicit pause.
      {180,990000,81990000},
      {181,200,82000200},
      {182,999999,83999999,false}, // Idle/BLE handoff.
      {191,900000,92900000},
      {192,100,93000100},
      {193,999000,94999000,true,false,true}, // RF fault barrier.
      {200,950000,101950000},
      {201,200,102000200},
      {202,900000,103900000,true,false,false,false}, // Timer unavailable.
      {203,900000,104900000},
      {204,300,105000300},
      {205,900000,106900000,true,true}, // Current delay clears immediately.
    };
    runTask();
    assert(radioMissedBoundaries==0 && radioBoundaryWorstUs==373);
    assert(radioBoundaryErrorUs==0 && radioProcessedSecond==5);
    puts("Actual radioTask pause/fault/timer/idle re-entry excludes deliberate gaps; real worst delay retained");
  } else if (!std::strcmp(argv[1], "steps")) {
    samples={
      {100,0,1000000},
      {101,373,2000373},
      {7302,468378,3000373,true,false,false,true,true,4},
      {7303,100,3532095,true,false,false,true,true,4},
      {103,600000,4532095,true,false,false,true,true,6},
      {104,200,4932295,true,false,false,true,true,6},
      {108,300,8932395,true,false,false,true,true,6},
    };
    runTask();
    assert(radioMissedBoundaries==3 && radioBoundaryWorstUs==373);
    assert(radioBoundaryErrorUs==300 && radioProcessedSecond==4);
    puts("Actual radioTask forward/backward NTP steps excluded; stable 3-second miss still reported");
  } else assert(false);
}
'''


class TimingDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("g++"):
            raise RuntimeError("g++ is required for focused timing fault injection")
        cls.directory = tempfile.TemporaryDirectory(prefix="radioclock-timing-diagnostics-")
        cls.path = Path(cls.directory.name)
        cls.helper = cls.compile("helper", HELPER_DRIVER)
        source = FIRMWARE.read_text()
        cls.task = cls.compile("task", TASK_MOCKS + function(source, "radioTask") + TASK_CASES)

    @classmethod
    def compile(cls, name, code):
        unit, binary = cls.path / (name + ".cpp"), cls.path / name
        unit.write_text(code)
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-I", str(FIRMWARE.parent), str(unit), "-o", str(binary),
        ], check=True)
        return binary

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_helper(self, case):
        result = subprocess.run([str(self.helper), case], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

    def test_initial_ntp_epoch_jump(self):
        self.run_helper("initial")

    def test_forward_backward_and_tiny_accepted_clock_steps(self):
        self.run_helper("steps")

    def test_concurrent_clock_samples_and_generation_rollover(self):
        self.run_helper("concurrent")

    def test_pause_resume_idle_and_rf_reentry(self):
        self.run_helper("suspend")

    def test_real_active_missed_seconds_and_boundary_crossing(self):
        self.run_helper("misses")

    def test_monotonic_rollovers_midnight_and_years_of_uptime(self):
        self.run_helper("rollovers")

    def test_counter_saturation_and_bounded_fixed_state(self):
        self.run_helper("saturation")

    def test_actual_radio_task_hook_reproduces_reported_startup(self):
        self.run_task("startup")

    def test_actual_radio_task_excludes_pause_fault_timer_and_idle_gaps(self):
        self.run_task("pauses")

    def test_actual_radio_task_clock_steps_and_true_misses(self):
        self.run_task("steps")

    def run_task(self, case):
        result = subprocess.run([str(self.task), case], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
