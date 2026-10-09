
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
int64_t restoredEpochUs = 0;
int fakeSetTime(const struct timeval* tv, const void*) {
  restoredEpochUs = (int64_t)tv->tv_sec*1000000 + tv->tv_usec; return 0;
}
#define settimeofday fakeSetTime
uint32_t sdkIntervalMs = 3600000, sdkNextTimeoutMs = 0;
unsigned ntpRestarts = 0;
constexpr int SNTP_SYNC_STATUS_RESET=0,SNTP_SYNC_STATUS_COMPLETED=1;
int sdkSyncStatus=SNTP_SYNC_STATUS_RESET;
void sntp_set_sync_status(int status) { sdkSyncStatus=status; }
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
static double clockDriftBoundPpm = CLOCK_INITIAL_PPM; // Absolute envelope: opposite signs cannot cancel.
static bool clockNtpRejected = false;
static uint32_t clockNtpRejectedCount = 0;
static const double CLOCK_DRIFT_FLOOR_PPM = 5.0;
static const double CLOCK_MAX_PLAUSIBLE_PPM = 1000.0;
uint64_t monotonicUptimeSeconds() { return (uint64_t)esp_timer_get_time() / 1000000ULL; }
uint32_t clockAgeSeconds() {
  portENTER_CRITICAL(&clockMux);
  bool valid=clockNtpEverSynced;
  uint64_t snapshot=ntpLastMonoUs;
  portEXIT_CRITICAL(&clockMux);
  if (!valid || !snapshot) return UINT32_MAX;
  int64_t age = esp_timer_get_time() - (int64_t)snapshot;
  if (age < 0 || (uint64_t)age / 1000000ULL >= UINT32_MAX) return UINT32_MAX;
  return (uint32_t)((uint64_t)age / 1000000ULL);
}
double clockEstimatedError() {
  uint32_t age = clockAgeSeconds();
  if (age == UINT32_MAX) return 9999.0;
  portENTER_CRITICAL(&clockMux);
  const double ppm=clockDriftBoundPpm;
  portEXIT_CRITICAL(&clockMux);
  return CLOCK_INITIAL_UNCERTAINTY_SEC + ppm * (double)age / 1000000.0;
}
const char *clockConfidence() {
  uint32_t age = clockAgeSeconds();
  portENTER_CRITICAL(&clockMux);
  const bool rejected=clockNtpRejected;
  portEXIT_CRITICAL(&clockMux);
  if (rejected || age == UINT32_MAX || age > CLOCK_MAX_HOLDOVER_SEC || clockEstimatedError() > 0.90) return "Unsynchronized";
  return age <= ntpIntervalSec ? "Synchronized" : "Holdover";
}
bool clockTrusted() { return strcmp(clockConfidence(), "Unsynchronized") != 0; }
#define NTP_INITIAL_INTERVAL_SEC 3600UL   // Resync interval before any drift has been measured yet
#define NTP_MIN_INTERVAL_SEC 15UL         // SNTP/RFC minimum; large drift must not force a 15-minute loss of trust
#define NTP_ERROR_BUDGET_SEC 0.90          // Total error budget, including initial sync uncertainty
#define NTP_RESYNC_MARGIN 0.90            // Request again before either the error or six-hour trust limit
#define NTP_DRIFT_EMA_ALPHA 0.35          // Smoothing factor (0-1) for the drift estimate; higher = reacts faster to new measurements, lower = more stable against noisy single measurements

