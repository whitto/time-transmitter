
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
static void safetyMonitorTask(void*);
void runSafety(int ticks) {
  maximumSafetyDelays=ticks;safetyDelays=0;
  try { safetyMonitorTask(nullptr); } catch(const StopSafety&) {}
}

static bool silenceRfCarrier(void) {
  if (!carrierReady || ledcWrite(PIN_RADIO, 0)) {
    rfSilenceFailed = false;
    return true;
  }
  // A failed duty update must not hand the pin to BLE with residual PWM.
  if (ledcDetach(PIN_RADIO)) {
    carrierReady = false;
    pinMode(PIN_RADIO, OUTPUT);
    digitalWrite(PIN_RADIO, LOW);
    rfSilenceFailed = false;
    return true;
  }
  rfSilenceFailed = true;
  return false; // Keep RF ownership: BLE initialization remains blocked.
}
static void emergencyRfOff(void) {
  // GPIO matrix override does not allocate or take LEDC's driver lock. Setting
  // the output latch before detaching the matrix prevents a transient high.
  gpio_set_level((gpio_num_t)PIN_RADIO, 0);
  esp_rom_gpio_connect_out_signal(PIN_RADIO, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)PIN_RADIO, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_RADIO, 0);
}
static void latchRadioFault(RadioReliabilityFault reason) {
  // Recheck the live heartbeat while holding the same lock used to finish
  // recovery. A stale monitor snapshot must not turn recovered RF off again.
  portENTER_CRITICAL(&progressMux);
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  if ((reason == RADIO_FAULT_TIMER_STALLED &&
       !RadioProgressPolicy::elapsed(nowUs, radioTimerLastUs, RadioProgressPolicy::TimerStallUs)) ||
      (reason == RADIO_FAULT_TASK_STALLED &&
       !RadioProgressPolicy::elapsed(nowUs, radioTaskLastUs, RadioProgressPolicy::RadioStallUs))) {
    portEXIT_CRITICAL(&progressMux);
    return;
  }
  radioFaultReason = reason;
  radioFaultLatched = true;
  emergencyRfOff();
  portEXIT_CRITICAL(&progressMux);
}
void applicationForceSafeRestart(void) {
  latchRadioFault(RADIO_FAULT_LOOP_STALLED);
  emergencyRfOff();
  esp_restart(); // Last-resort progress recovery; no periodic flash log/dump.
}
bool radioFaultActive(void) { return radioFaultLatched.load(); }
int radioFaultCode(void) { return radioFaultReason.load(); }
uint32_t radioTimerRetryCount(void) { return radioTimerRetries.load(); }
uint32_t radioCommandTimeoutCount(void) { return radioCommandTimeouts.load(); }
bool radioTimerOperational(void) { return istimerstarted && radioTimerQualified && !radioFaultLatched; }
static uint32_t progressAgeMs(uint64_t lastUs) {
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  if (!lastUs || nowUs < lastUs) return UINT32_MAX;
  uint64_t ageMs = (nowUs - lastUs) / 1000ULL;
  return ageMs > UINT32_MAX ? UINT32_MAX : (uint32_t)ageMs;
}
uint32_t applicationLoopAgeMs(void) {
  portENTER_CRITICAL(&progressMux);
  uint64_t last = applicationLoopLastUs;
  portEXIT_CRITICAL(&progressMux);
  return progressAgeMs(last);
}
uint32_t radioTaskAgeMs(void) {
  portENTER_CRITICAL(&progressMux);
  uint64_t last = radioTaskLastUs;
  portEXIT_CRITICAL(&progressMux);
  return progressAgeMs(last);
}
uint32_t radioTimerAgeMs(void) {
  portENTER_CRITICAL(&progressMux);
  uint64_t last = radioTimerLastUs;
  portEXIT_CRITICAL(&progressMux);
  return progressAgeMs(last);
}
uint32_t applicationLoopStackMinimum(void) { uint32_t n=loopStackMinimum.load(); return n==UINT32_MAX?0:n; }
uint32_t radioTaskStackMinimum(void) { uint32_t n=radioStackMinimum.load(); return n==UINT32_MAX?0:n; }
uint32_t safetyMonitorStackMinimum(void) { uint32_t n=monitorStackMinimum.load(); return n==UINT32_MAX?0:n; }
void applicationProgressCheckpoint(void) {
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&progressMux);
  applicationLoopLastUs = nowUs;
  portEXIT_CRITICAL(&progressMux);
  static uint64_t lastStackCheckUs = 0;
  if (nowUs - lastStackCheckUs >= 1000000ULL) {
    lastStackCheckUs = nowUs;
    uint32_t available = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (available < loopStackMinimum) loopStackMinimum = available;
  }
}
void radioProcessPauseCommand(void) {
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&radioCommandMux);
  uint32_t generation = radioPauseControl.pendingGeneration(nowUs);
  bool pause = radioPauseControl.desired(nowUs) || radioApPauseOwned || radioFaultLatched;
  bool wasPaused = radioPaused.exchange(pause);
  portEXIT_CRITICAL(&radioCommandMux);
  bool completed = !pause || radioFaultLatched || silenceRfCarrier();
  if (!pause && wasPaused) encodingRefreshRequested = true;
  if (pause) {
    digitalWrite(PIN_LED, LOW);
    digitalWrite(PIN_BUZZ, LOW);
    buzzout = 0;
    ampmod = 0;
  }
  if (completed && generation) {
    portENTER_CRITICAL(&radioCommandMux);
    (void)radioPauseControl.acknowledge(generation, (uint64_t)esp_timer_get_time());
    portEXIT_CRITICAL(&radioCommandMux);
  }
}
bool radioSetPaused(bool pause) {
  const uint64_t started = (uint64_t)esp_timer_get_time();
  const uint64_t deadline = started + 2000000ULL;
  portENTER_CRITICAL(&radioCommandMux);
  if (!radioTaskHandle) {
    radioPauseControl.commitWithoutTask(pause);
    radioPaused = pause || radioApPauseOwned || radioFaultLatched;
    portEXIT_CRITICAL(&radioCommandMux);
    return true;
  }
  const uint32_t generation = radioPauseControl.submit(pause, started, 2000000ULL);
  portEXIT_CRITICAL(&radioCommandMux);
  xTaskNotifyGive(radioTaskHandle);
  for (;;) {
    portENTER_CRITICAL(&radioCommandMux);
    const bool acknowledged = radioPauseControl.acknowledged(generation);
    portEXIT_CRITICAL(&radioCommandMux);
    if (acknowledged) return true;
    if ((uint64_t)esp_timer_get_time() >= deadline) {
      // Recheck acknowledgement under the same lock as cancellation. A late
      // RF tick can no longer apply/commit this rejected request generation.
      portENTER_CRITICAL(&radioCommandMux);
      const bool completed = radioPauseControl.acknowledged(generation);
      if (!completed) radioPauseControl.cancel(generation);
      portEXIT_CRITICAL(&radioCommandMux);
      if (completed) return true;
      ++radioCommandTimeouts;
      encodingRefreshRequested = true;
      xTaskNotifyGive(radioTaskHandle);
      Serial.println("ERROR: radio command timed out; pending generation cancelled");
      return false;
    }
    delay(1);
  }
}
bool radioAcquirePause(RadioPauseLease &lease) {
  if (lease.acquired) return false;
  portENTER_CRITICAL(&radioCommandMux);
  lease.wasPaused = radioPauseControl.committed();
  portEXIT_CRITICAL(&radioCommandMux);
  if (!radioSetPaused(true)) return false;
  lease.acquired = true;
  return true;
}
bool radioReleasePause(RadioPauseLease &lease) {
  if (!lease.acquired) return false;
  lease.acquired = false;
  if (lease.wasPaused) return true; // Existing intentional owner retains pause.
  if (radioSetPaused(false)) {
    radioResumeRecoveryPending = false;
    radioResumeRetries = 0;
    return true;
  }
  // Safe pause remains committed. A bounded service retry restores only this
  // temporary owner; the independent AP owner is never cleared here.
  radioResumeRecoveryPending = true;
  radioResumeRetries = 0;
  radioResumeRetryUs = (uint64_t)esp_timer_get_time() + 1000000ULL;
  return false;
}
static void safetyMonitorTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!radioSafetyMonitorReady) continue;
    uint64_t nowUs = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&progressMux);
    uint64_t loopLast = applicationLoopLastUs;
    uint64_t radioLast = radioTaskLastUs;
    uint64_t timerLast = radioTimerLastUs;
    portEXIT_CRITICAL(&progressMux);
    uint32_t available = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (available < monitorStackMinimum) monitorStackMinimum = available;
    if (loopLast && RadioProgressPolicy::elapsed(nowUs, loopLast, RadioProgressPolicy::LoopRestartUs)) {
      if (applicationLoopAgeMs() > RadioProgressPolicy::LoopRestartUs / 1000ULL)
        applicationForceSafeRestart();
      continue;
    }
    if (!radioTaskHandle) continue;
    if (radioLast && RadioProgressPolicy::elapsed(nowUs, radioLast, RadioProgressPolicy::RadioStallUs)) {
      latchRadioFault(RADIO_FAULT_TASK_STALLED);
      if (radioTaskAgeMs() > RadioProgressPolicy::RadioRestartUs / 1000ULL) {
        emergencyRfOff();
        esp_restart();
      }
    } else if (istimerstarted && timerLast &&
               RadioProgressPolicy::elapsed(nowUs, timerLast, RadioProgressPolicy::TimerStallUs)) {
      latchRadioFault(RADIO_FAULT_TIMER_STALLED);
    }
  }
}
static bool startSafetyMonitor(void) {
  if (safetyMonitorTaskHandle && radioSafetyMonitorReady) return true;
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&progressMux);
  applicationLoopLastUs = radioTaskLastUs = radioTimerLastUs = nowUs;
  portEXIT_CRITICAL(&progressMux);
  BaseType_t created;
