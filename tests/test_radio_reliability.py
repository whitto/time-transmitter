#!/usr/bin/env python3
"""Inject command delays, allocation failure and stalled ISR/task progress.

Compile the actual production functions with native GPIO/RTOS mocks. This
checks decisions and the safe output ordering, not real RF timing or hardware.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware/RadioClock_V4_15/RadioClock_V4_15.ino"


def function(source, name):
    match = re.search(r'^(?:static )?(?:bool|void|int|uint32_t) (?:IRAM_ATTR )?' +
                      name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f"Missing function {name}")
    depth, end = 1, match.end()
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[match.start():end]


MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include "RadioReliability.h"
#include "RadioBleArbiter.h"
#define PIN_RADIO 12
#define PIN_LED 25
#define PIN_BUZZ 26
#define LOW 0
#define HIGH 1
#define OUTPUT 1
#define GPIO_MODE_OUTPUT 1
#define SIG_GPIO_OUT_IDX 256
#define IRAM_ATTR
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define CONFIG_FREERTOS_UNICORE 1
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
#define portYIELD_FROM_ISR() ((void)0)
using gpio_num_t = int;
using BaseType_t = int;
using TaskHandle_t = void*;
struct hw_timer_t {} timerStorage;
hw_timer_t* tm0 = nullptr;
TaskHandle_t radioTaskHandle = reinterpret_cast<void*>(1), safetyMonitorTaskHandle = nullptr;
std::atomic<int> istimerstarted{0};
std::atomic<bool> radioTimerQualified{false}, carrierReady{false}, rfSilenceFailed{false};
std::atomic<bool> radioSafetyMonitorReady{true}, radioFaultLatched{false};
std::atomic<uint8_t> radioFaultReason{RADIO_FAULT_NONE};
std::atomic<uint32_t> radioCommandTimeouts{0}, radioTimerRetries{0};
std::atomic<bool> radioApPauseOwned{false}, radioPaused{false}, encodingRefreshRequested{false};
std::atomic<bool> radioResumeRecoveryPending{false};
std::atomic<uint32_t> loopStackMinimum{UINT32_MAX}, radioStackMinimum{UINT32_MAX}, monitorStackMinimum{UINT32_MAX};
RadioPauseControl radioPauseControl;
RadioBleArbiter radioBleArbiter;
RadioTimerRetryBudget radioTimerRetryBudget;
RadioBoundaryDiagnostics timingDiagnostics;
std::atomic<int> radioBoundaryErrorUs{0};
int radioCommandMux = 0, progressMux = 0;
uint64_t applicationLoopLastUs=1, radioTaskLastUs=1, radioTimerLastUs=1;
uint32_t radioTimerTicks=0, radioTimerStartTick=0;
uint64_t radioTimerRetryUs=0, radioTimerRecoveryStartedUs=0;
uint32_t radioTimerRecoveryTick=0;
uint64_t radioResumeRetryUs=0;
uint32_t radioResumeRetries=0;
int ampc=0,tssec=0,ampmod=0,buzzout=0;
int64_t lastBoundary=-1;
int lastMinute=-1,lastSecond=-1,lastSlot=-1;
uint64_t lastStackCheckUs=0;
uint64_t nowUs=1;
std::function<void()> afterDelay, afterSafetyDelay, duringDutyWrite;
bool dutyOk=true, detachOk=true, timerAllocationOk=true, taskAllocationOk=true;
uint32_t duty=0, dutyWrites=0, timerAllocations=0, timerStops=0, gpioOverrides=0, refreshes=0, monitorAllocations=0;
bool gpioRfConnected=true;
int safetyDelays=0, maximumSafetyDelays=0;
struct StopSafety {};
struct Restarted {};
struct SerialMock { void println(const char*) {} } Serial;
int64_t esp_timer_get_time() { return (int64_t)nowUs; }
void delay(unsigned ms) { nowUs += uint64_t(ms)*1000; if(afterDelay) afterDelay(); }
void vTaskDelay(unsigned ms) {
  if (++safetyDelays > maximumSafetyDelays) throw StopSafety{};
  nowUs += uint64_t(ms)*1000;
  if(afterSafetyDelay) afterSafetyDelay();
}
unsigned uxTaskGetStackHighWaterMark(void*) { return 1024; }
void xTaskNotifyGive(void*) {}
void xTaskNotifyStateClear(void*) {}
void ulTaskNotifyValueClear(void*, unsigned) {}
unsigned ulTaskNotifyTake(bool, unsigned) { return 1; }
void vTaskNotifyGiveFromISR(void*, BaseType_t*) {}
void gpio_set_level(int, int value) { assert(value==0); }
void gpio_set_direction(int, int mode) { assert(mode==GPIO_MODE_OUTPUT); }
void esp_rom_gpio_connect_out_signal(int, int signal, bool, bool) {
  assert(signal==SIG_GPIO_OUT_IDX); gpioRfConnected=false; ++gpioOverrides;
}
void esp_restart() { throw Restarted{}; }
bool ledcWrite(int, unsigned value) {
  ++dutyWrites;
  if(duringDutyWrite) duringDutyWrite();
  if(!dutyOk) return false;
  duty=value;
  return true;
}
bool ledcDetach(int) { return detachOk; }
void pinMode(int,int) {}
void digitalWrite(int,int) {}
hw_timer_t* timerBegin(unsigned) { ++timerAllocations; return timerAllocationOk?&timerStorage:nullptr; }
void timerAttachInterrupt(hw_timer_t*,void(*)()) {}
void timerAlarm(hw_timer_t*,unsigned,bool,unsigned) {}
void timerDetachInterrupt(hw_timer_t*) {}
void timerEnd(hw_timer_t*) { ++timerStops; }
BaseType_t xTaskCreate(void(*)(void*), const char*, int, void*, int, TaskHandle_t* handle) {
  ++monitorAllocations;
  *handle=taskAllocationOk?reinterpret_cast<void*>(2):nullptr;
  return taskAllocationOk?pdPASS:0;
}
void radioRequestRefresh() { encodingRefreshRequested=true; ++refreshes; }
void stoptimer();
void onTimer();
void radioProcessPauseCommand();
static void emergencyRfOff();
uint32_t applicationLoopAgeMs();
uint32_t radioTaskAgeMs();
void reset() {
  if(radioBleArbiter.bleOwned()) radioBleArbiter.finishBleShutdown(true);
  radioBleArbiter.releaseRf();
  radioPauseControl=RadioPauseControl{};
  radioTimerRetryBudget=RadioTimerRetryBudget{};
  radioApPauseOwned=radioPaused=radioFaultLatched=false;
  radioResumeRecoveryPending=false;
  radioFaultReason=RADIO_FAULT_NONE;
  radioCommandTimeouts=radioTimerRetries=0;
  radioResumeRetries=0;
  radioResumeRetryUs=radioTimerRetryUs=radioTimerRecoveryStartedUs=0;
  radioTimerTicks=radioTimerStartTick=radioTimerRecoveryTick=0;
  istimerstarted=0;radioTimerQualified=false;tm0=nullptr;
  radioSafetyMonitorReady=true;
  safetyMonitorTaskHandle=nullptr;
  nowUs=1;applicationLoopLastUs=radioTaskLastUs=radioTimerLastUs=1;
  lastStackCheckUs=0;afterDelay=afterSafetyDelay=duringDutyWrite=nullptr;
  dutyOk=detachOk=timerAllocationOk=taskAllocationOk=true;
  carrierReady=rfSilenceFailed=false;
  duty=dutyWrites=timerAllocations=timerStops=gpioOverrides=refreshes=monitorAllocations=0;
  gpioRfConnected=true;maximumSafetyDelays=safetyDelays=0;
}
bool physicalCarrierOn() { return gpioRfConnected && duty!=0; }
void runSafety(int ticks) {
  maximumSafetyDelays=ticks;safetyDelays=0;
  try { safetyMonitorTask(nullptr); } catch(const StopSafety&) {}
}
'''

COMMAND_CASES = r'''
int main() {
  reset();
  assert(!radioSetPaused(true)); // The RF task did not consume its notification.
  assert(radioCommandTimeoutCount()==1 && !radioPauseControl.committed());
  radioProcessPauseCommand(); // Delayed task runs after HTTP caller rejected.
  assert(!radioPaused && !radioPauseControl.committed());

  reset();carrierReady=true;duty=512;dutyOk=detachOk=false;
  afterDelay=[] { radioProcessPauseCommand(); };
  assert(!radioSetPaused(true)); // Applied, but physical silence never acknowledged.
  dutyOk=detachOk=true;
  radioProcessPauseCommand();
  assert(!radioPaused && !radioPauseControl.committed());

  reset();afterDelay=[] { radioProcessPauseCommand(); };
  RadioPauseLease temporary;
  assert(radioAcquirePause(temporary) && radioPaused);
  assert(radioReleasePause(temporary) && !radioPaused);
  assert(!radioReleasePause(temporary)); // Single release only.

  reset();afterDelay=[] { radioProcessPauseCommand(); };
  radioApPauseOwned=true;
  RadioPauseLease whileAp;
  assert(radioAcquirePause(whileAp));
  assert(radioReleasePause(whileAp) && radioPaused); // AP independent owner remains.
  radioApPauseOwned=false;radioProcessPauseCommand();
  assert(!radioPaused);

  reset();afterDelay=[] { radioProcessPauseCommand(); };
  assert(radioSetPaused(true)); // Existing independent intentional pause.
  RadioPauseLease nested;
  assert(radioAcquirePause(nested) && nested.wasPaused);
  assert(radioReleasePause(nested) && radioPaused && radioPauseControl.committed());

  reset();nowUs=uint64_t(UINT32_MAX)*1000ULL-500000ULL;
  afterDelay=[] { radioProcessPauseCommand(); };
  assert(radioSetPaused(true)); // Command deadline spans 32-bit millis rollover.
  assert(radioSetPaused(false) && !radioPaused);

  reset();
  auto stale=radioPauseControl.submit(true,nowUs,2000000);
  auto latest=radioPauseControl.submit(false,nowUs,2000000);
  assert(!radioPauseControl.cancel(stale));
  radioProcessPauseCommand();
  assert(radioPauseControl.acknowledged(latest) && !radioPaused);

  reset();afterDelay=[] { radioProcessPauseCommand(); };
  RadioPauseLease resumeFailure;
  assert(radioAcquirePause(resumeFailure));
  afterDelay=nullptr;
  assert(!radioReleasePause(resumeFailure) && radioResumeRecoveryPending);
  assert(radioPauseControl.committed()); // Cancelled resume keeps a safe barrier.
  afterDelay=[] { radioProcessPauseCommand(); };
  nowUs=radioResumeRetryUs;serviceRadioReliability();
  assert(!radioResumeRecoveryPending && !radioPaused);
  puts("Actual RF command tests passed: late/rejected generations, silence failure, AP/intentional leases, rollover and checked resume recovery");
}
'''

TIMER_CASES = r'''
int main() {
  reset();timerAllocationOk=false;duty=512;
  assert(!starttimer() && radioFaultActive() && radioFaultCode()==RADIO_FAULT_TIMER_ALLOCATION);
  assert(!istimerstarted && !radioTimerQualified && !physicalCarrierOn());
  radioRequestRefresh();radioSafetyTick();
  assert(!physicalCarrierOn()); // Refresh alone cannot enable a carrier.
  nowUs=radioTimerRetryUs-1;serviceRadioReliability();assert(timerAllocations==1);
  for(unsigned n=1;n<=3;++n) {
    nowUs=radioTimerRetryUs;radioTaskLastUs=nowUs;
    serviceRadioReliability();assert(radioTimerRetryCount()==n && timerAllocations==n+1);
  }
  nowUs+=10000000;radioTaskLastUs=nowUs;serviceRadioReliability();
  assert(timerAllocations==4 && radioFaultActive()); // No infinite allocation retry.

  reset();timerAllocationOk=false;assert(!starttimer());
  nowUs=radioTimerRetryUs;radioTaskLastUs=nowUs;timerAllocationOk=true;
  serviceRadioReliability();
  assert(istimerstarted && radioFaultActive() && !radioTimerQualified);
  nowUs+=1000;onTimer();radioSafetyTick();serviceRadioReliability();
  assert(radioFaultActive() && !radioTimerQualified); // One ISR is insufficient.
  nowUs+=1000;onTimer();radioSafetyTick();serviceRadioReliability();
  assert(!radioFaultActive() && radioTimerOperational() && encodingRefreshRequested);

  reset();assert(starttimer());radioSafetyTick();
  assert(!radioTimerQualified && !physicalCarrierOn());
  nowUs+=300001;radioSafetyTick(); // Timer exists, but no actual interrupts.
  assert(radioFaultActive() && radioFaultCode()==RADIO_FAULT_TIMER_STALLED);
  assert(!physicalCarrierOn());

  reset();taskAllocationOk=false;duty=512;
  assert(!startSafetyMonitor() && radioFaultCode()==RADIO_FAULT_MONITOR_CREATION);
  assert(!physicalCarrierOn());
  reset();assert(startSafetyMonitor());
  uint64_t bootHeartbeat=applicationLoopLastUs;
  nowUs+=1000000;
  assert(startSafetyMonitor() && monitorAllocations==1);
  assert(applicationLoopLastUs==bootHeartbeat); // Idempotence cannot mask a stuck boot.
  puts("Actual timer tests passed: allocation failure, bounded retries, absent interrupts, two-tick recovery and monitor creation failure");
}
'''

TIMER_BUDGET_CASES = r'''
// Simulate the 1 kHz IRQ counter/heartbeat in bounded batches. Qualification
// and recovery still use the production ISR and actual task/service functions.
void healthyProgress(uint64_t durationUs) {
  radioTaskLastUs=radioTimerLastUs=nowUs;
  serviceRadioReliability();
  while(durationUs) {
    uint64_t step=durationUs>30000000ULL?30000000ULL:durationUs;
    nowUs+=step; durationUs-=step;
    radioTimerTicks+=static_cast<uint32_t>(step/1000ULL);
    radioTaskLastUs=radioTimerLastUs=nowUs;
    serviceRadioReliability();
  }
}
void qualifyTimer() {
  nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
  nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
  assert(radioTimerOperational());
}
void stallTimer() {
  nowUs+=RadioProgressPolicy::TimerStallUs+1;
  radioTaskLastUs=nowUs;
  latchRadioFault(RADIO_FAULT_TIMER_STALLED);
  assert(radioFaultActive() && !physicalCarrierOn());
  nowUs+=RadioProgressPolicy::RetrySpacingUs;
  radioTaskLastUs=nowUs;
  serviceRadioReliability();
}
int main() {
  reset(); assert(starttimer()); qualifyTimer();
  for(unsigned episode=1;episode<=4;++episode) {
    healthyProgress(7ULL*86400ULL*1000000ULL);
    assert(radioTimerRetryBudgetUsed()==0);
    const unsigned allocations=timerAllocations;
    stallTimer();
    assert(timerAllocations==allocations+1);
    assert(radioTimerRetryCount()==episode && radioTimerRetryBudgetUsed()==1);
    qualifyTimer();
  }
  assert(radioTimerRetryCount()==4); // The old lifetime cap stranded episode 4.

  reset(); assert(starttimer()); qualifyTimer();
  for(unsigned episode=1;episode<=3;++episode) {
    stallTimer(); qualifyTimer();
    healthyProgress(5ULL*60ULL*1000000ULL);
    assert(radioTimerRetryBudgetUsed()==episode);
  }
  const unsigned rapidAllocations=timerAllocations;
  stallTimer();
  assert(timerAllocations==rapidAllocations && radioTimerRetryCount()==3);
  healthyProgress(7ULL*86400ULL*1000000ULL);
  assert(radioFaultActive() && !physicalCarrierOn());
  assert(timerAllocations==rapidAllocations && radioTimerRetryBudgetUsed()==3);

  reset(); timerAllocationOk=false; assert(!starttimer());
  timerAllocationOk=true;
  for(unsigned attempt=1;attempt<=3;++attempt) {
    nowUs=radioTimerRetryUs; radioTaskLastUs=nowUs;
    serviceRadioReliability();
    assert(radioTimerRetryBudgetUsed()==attempt && istimerstarted);
    nowUs+=1000001ULL; radioTaskLastUs=nowUs;
    serviceRadioReliability(); // Allocation succeeds, but no IRQ qualification.
    assert(radioFaultActive() && !radioTimerOperational());
  }
  const unsigned unqualifiedAllocations=timerAllocations;
  nowUs+=7ULL*86400ULL*1000000ULL; radioTaskLastUs=nowUs;
  serviceRadioReliability();
  assert(timerAllocations==unqualifiedAllocations && radioTimerRetryCount()==3);

  reset(); nowUs=uint64_t(UINT32_MAX)*1000ULL-600000000ULL;
  radioTaskLastUs=radioTimerLastUs=nowUs;
  radioTimerTicks=UINT32_MAX-1000000;
  assert(starttimer()); qualifyTimer(); stallTimer(); qualifyTimer();
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs-1000);
  assert(radioTimerRetryBudgetUsed()==1);
  healthyProgress(1000);
  assert(nowUs>uint64_t(UINT32_MAX)*1000ULL);
  assert(radioTimerTicks<1000000); // Both millis and the ISR count rolled over.
  assert(radioTimerRetryBudgetUsed()==0 && radioTimerRetryCount()==1);

  reset(); assert(starttimer()); qualifyTimer(); stallTimer(); qualifyTimer();
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  nowUs+=RadioProgressPolicy::TimerStallUs+1;
  radioTaskLastUs=nowUs; serviceRadioReliability(); // No fresh IRQ: break health.
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  assert(radioTimerRetryBudgetUsed()==1);
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  assert(radioTimerRetryBudgetUsed()==0);

  reset(); assert(starttimer()); qualifyTimer(); stallTimer(); qualifyTimer();
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  nowUs+=RadioProgressPolicy::MaximumHealthyObservationGapUs+1;
  radioTaskLastUs=radioTimerLastUs=nowUs;
  radioTimerTicks+=70001; serviceRadioReliability(); // Fresh now, unobserved gap.
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  assert(radioTimerRetryBudgetUsed()==1);
  healthyProgress(RadioProgressPolicy::TimerHealthyReplenishUs/2);
  assert(radioTimerRetryBudgetUsed()==0);

  reset(); assert(starttimer()); qualifyTimer(); stallTimer(); qualifyTimer();
  radioTaskLastUs=radioTimerLastUs=nowUs;
  serviceRadioReliability();
  for(unsigned minute=0;minute<31;++minute) {
    nowUs+=60000000ULL; radioTaskLastUs=radioTimerLastUs=nowUs;
    serviceRadioReliability(); // Faked fresh timestamps alone are insufficient.
  }
  assert(radioTimerRetryBudgetUsed()==1);

  reset(); timerAllocationOk=false; assert(!starttimer());
  radioTimerRetries=UINT32_MAX;
  nowUs=radioTimerRetryUs; radioTaskLastUs=nowUs; serviceRadioReliability();
  assert(radioTimerRetryCount()==UINT32_MAX && radioTimerRetryBudgetUsed()==1);
  puts("Actual retry budget passed: week-apart recovery, bounded rapid faults, IRQ qualification failure, continuous health, observation gaps, IRQ/millis rollover and saturated lifetime diagnostics");
}
'''

PROGRESS_CASES = r'''
int main() {
  reset();istimerstarted=1;duty=512;
  afterSafetyDelay=[] { applicationLoopLastUs=radioTaskLastUs=nowUs; };
  runSafety(4); // Radio/loop run; only the ISR has silently stopped.
  assert(radioFaultCode()==RADIO_FAULT_TIMER_STALLED && !physicalCarrierOn());

  reset();istimerstarted=1;duty=512;
  afterSafetyDelay=[] { applicationLoopLastUs=radioTimerLastUs=nowUs; };
  runSafety(6);
  assert(radioFaultCode()==RADIO_FAULT_TASK_STALLED && !physicalCarrierOn());
  bool restarted=false;
  try { runSafety(30); } catch(const Restarted&) { restarted=true; }
  assert(restarted); // Independent monitor recovers a genuinely blocked RF task.

  reset();istimerstarted=1;
  afterSafetyDelay=[] { radioTaskLastUs=radioTimerLastUs=nowUs; };
  nowUs=47000001;runSafety(1); // Legitimate 45-second BLE+2-second unwind is allowed.
  assert(!radioFaultActive());
  nowUs=71000001;restarted=false;
  try { runSafety(1); } catch(const Restarted&) { restarted=true; }
  assert(restarted && !physicalCarrierOn());

  reset();nowUs=uint64_t(UINT32_MAX)*1000ULL+86400000000ULL;
  applicationProgressCheckpoint();
  assert(applicationLoopAgeMs()==0 && applicationLoopStackMinimum()==1024);
  assert(RadioProgressPolicy::elapsed(nowUs,nowUs-500001,500000));
  assert(!RadioProgressPolicy::elapsed(nowUs,nowUs+1,500000));
  puts("Actual progress monitor tests passed: stopped ISR, stalled RF task, bounded BLE allowance, stalled loop restart and long uptime");
}
'''


class RadioReliabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FIRMWARE.read_text()

    def unit(self):
        names = ['silenceRfCarrier', 'emergencyRfOff', 'latchRadioFault',
                 'applicationForceSafeRestart', 'radioFaultActive', 'radioFaultCode',
                 'radioTimerRetryCount', 'radioTimerRetryBudgetUsed',
                 'radioCommandTimeoutCount', 'radioTimerOperational',
                 'progressAgeMs', 'applicationLoopAgeMs', 'radioTaskAgeMs', 'radioTimerAgeMs',
                 'applicationLoopStackMinimum', 'radioTaskStackMinimum', 'safetyMonitorStackMinimum',
                 'applicationProgressCheckpoint', 'radioProcessPauseCommand', 'radioSetPaused',
                 'radioAcquirePause', 'radioReleasePause', 'safetyMonitorTask', 'startSafetyMonitor',
                 'onTimer', 'starttimer', 'stoptimer', 'serviceRadioReliability']
        # Production task prelude includes ISR qualification and fault barrier.
        commands = function(self.source, 'radioTask').split('for (;;) {', 1)[1]
        commands = commands.split('// Derive the frame phase', 1)[0]
        task_tick = '\nvoid radioSafetyTick(){for(int tick=0;tick<1;++tick){' + commands + '}}\n'
        # Some cases deliberately use only one branch of the shared fixture.
        unused_monitor = '\nvoid useMonitorSymbol(){(void)&startSafetyMonitor;}\n'
        return MOCKS.replace('void runSafety(int ticks) {', 'static void safetyMonitorTask(void*);\nvoid runSafety(int ticks) {') + '\n' + '\n'.join(
            function(self.source, name) for name in names) + task_tick + unused_monitor

    def compile_and_run(self, cases, sanitize=False):
        self.assertIsNotNone(shutil.which('g++'))
        with tempfile.TemporaryDirectory(prefix='radio-reliability-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(self.unit() + cases)
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)]
            if sanitize:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_actual_pause_generations_and_leases(self):
        self.compile_and_run(COMMAND_CASES)

    def test_actual_timer_failure_and_recovery(self):
        setup = function(self.source, 'setup')
        self.assertLess(setup.index('startSafetyMonitor()'), setup.index('initBluetoothSync()'))
        self.compile_and_run(TIMER_CASES)

    def test_actual_independent_progress_monitor(self):
        self.compile_and_run(PROGRESS_CASES)

    def test_actual_replenishable_timer_budget(self):
        self.compile_and_run(TIMER_BUDGET_CASES)

    def test_actual_functions_with_sanitizers(self):
        self.compile_and_run(COMMAND_CASES, sanitize=True)
        self.compile_and_run(TIMER_CASES, sanitize=True)
        self.compile_and_run(PROGRESS_CASES, sanitize=True)
        self.compile_and_run(TIMER_BUDGET_CASES, sanitize=True)


if __name__ == '__main__':
    unittest.main()