void configureAdaptiveNtp(void)
{
  portENTER_CRITICAL(&clockMux);
  bool measured=clockDriftSampleAvailable;
  double samplePpm=clockDriftBoundPpm;
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
static bool ntpReplyPlausible(const struct timeval *tv)
{
  if (!tv || tv->tv_usec < 0 || tv->tv_usec >= 1000000 ||
      tv->tv_sec < 1577836800LL || tv->tv_sec >= 4102444800LL) return false;
  const uint64_t mono = (uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&clockMux);
  const bool known=clockNtpEverSynced;
  const uint64_t anchorMono=ntpLastMonoUs;
  const int64_t anchorEpoch=ntpLastEpochUs;
  portEXIT_CRITICAL(&clockMux);
  if (!known) return true;
  if (mono < anchorMono) return false;
  const uint64_t elapsed=mono-anchorMono;
  const int64_t epochUs=(int64_t)tv->tv_sec*1000000LL+tv->tv_usec;
  const int64_t expected=anchorEpoch+(int64_t)elapsed;
  const double allowedUs=2000000.0 + (double)elapsed*CLOCK_MAX_PLAUSIBLE_PPM/1000000.0;
  return fabs((double)(epochUs-expected)) <= allowedUs;
}
static void rejectNtpReply(void)
{
  portENTER_CRITICAL(&clockMux);
  clockNtpRejected=true;
  ++clockNtpRejectedCount;
  portEXIT_CRITICAL(&clockMux);
  ntpsync=0;
  ntpIntervalSec=NTP_MIN_INTERVAL_SEC;
  sntp_set_sync_interval(NTP_MIN_INTERVAL_SEC*1000UL);
}
void onNtpSync(struct timeval *tv)
{
  // Called only after the supported pre-commit SNTP hook accepts and applies
  // this reply. Retain a defensive guard for any direct callback invocation.
  if (!ntpReplyPlausible(tv)) { rejectNtpReply(); return; }
  const uint64_t mono = (uint64_t)esp_timer_get_time();
  const int64_t epochUs = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
  portENTER_CRITICAL(&clockMux);
  const uint64_t previousMono = ntpLastMonoUs;
  const int64_t previousEpoch = ntpLastEpochUs;
  const bool previouslySynced = clockNtpEverSynced;
  portEXIT_CRITICAL(&clockMux);
  const uint64_t elapsed = mono >= previousMono ? mono - previousMono : 0;
  const int64_t expected = previousEpoch + (int64_t)elapsed;
  const int64_t correction = epochUs - expected;
  if (previouslySynced && elapsed >= 300000000ULL) {
    const double ppm = (double)correction * 1000000.0 / (double)elapsed;
    // Bound measurement uncertainty at both ends. Retain recent absolute
    // peaks with slow decay; signed diagnostic EMA cannot hide reversal.
    const double measurementBound = fabs(ppm) +
         2.0 * CLOCK_INITIAL_UNCERTAINTY_SEC * 1000000000000.0 / (double)elapsed;
    portENTER_CRITICAL(&clockMux);
    ntpDriftPpm = clockDriftSampleAvailable
      ? ntpDriftPpm * (1.0 - NTP_DRIFT_EMA_ALPHA) + ppm * NTP_DRIFT_EMA_ALPHA : ppm;
    clockDriftBoundPpm = fmax(CLOCK_DRIFT_FLOOR_PPM,
        fmax(measurementBound, clockDriftBoundPpm * 0.95));
    ntpDriftAbsSecPerHour = clockDriftBoundPpm * 3600.0 / 1000000.0;
    clockDriftSampleAvailable = true;
    portEXIT_CRITICAL(&clockMux);
  }
  portENTER_CRITICAL(&clockMux);
  ntpLastMonoUs = mono;
  ntpLastEpochUs = epochUs;
  clockNtpEverSynced = true;
  clockNtpRejected = false;
  ++clockNtpSyncCount;
  portEXIT_CRITICAL(&clockMux);
  ntpsync = 1;
  configureAdaptiveNtp();
}
extern "C" void sntp_sync_time(struct timeval *tv)
{
  if (!ntpReplyPlausible(tv)) {
    rejectNtpReply();
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    return;
  }
  if (settimeofday(tv, nullptr) != 0) {
    rejectNtpReply();
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    return;
  }
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
  onNtpSync(tv);
}
int main() {
  const int64_t correctStart = 1791500000LL * 1000000LL;
  const int64_t wrongOffset = 3600LL * 1000000LL;
  struct timeval first{(time_t)((correctStart + wrongOffset) / 1000000LL), 0};
  sntp_sync_time(&first);
  assert(clockTrusted() && clockNtpSyncCount == 1);
  const int64_t stored = ntpLastEpochUs;
  for (int i = 1; i <= 5760; ++i) {
    mockMonoUs = 1000000ULL + (uint64_t)i * 15000000ULL;
    const int64_t corrected = correctStart + (int64_t)i * 15000000LL;
    struct timeval good{(time_t)(corrected / 1000000LL), (suseconds_t)(corrected % 1000000LL)};
    sntp_sync_time(&good);
    assert(!clockTrusted() && clockNtpSyncCount == 1);
    assert(ntpLastEpochUs == stored);
  }
  std::printf("Initial plausible reply accepted one hour ahead; 5760 correct replies over 24 hours all rejected. NTP accepted count=%u rejected count=%u confidence=%s.\n", clockNtpSyncCount, clockNtpRejectedCount, clockConfidence());
  // A one-off accepted NTP jitter sample can also lock the adaptive interval
  // below its own 300-second measurement admission threshold.
  clockNtpEverSynced=false; clockDriftSampleAvailable=false;
  clockNtpRejected=false; clockDriftBoundPpm=CLOCK_INITIAL_PPM;
  clockNtpSyncCount=0; ntpLastMonoUs=0; ntpLastEpochUs=0;
  ntpIntervalSec=NTP_INITIAL_INTERVAL_SEC; mockMonoUs=1000000ULL;
  struct timeval initial{(time_t)(correctStart/1000000LL),0};
  sntp_sync_time(&initial);
  mockMonoUs+=300000000ULL;
  struct timeval jitter{(time_t)(correctStart/1000000LL+300),500000};
  sntp_sync_time(&jitter);
  const uint32_t penaltyInterval=ntpIntervalSec.load();
  assert(penaltyInterval<300 && penaltyInterval>=15);
  const double penaltyBound=clockDriftBoundPpm;
  for (int reply=1;reply<=100;++reply) {
    mockMonoUs+=(uint64_t)penaltyInterval*1000000ULL;
    struct timeval perfect{(time_t)(correctStart/1000000LL+300+(int64_t)reply*penaltyInterval),0};
    sntp_sync_time(&perfect);
    assert(clockTrusted() && clockDriftBoundPpm==penaltyBound && ntpIntervalSec==penaltyInterval);
  }
  std::printf("One 500ms jitter sample at 300s produced %.3f ppm bound and %u-second interval; 100 perfect replies spanning %u seconds never update or reduce that bound.\n",penaltyBound,penaltyInterval,penaltyInterval*100);
}
