
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
using String = std::string;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
std::atomic<int> ntpsync{1};
uint64_t mockMonoUs = 1000000;
int64_t esp_timer_get_time() { return mockMonoUs; }
uint32_t sdkIntervalMs = 3600000, sdkNextTimeoutMs = 0;
unsigned ntpRestarts = 0;
void sntp_set_sync_interval(uint32_t ms) { sdkIntervalMs = ms < 15000 ? 15000 : ms; }
void sntp_restart() { ++ntpRestarts; sdkNextTimeoutMs = 2000; }
String btTimezoneName = "Australia/Sydney", timezone_name = "UTC";
int btTimeOffsetMinutes = 0, transmission_offset_minutes = 0;
uint8_t txSymbol[60] = {}, txEnvelope[60][10] = {};
constexpr int SN_BPC = 6, SP_M4 = 4;
time_t fakeEpoch = 0;
time_t fakeTime(time_t* out) { if (out) *out = fakeEpoch; return fakeEpoch; }
#define time fakeTime
portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;
uint64_t ntpLastMonoUs = 0;         // esp_timer (monotonic) time at the previous sync, microseconds
int64_t ntpLastEpochUs = 0;         // Wall-clock (epoch) time at the previous sync, microseconds
double ntpDriftPpm = 0.0;           // Smoothed clock drift estimate, parts-per-million
double ntpDriftAbsSecPerHour = 0.0; // Same drift, expressed as seconds/hour (for logging/diagnostics)
// Validity comes ONLY from the SNTP callback, not a plausible clock year.
// A conservative 50 ppm estimate is used until real drift samples exist.
static const double CLOCK_INITIAL_PPM = 50.0;
static const uint32_t CLOCK_MAX_HOLDOVER_SEC = 21600UL;
static const double CLOCK_INITIAL_UNCERTAINTY_SEC = 0.20;
std::atomic<uint32_t> ntpIntervalSec{3600};
static bool clockNtpEverSynced = false;
static uint32_t clockNtpSyncCount = 0;
static bool clockDriftSampleAvailable = false;
uint32_t clockAgeSeconds() {
  portENTER_CRITICAL(&clockMux);
  bool valid=clockNtpEverSynced;
  uint64_t snapshot=ntpLastMonoUs;
  portEXIT_CRITICAL(&clockMux);
  if (!valid || !snapshot) return UINT32_MAX;
  int64_t age = esp_timer_get_time() - (int64_t)snapshot;
  return age < 0 ? UINT32_MAX : (uint32_t)(age / 1000000LL);
}
double clockEstimatedError() {
  uint32_t age = clockAgeSeconds();
  if (age == UINT32_MAX) return 9999.0;
  portENTER_CRITICAL(&clockMux);
  bool measured=clockDriftSampleAvailable;
  double drift=ntpDriftPpm;
  portEXIT_CRITICAL(&clockMux);
  double ppm = measured ? fabs(drift) : CLOCK_INITIAL_PPM;
  return CLOCK_INITIAL_UNCERTAINTY_SEC + ppm * (double)age / 1000000.0;
}
const char *clockConfidence() {
  uint32_t age = clockAgeSeconds();
  if (age == UINT32_MAX || age > CLOCK_MAX_HOLDOVER_SEC || clockEstimatedError() > 0.90) return "Unsynchronized";
  return age <= ntpIntervalSec ? "Synchronized" : "Holdover";
}
bool clockTrusted() { return strcmp(clockConfidence(), "Unsynchronized") != 0; }
#define NTP_INITIAL_INTERVAL_SEC 3600UL   // Resync interval before any drift has been measured yet
#define NTP_MIN_INTERVAL_SEC 15UL         // SNTP/RFC minimum; large drift must not force a 15-minute loss of trust
#define NTP_ERROR_BUDGET_SEC 0.90          // Total error budget, including initial sync uncertainty
#define NTP_RESYNC_MARGIN 0.90            // Request again before either the error or six-hour trust limit
#define NTP_DRIFT_EMA_ALPHA 0.35          // Smoothing factor (0-1) for the drift estimate; higher = reacts faster to new measurements, lower = more stable against noisy single measurements