#if CONFIG_FREERTOS_UNICORE
  created = xTaskCreate(safetyMonitorTask, "safetyMonitor", 2048, nullptr, 1,
                        &safetyMonitorTaskHandle);
#else
  created = xTaskCreatePinnedToCore(safetyMonitorTask, "safetyMonitor", 2048, nullptr, 1,
                                    &safetyMonitorTaskHandle, 0);
#endif
  if (created != pdPASS || safetyMonitorTaskHandle == nullptr) {
    safetyMonitorTaskHandle = nullptr;
    latchRadioFault(RADIO_FAULT_MONITOR_CREATION);
    Serial.println("ERROR: RF safety monitor unavailable; carrier left safely off");
    return false;
  }
  radioSafetyMonitorReady = true;
  return true;
}
void IRAM_ATTR onTimer(void)
{
  // 1 kHz hardware-timer tick.
  // The ISR does not generate the RF carrier; LEDC does that in hardware.
  // Keeping the ISR to a single notification call minimizes interrupt
  // latency and timing jitter. The notification is only a wakeup source:
  // if several ticks accumulate they can be coalesced because radioTask
  // derives second/subsecond phase from gettimeofday().
  portENTER_CRITICAL_ISR(&progressMux);
  radioTimerLastUs = (uint64_t)esp_timer_get_time();
  ++radioTimerTicks;
  portEXIT_CRITICAL_ISR(&progressMux);
  BaseType_t hpTaskWoken = pdFALSE;
  if (radioTaskHandle != NULL) {
    vTaskNotifyGiveFromISR(radioTaskHandle, &hpTaskWoken);
  }
  if (hpTaskWoken) portYIELD_FROM_ISR();
}
bool starttimer(void)
{
  if (istimerstarted) stoptimer();
  radioTimerQualified = false;

  ampc = 0;
  tssec = 0;

  // Clear any stale notification count left over from before the timer was
  // stopped, so the radio task doesn't "catch up" on ticks that never
  // actually happened in hardware.
  if (radioTaskHandle != NULL) {
    xTaskNotifyStateClear(radioTaskHandle);
    ulTaskNotifyValueClear(radioTaskHandle, 0xFFFFFFFF);
  }

  // Use a 1 MHz hardware-timer tick and generate the 1 kHz service
  // interrupt with a 1000-tick alarm.  A direct 1 kHz timer clock would
  // require an 80,000 divider on an 80 MHz APB clock, exceeding the
  // ESP32 hardware timer divider range.
  tm0 = timerBegin(1000000);
  if (tm0 == NULL) {
    istimerstarted = 0;
    latchRadioFault(RADIO_FAULT_TIMER_ALLOCATION);
    radioTimerRetryUs = (uint64_t)esp_timer_get_time() + RadioProgressPolicy::RetrySpacingUs;
    Serial.println("ERROR: Failed to create 1 MHz hardware timer");
    return false;
  }
  portENTER_CRITICAL(&progressMux);
  radioTimerStartTick = radioTimerTicks;
  radioTimerLastUs = (uint64_t)esp_timer_get_time();
  portEXIT_CRITICAL(&progressMux);
  timerAttachInterrupt(tm0, &onTimer);
  timerAlarm(tm0, 1000, true, 0);

  istimerstarted = 1;
  return true;
}
void stoptimer(void)
{
  if (tm0 != NULL) {
    timerDetachInterrupt(tm0);
    timerEnd(tm0);
    tm0 = NULL;
  }

  if (carrierReady) ledcWrite(PIN_RADIO, 0);
  istimerstarted = 0;
  radioTimerQualified = false;
  emergencyRfOff();
}
void serviceRadioReliability(void) {
  if (radioResumeRecoveryPending &&
      (uint64_t)esp_timer_get_time() >= radioResumeRetryUs) {
    if (radioResumeRetries >= 3) {
      radioResumeRecoveryPending = false;
      latchRadioFault(RADIO_FAULT_RESUME_TIMEOUT);
    } else {
      ++radioResumeRetries;
      radioResumeRetryUs = (uint64_t)esp_timer_get_time() + 1000000ULL;
      if (radioSetPaused(false)) radioResumeRecoveryPending = false;
    }
  }
  if (!radioFaultLatched || !radioTaskHandle || !radioSafetyMonitorReady) return;
  uint8_t reason = radioFaultReason.load();
  if (reason != RADIO_FAULT_TIMER_ALLOCATION && reason != RADIO_FAULT_TIMER_STALLED &&
      reason != RADIO_FAULT_TASK_STALLED) return;
  uint64_t nowUs = (uint64_t)esp_timer_get_time();
  if (radioTimerRecoveryStartedUs) {
    portENTER_CRITICAL(&progressMux);
    bool responsive = !RadioProgressPolicy::elapsed(nowUs, radioTaskLastUs, 250000ULL);
    bool ticks = radioTimerTicks - radioTimerRecoveryTick >= 2;
    bool fresh = !RadioProgressPolicy::elapsed(nowUs, radioTimerLastUs, RadioProgressPolicy::TimerStallUs);
    if (responsive && ticks && fresh && radioTimerQualified) {
      radioFaultReason = RADIO_FAULT_NONE;
      radioFaultLatched = false;
      radioTimerRecoveryStartedUs = 0;
      portEXIT_CRITICAL(&progressMux);
      radioRequestRefresh();
      Serial.println("RF timer recovered; scheduler resumed after verified interrupts");
      return;
    }
    portEXIT_CRITICAL(&progressMux);
    if (!RadioProgressPolicy::elapsed(nowUs, radioTimerRecoveryStartedUs, 1000000ULL)) return;
    radioTimerRecoveryStartedUs = 0;
    latchRadioFault(RADIO_FAULT_TIMER_STALLED);
    radioTimerRetryUs = nowUs + RadioProgressPolicy::RetrySpacingUs;
    return;
  }
  if (radioTimerRetries >= RadioProgressPolicy::MaximumTimerRetries || nowUs < radioTimerRetryUs ||
      radioTaskAgeMs() > 250) return;
  ++radioTimerRetries;
  radioTimerRetryUs = nowUs + RadioProgressPolicy::RetrySpacingUs;
  stoptimer();
  // The fault barrier has kept radioTask out of LEDC for at least one complete
  // bounded wait. Drop the old channel metadata before reconnecting the pin.
  if (carrierReady && !ledcDetach(PIN_RADIO)) return;
  carrierReady = false;
  portENTER_CRITICAL(&progressMux);
  radioTimerRecoveryTick = radioTimerTicks;
  portEXIT_CRITICAL(&progressMux);
  if (starttimer()) radioTimerRecoveryStartedUs = (uint64_t)esp_timer_get_time();
}
void radioSafetyTick(){for(int tick=0;tick<1;++tick){
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    uint64_t nowUs = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&progressMux);
    radioTaskLastUs = nowUs;
    uint64_t timerLastUs = radioTimerLastUs;
    uint32_t timerTicks = radioTimerTicks;
    uint32_t timerStartTick = radioTimerStartTick;
    portEXIT_CRITICAL(&progressMux);
    if (istimerstarted && timerTicks - timerStartTick >= 2 &&
        !RadioProgressPolicy::elapsed(nowUs, timerLastUs, RadioProgressPolicy::TimerStallUs))
      radioTimerQualified = true;
    if (nowUs - lastStackCheckUs >= 1000000ULL) {
      lastStackCheckUs = nowUs;
      uint32_t available = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
      if (available < radioStackMinimum) radioStackMinimum = available;
    }
    if (istimerstarted && RadioProgressPolicy::elapsed(nowUs, timerLastUs, RadioProgressPolicy::TimerStallUs))
      latchRadioFault(RADIO_FAULT_TIMER_STALLED);
    radioProcessPauseCommand();
    if (radioFaultLatched || !istimerstarted || !radioTimerQualified || !radioSafetyMonitorReady) {
      emergencyRfOff();
      radioBleArbiter.releaseRf();
      lastBoundary = -1; lastMinute = lastSecond = lastSlot = -1;
      continue;
    }
    // Paused means quiescent: no schedule reads, frame regeneration or RF
    // writes. Web/config code relies on this acknowledgement as a barrier.
    if (radioPaused) {
      digitalWrite(PIN_LED, LOW);
      digitalWrite(PIN_BUZZ, LOW);
      buzzout = 0;
      ampmod = 0;
      continue;
    }
    }}

