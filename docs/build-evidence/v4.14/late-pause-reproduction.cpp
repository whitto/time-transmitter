
#include <atomic>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include "firmware/RadioClock_V4_14/RadioBleArbiter.h"
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

void* radioTaskHandle=reinterpret_cast<void*>(1);
void xTaskNotifyGive(void*){}
void delay(unsigned ms){clockMillis+=ms;}
struct SerialMock{void println(const char*){}}Serial;
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
}void radioCommandTick() {
static bool pauseSilencePending = false;
for (int tick=0;tick<1;++tick) {

    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (radioPauseRequested.exchange(false)) {
      radioPaused = true;
      bool silenced = silenceRfCarrier();
      pauseSilencePending = !silenced;
      digitalWrite(PIN_LED, LOW);
      digitalWrite(PIN_BUZZ, LOW);
      buzzout = 0;
      ampmod = 0;
      radioPauseAck = silenced;
    }
    if (radioResumeRequested.exchange(false)) {
      pauseSilencePending = false;
      radioPaused = false;
      encodingRefreshRequested = true;
      radioPauseAck = true;
    }
    // Paused means quiescent: no schedule reads, frame regeneration or RF
    // writes. Web/config code relies on this acknowledgement as a barrier.
    if (radioPaused) {
      if (silenceRfCarrier() && pauseSilencePending) {
        pauseSilencePending = false;
        radioPauseAck = true;
      }
      digitalWrite(PIN_LED, LOW);
      digitalWrite(PIN_BUZZ, LOW);
      buzzout = 0;
      ampmod = 0;
      continue;
    }

}
}bool radioSetPaused(bool pause) {
  if (!radioTaskHandle) { radioPaused = pause; return true; }
  radioPauseAck = false;
  if (pause) radioPauseRequested = true; else radioResumeRequested = true;
  xTaskNotifyGive(radioTaskHandle);
  uint32_t started = millis();
  while (!radioPauseAck && millis() - started < 2000) delay(1);
  if (!radioPauseAck) Serial.println("ERROR: radio command acknowledgement timed out");
  return radioPauseAck && radioPaused == pause;
}
int main(){reset();transmitting();clockMillis=100;
bool acknowledged=radioSetPaused(true);assert(!acknowledged && !radioPaused && radioPauseRequested);
// The HTTP schedule/timezone callers return an error here without a cancellation/resume command.
radioCommandTick();assert(radioPaused && !radioResumeRequested);
// Subsequent timer service never resumes without another user/NTP command.
for(int tick=0;tick<5;++tick)radioCommandTick();assert(radioPaused);
puts("CONFIRMED: a timed-out RF pause can execute late and leave RF paused indefinitely when the rejecting caller supplies no matching resume");}