#define BT_SYNC_SLOT_COUNT 4
#define BT_PREPARE_MINUTES 5
#define BT_WINDOW_AFTER_MINUTES 5
#define WIFI_PRE_SCHEDULE_LEAD_MIN 10
#define WIFI_PERIODIC_WAKE_INTERVAL_MS 21600000UL
#define WIFI_PERIODIC_WAKE_DURATION_MS 600000UL
bool full_time_tx=false,btSyncEnabled[4]={};int btSyncTimes[4]={30,390,750,1110};
struct TimeSchedule{int start_min,end_min;};TimeSchedule schedules[24]={{0,1440}};int schedule_count=1;
uint32_t tick=0;uint32_t millis(){return tick;}
void bluetoothLocalTime(time_t epoch,struct tm&out){gmtime_r(&epoch,&out);}
void configureAdaptiveNtp(void)
{
  portENTER_CRITICAL(&clockMux);
  bool measured=clockDriftSampleAvailable;
  double samplePpm=ntpDriftPpm;
  portEXIT_CRITICAL(&clockMux);
  uint32_t interval = NTP_INITIAL_INTERVAL_SEC;
  if (measured) {
    double seconds = CLOCK_MAX_HOLDOVER_SEC * NTP_RESYNC_MARGIN;
    const double drift = fabs(samplePpm) / 1000000.0;
    if (drift > 0.0) {
      const double errorSeconds = (NTP_ERROR_BUDGET_SEC - CLOCK_INITIAL_UNCERTAINTY_SEC) * NTP_RESYNC_MARGIN / drift;
      if (seconds > errorSeconds) seconds = errorSeconds;
    }
    if (seconds < NTP_MIN_INTERVAL_SEC) seconds = NTP_MIN_INTERVAL_SEC;
    interval = (uint32_t)seconds;
  }
  ntpIntervalSec = interval;
  sntp_set_sync_interval(interval * 1000UL);
}
void onNtpSync(struct timeval *tv)
{
  uint64_t mono = (uint64_t)esp_timer_get_time();
  int64_t epochUs = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;

  portENTER_CRITICAL(&clockMux);
  uint64_t previousMono = ntpLastMonoUs;
  int64_t previousEpoch = ntpLastEpochUs;
  portEXIT_CRITICAL(&clockMux);
  if (previousMono != 0 && mono >= previousMono) {
    uint64_t elapsed = mono - previousMono;
    if (elapsed > 30000000ULL) {
      int64_t expected = previousEpoch + (int64_t)elapsed;
      int64_t correction = epochUs - expected;
      double ppm = ((double)correction * 1000000.0) / (double)elapsed;

      portENTER_CRITICAL(&clockMux);
      ntpDriftPpm = clockDriftSampleAvailable
        ? ntpDriftPpm * (1.0 - NTP_DRIFT_EMA_ALPHA) + ppm * NTP_DRIFT_EMA_ALPHA
        : ppm;
      ntpDriftAbsSecPerHour = fabs(ntpDriftPpm) * 3600.0 / 1000000.0;
      clockDriftSampleAvailable = true;
      portEXIT_CRITICAL(&clockMux);
    }
  }

  portENTER_CRITICAL(&clockMux);
  ntpLastMonoUs = mono;
  ntpLastEpochUs = epochUs;
  clockNtpEverSynced = true;
  ++clockNtpSyncCount;
  portEXIT_CRITICAL(&clockMux);
  ntpsync = 1;
  configureAdaptiveNtp();
}
bool shouldWifiBeOnForSchedule(void)
{
  // Bluetooth watch sync also needs Wi-Fi available during its short
  // preparation/connection window when Scheduled power-save is enabled.
  // This gives the BLE sync an opportunity to request a fresh NTP sync without
  // requiring a radio transmission schedule at the same time.
  if (!full_time_tx) {
    time_t epochNow=time(nullptr); struct tm wifiBtLocal; bluetoothLocalTime(epochNow,wifiBtLocal);
    int current_min = wifiBtLocal.tm_hour * 60 + wifiBtLocal.tm_min;
    for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
      if (!btSyncEnabled[i]) continue;
      for (int d = -BT_PREPARE_MINUTES; d <= BT_WINDOW_AFTER_MINUTES; ++d) {
        int test = btSyncTimes[i] + d;
        while (test < 0) test += 1440;
        while (test >= 1440) test -= 1440;
        if (current_min == test) return true;
      }
    }
  }

  // Full-time transmission or no configured schedules -> there's no
  // discrete "next start time" to key off. Wake periodically instead so
  // the clock still gets an occasional NTP resync.
  if (full_time_tx || schedule_count == 0) {
    static unsigned long periodicWakeAnchor = 0;
    unsigned long now = millis();
    if (periodicWakeAnchor == 0) periodicWakeAnchor = now; // first check after boot
    unsigned long sinceAnchor = now - periodicWakeAnchor;
    if (sinceAnchor < WIFI_PERIODIC_WAKE_DURATION_MS) return true;
    if (sinceAnchor >= WIFI_PERIODIC_WAKE_INTERVAL_MS) periodicWakeAnchor = now;
    return false;
  }

  time_t epochNow=time(nullptr); struct tm wifiLocal; localtime_r(&epochNow,&wifiLocal);
  int current_min = wifiLocal.tm_hour * 60 + wifiLocal.tm_min;
  for (int i = 0; i < schedule_count; ++i) {
    int start = schedules[i].start_min;
    int lead = start - WIFI_PRE_SCHEDULE_LEAD_MIN;
    bool inWindow;
    if (lead >= 0) {
      inWindow = (current_min >= lead && current_min < start);
    } else {
      // Lead window wraps past local midnight.
      lead += 1440;
      inWindow = (current_min >= lead || current_min < start);
    }
    if (inWindow) return true;
  }
  return false;
}
int main(){setenv("TZ","UTC0",1);tzset();
struct timeval reply{1760000000,0};onNtpSync(&reply);
int wakes=0; for(int minute=0;minute<1440;++minute){fakeEpoch=1760054400+minute*60; tick=minute*60000UL; if(shouldWifiBeOnForSchedule())++wakes;}
assert(wakes==10);mockMonoUs=ntpLastMonoUs+21601ULL*1000000ULL;assert(!clockTrusted());
printf("CONFIRMED: default all-day LF / BT schedules Off -> only %d wake minutes per day; NTP trust expires before the next daily wake\n",wakes);
return 0;}