void useMonitorSymbol(){(void)&startSafetyMonitor;}

int main() {
  reset();
  assert(starttimer());
  nowUs += 1000; onTimer(); radioSafetyTick();
  nowUs += 1000; onTimer(); radioSafetyTick();
  assert(radioTimerOperational());
  for (int fault=0; fault<4; ++fault) {
    nowUs += 7ULL*86400ULL*1000000ULL;
    applicationLoopLastUs=radioTaskLastUs=nowUs;
    radioTimerLastUs=nowUs-300001;
    latchRadioFault(RADIO_FAULT_TIMER_STALLED);
    assert(radioFaultActive());
    const uint32_t before=timerAllocations;
    nowUs+=5000000ULL; radioTaskLastUs=nowUs;
    serviceRadioReliability();
    if (fault<3) {
      assert(timerAllocations==before+1);
      nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
      nowUs+=1000; onTimer(); radioSafetyTick(); serviceRadioReliability();
      assert(!radioFaultActive() && radioTimerOperational());
    } else {
      assert(timerAllocations==before && radioFaultActive());
      for(int check=0;check<100;++check){nowUs+=10000000;radioTaskLastUs=nowUs;serviceRadioReliability();}
      assert(timerAllocations==before && radioFaultActive());
    }
  }
  std::printf("Three isolated timer stalls recovered across weeks; fourth transient stall receives no recovery attempt. retries=%u fault=%d.\n", radioTimerRetryCount(),radioFaultCode());
}
