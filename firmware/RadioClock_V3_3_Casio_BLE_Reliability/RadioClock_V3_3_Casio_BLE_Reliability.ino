//
// RadioClock_V3_3_Casio_BLE_Reliability.ino
// Based on RadioClock_JJY40_HWTimer_Validated_V2.7; Casio time-only BLE protocol selection.
// Retains V2.7 RF timing, encoding, timezone, LittleFS, Wi-Fi power and flash-wear improvements.
// Scheduled watch-initiated BLE: GW-BX5600 MIP, Standard digital/hybrid, and experimental analogue time-only.
// JJY validation baseline:
//   NICT "The Method of Emitting Standard Time and Frequency Signal Emission"
//   https://jjy.nict.go.jp/jjy/trans/index-e.html
//
// Important implementation choice:
//   - ESP32 LEDC = hardware 40 kHz carrier
//   - ESP32 hardware timer = 1 kHz task wake; RF envelope phase follows system clock
//   - JJY envelope = 0.2/0.5/0.8 s, selected by system clock phase
//   - No software GPIO bit-banging of the 40 kHz carrier
//
// V3.3 builds on the V3.2.2 BX1 release and hardens the Casio Bluetooth workflow:
//   - Explicit Pair Watch remains separate from normal manual Sync.
//   - Manual Sync is restartable instead of returning a vague "already busy" error.
//   - Optional Always Wait mode continuously listens for the already-paired selected watch.
//   - Bluetooth and LF radio transmission are mutually exclusive; RF has priority.
//   - NimBLE is fully deinitialized for the duration of any RF transmission and reinitialized afterwards.
//   - Existing watch binding is replaced only after a successful pairing connection/time write.
//   - Static web UI remains gzip-compressed in flash and streamed from PROGMEM.
//   - NimBLE-Arduino 2.5.1+ is required for reliable shutdown/re-init on Arduino-ESP32 3.3.11.
// BX1 correction: SP record framing, complete single-ATT writes, and stage/error diagnostics.

// Using software radio on ESP32/ESP32-C3.
//
// (c) 2021 by taroh (sasaki.taroh@gmail.com) (origin)
//
// (c) 2025 by 5Breeze (bitshen@qq.com)(improve)
// Modified 2025: Added web UI, WiFi config, multi-station scheduling, station rotation
//
// Supported Radio Time Services:
// - JJY (Japan): NICT time-code specification
// - WWVB (US): NIST WWVB time-code specification
// - DCF77 (Germany): PTB DCF77 time-code specification
// - MSF (UK): NPL MSF time-code specification
// - BPC (China): 68.5 kHz China Standard Time signal
//
// Primary standards: JJY, WWVB, DCF77, MSF and BPC.
// BSF is retained only as a legacy experimental mode for compatibility with old schedules.
// This output is intended as a low-power local time-signal emulator, not a broadcast transmitter.

//...................................................................
// Hardware Configuration - Auto-detect ESP32 variant
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3 GPIO Pins
  #define PIN_RADIO  (3)  // Radio signal output pin
  #define PIN_BUZZ    (4)  // Buzzer output pin
  #define PIN_LED    (5)  // LED indicator pin (envelope indicator)
  // ESP32-C3 devkits vary in which pin (if any) is a simple onboard LED
  // (many use GPIO8 for a WS2812 RGB LED instead). Rather than guess wrong
  // and drive a pin the board doesn't expect, default the "onboard LED" to
  // the same pin as PIN_LED here so the code still compiles/runs sensibly.
  #define PIN_ONBOARD_LED (5)
#else
  // ESP32 (Classic) GPIO Pins
  #define PIN_RADIO  (26) // Radio signal output pin
  #define PIN_BUZZ   (27) // Buzzer output pin
  #define PIN_LED    (25) // LED indicator pin (mirrors the RF envelope - flashes with the transmitted signal)
  // The original ESP32-DevKitC-style "ESP32 Dev Module" board has a fixed
  // onboard LED wired to GPIO2 (active HIGH). This is used here as a
  // separate system-status indicator (WiFi/AP state), independent of
  // PIN_LED which tracks the RF envelope.
  #define PIN_ONBOARD_LED (2)
#endif

// Library Includes
#include <WiFi.h>           // WiFi connectivity
#include <WebServer.h>      // HTTP web server
#include <LittleFS.h>        // File system for configuration storage (see initFilesystem())
// BLE central stack. NimBLE-Arduino 2.5.1+ substantially reduces
// flash/RAM use while retaining the Casio client protocols.
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#ifdef USING_NIMBLE_ARDUINO_HEADERS
#include "nimble/porting/nimble/include/nimble/nimble_port.h"
#else
#include "nimble/nimble_port.h"
#endif

#include "RadioBleArbiter.h"
#include "CasioBxProtocol.h"
#include "esp_bt.h"
#include <string>
#include <strings.h>
#include <ArduinoJson.h>    // JSON parsing and generation
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_timer.h"
#include <math.h>

// Global Objects
WebServer server(80);     // Web server on port 80
bool webRoutesRegistered=false;
bool webServerStarted=false;

// Configuration Constants
#define DEVICENAME_PREFIX "RadioStation"     // Device name prefix for WiFi AP mode
#define FIRMWARE_VERSION "V3.3"
#define FIRMWARE_BUILD "BX1"

#define DEFAULT_TZ_NAME "Asia/Tokyo"
#define CONFIG_FILE "/config.json"           // WiFi and timezone configuration file
#define STATION_CONFIG_FILE "/stations.json" // Station configuration file
#define CONFIG_TEMP_FILE "/config.tmp"
#define SCHEDULE_TEMP_FILE "/stations.tmp"

// WiFi Configuration Variables
char ssid[64] = "";                          // WiFi SSID (network name)
char passwd[64] = "";                        // WiFi password
String timezone_name = DEFAULT_TZ_NAME;           // IANA timezone name
std::atomic<int> transmission_offset_minutes{0};           // Extra offset (minutes) applied on top of the selected time zone's local time; 0 = exactly that zone's time
unsigned long wifi_connect_start = 0;        // Timestamp when WiFi connection started
#define WIFI_CONNECT_TIMEOUT 30000           // WiFi connection timeout: 30 seconds
#define WIFI_CONNECT_CHECK_INTERVAL 5000     // WiFi connection check interval: 5 seconds

//...................................................................
// Radio Station Definitions and Specifications

// Station Index Constants
#define SN_JJY_E  (0) // JJY Fukushima, Japan (40 KHz)
#define SN_JJY_W  (1) // JJY Fukuoka, Japan (60 KHz)
#define SN_WWVB (2)   // WWVB, United States (60 KHz)
#define SN_DCF77  (3) // DCF77, Germany (77.5 KHz)
#define SN_BSF  (4)   // BSF, Taiwan (77.5 KHz)
#define SN_MSF  (5)   // MSF, United Kingdom (60 KHz)
#define SN_BPC  (6)   // BPC, China (68.5 KHz)

#define SN_DEFAULT  (SN_JJY_E) // Default station
#define NUM_STATIONS 7         // Total number of supported stations

// Station Display Names (Full)
const char *station_names[] = {
  "JJY-E (40KHz)",
  "JJY-W (60KHz)",
  "WWVB (60KHz)",
  "DCF77 (77.5KHz)",
  "BSF (77.5 kHz, experimental carrier only)",
  "MSF (60KHz)",
  "BPC (68.5KHz)"
};

// Station Display Names (Short)
const char *station_short[] = {
  "JJY-E",
  "JJY-W",
  "WWVB",
  "DCF77",
  "BSF",
  "MSF",
  "BPC"
};

// Time Schedule Structure
// Defines when a specific radio station should be active
typedef struct {
  uint8_t station;      // Station index (0-6)
  uint16_t start_min;   // Start time in minutes from midnight (0-1439)
  uint16_t end_min;     // End time in minutes from midnight (0-1439)
} TimeSchedule;

// Schedule Storage
#define MAX_SCHEDULES 24                      // Maximum number of schedules
TimeSchedule schedules[MAX_SCHEDULES];        // Array of time schedules
int schedule_count = 0;                       // Current number of active schedules

// Multi-Station Rotation Control
// Used when multiple schedules overlap in time, causing the system to rotate between stations
#define ROTATION_INTERVAL_MINUTES 5           // Switch station every N minutes during rotation
int current_schedule_index = -1;              // Current selected schedule (-1 = none active)
unsigned long last_rotation_time = 0;         // Timestamp of last rotation event
int applicable_schedules[MAX_SCHEDULES];      // Array of currently applicable schedule indices
int applicable_count = 0;                     // Number of applicable schedules at current time
std::atomic<int> last_station{-1};                        // Last selected station (prevents redundant switches)

//...................................................................
// Timer and Interrupt Configuration

// Time Base Constants
// Timer interrupt rate depends on frequency: 1 KHz base frequency
// AMPDIV: Amplitude update frequency (10 Hz)
// SSECDIV: Second tick frequency (1 Hz)
#define AMPDIV   (100)  // 1 KHz / 100 => 10 Hz (amplitude update every 0.1 seconds)
#define SSECDIV   (10)  // 10 Hz / 10 => 1 Hz (clock tick every 1 second)

// Time-code bit-position markers used throughout the mb_*() encoders below
// as the values written into txSymbol[]/parity()'s input.
#define SP_0  (0)  // Binary 0 symbol
#define SP_1  (1)  // Binary 1 symbol
#define SP_M  (2)  // Minute/position marker symbol
#define SP_M4 (4)  // BPC: marks the frame-start second of each 20-second block in txSymbol[] (see mb_bpc())

// (Earlier revisions of this file also built each minute's frame from a set
// of static per-station template arrays - bits_jjy[]/sp_jjy[]/bits_dcf[]/
// etc, indexed via st_bits[]/st_sp[]/bits60/secpattern. That machinery was
// removed during cleanup: it was fully superseded by the mb_jjy()/mb_wwvb()/
// mb_dcf()/mb_msf()/mb_bpc() functions further down, which compute the
// actual current time via stationTime() and write directly into
// txSymbol[60]/txEnvelope[60][10] - the arrays ampchange() actually reads
// to drive the RF envelope. The old arrays were assigned on every station
// switch but never read by anything, so they were pure dead weight. If
// you're looking for how a station's time code is actually built, start at
// mb_jjy() etc., not here.)

// Function Declarations: Time Code Pattern Generation

/**
 * Generate JJY (Fukushima/Fukuoka) time code pattern for the current second
 * Encodes: year, month, day, hour, minute, second, day-of-week, leap second, parity
 */
void mb_jjy(void);

/**
 * Generate WWVB (US) time code pattern for the current second
 * Encodes: year, month, day, hour, minute, second, day-of-year, leap second, parity
 */
void mb_wwvb(void);

/**
 * Generate DCF77 (Germany) time code pattern for the current second
 * Encodes: minute, hour, day, month, year, day-of-week, time-zone, parity, leap second
 */
void mb_dcf(void);

/**
 * Generate BSF (Taiwan) time code pattern for the current second
 * Uses quad (4-level) encoding for denser data
 */
void mb_bsf(void);

/**
 * Generate MSF (UK) time code pattern for the current second
 * Encodes: minute, hour, day, month, year, day-of-week, leap second, parity
 */
void mb_msf(void);
void mb_bpc(void);
void getlocaltime(void);
void binarize(int v, int pos, int len);
void rbinarize(int v, int pos, int len);
void rbcdize(int v, int pos, int len);
int parity(int pos, int len);

// Dedicated high-priority FreeRTOS task that performs all time-critical
// radio signal generation, woken directly by the hardware timer ISR.
void radioTask(void *pvParameters);
const char *stationEncodingName(int station);
const char *txFieldDescription(int station, int second);

// Applies the configured "Time zone" card selection to the C library as a
// POSIX TZ rule (see posixTzFor()/applyTimezone() below), used by both the
// displayed clock and the transmitted time code.
static void applyTimezone(void);
static void sendIndexPage(void);

// WiFi power management (Scheduled/power-save mode) - see definitions near
// checkWiFiConnection() for full explanation.
bool shouldWifiBeOnForSchedule(void);
void updateWifiPowerManagement(void);

// Debounced config persistence (flash-wear mitigation) - see definition
// near saveConfig()/writeConfigNow().
void writeConfigNow(void);
bool saveSchedules(void);
void normalizeBluetoothSlots();

// txSymbol/txEnvelope are THE active buffers: each mb_*() encoder writes
// this minute's frame directly into them, and ampchange() (in radioTask)
// reads txEnvelope every 1 ms to drive the RF carrier's PWM duty. See the
// notice above SP_0/SP_1/SP_M for what used to sit here instead.
uint8_t txEnvelope[60][10]; // 0=carrier off, 1=full carrier, 2=reduced carrier
uint8_t txSymbol[60];
std::atomic<uint32_t> carrierFrequencyHz{40000};
bool carrierReady = false;

std::atomic<bool> rfSilenceFailed{false};
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


// Function pointer lookup table: maps station index to its pattern generation function
void (*st_makebits[])(void) = {mb_jjy, mb_jjy, mb_wwvb, mb_dcf, mb_bsf, mb_msf, mb_bpc};
void (*makebitpattern)(void);  // Function pointer to current station's pattern generator

// LEDC carrier configuration. The LF carrier is generated entirely by hardware.
// 10-bit resolution gives a useful frequency/resolution tradeoff at 40-77.5 kHz.
#define RF_PWM_RESOLUTION 10
#define RF_PWM_CHANNEL    0

// LEDC duty controls the carrier waveform itself. 100% duty would produce a
// constant DC level and therefore NO LF carrier. The full carrier must be
// approximately 50% duty.
// Reduced-carrier values below are PWM-duty approximations for a resonant RF
// output stage. Exact 10%/15% RF amplitude requires an external attenuator or
// amplifier control; PWM duty is not linearly proportional to RF amplitude.
#define RF_DUTY_FULL  ((1U << RF_PWM_RESOLUTION) / 2U)

// Approximate the fundamental RF amplitude ratios with a narrow-duty PWM
// waveform. These values are intended for a tuned/resonant LF output stage.
#define RF_DUTY_JJY_REDUCED ((1U << RF_PWM_RESOLUTION) * 32U / 1000U)  // ~10% fundamental
#define RF_DUTY_WWVB_REDUCED ((1U << RF_PWM_RESOLUTION) * 32U / 1000U) // ~10% fundamental
#define RF_DUTY_DCF_REDUCED ((1U << RF_PWM_RESOLUTION) * 48U / 1000U)  // ~15% fundamental
#define RF_DUTY_BPC_REDUCED ((1U << RF_PWM_RESOLUTION) * 32U / 1000U) // ~10% fundamental

//...................................................................
// Hardware Timer and Interrupt Handling

// Hardware Timer 0 provides the deterministic 1 kHz envelope/time base.
// LEDC generates the RF carrier entirely in hardware.  This avoids bit-banging
// the 40 kHz carrier and keeps the carrier frequency independent of FreeRTOS loop jitter.
hw_timer_t *tm0 = NULL;  // ESP32 hardware timer handle

// Timer interrupt synchronization
// The ISR notifies a dedicated task. Every wake derives the current phase
// from the system clock so delayed ticks do not shift subsequent envelope edges.
TaskHandle_t radioTaskHandle = NULL;  // Dedicated high-priority radio-timing task

// Priority chosen to sit comfortably above the default Arduino loopTask
// (priority 1, which runs the web server / Serial / WiFi housekeeping) so
// it always preempts them, but below the ESP-IDF WiFi and TCP/IP stack
// tasks (typically priority ~18-23) so this task never starves the
// networking stack it depends on for NTP.
#define RADIO_TASK_PRIORITY  3

// Timer state flag
int istimerstarted = 0;  // 1 if timer is running, 0 if stopped

//...................................................................
// Interrupt Counter Variables
// These track the 1 kHz housekeeping tick and the 10 Hz modulation frame.

int ampc = 0;    // Amplitude cycle counter: 0..(AMPDIV - 1), generates 10 Hz updates
int tssec = 0;   // Time base counter: 0..(SSECDIV - 1), generates 1 Hz clock ticks

//...................................................................
// Network Time and Synchronization

std::atomic<int> ntpsync{1};  // NTP synchronization flag: 1 = synchronized, 0 = not synchronized
std::atomic<bool> full_time_tx{false}; // Continuous transmission mode
std::atomic<int> full_time_station{SN_JJY_E}; // Radio type used in full-time mode
struct tm nowtm;  // Broken-down current LOCAL time in the selected timezone (see getlocaltime()); drives the clock display, schedule matching, and (via stationTime()) the transmitted time code.
std::atomic<bool> ntpSyncEvent{false};

// Set by the web-server task (loop()/HTTP handlers) whenever a change is
// made that affects what is encoded in the radio signal (transmission
// offset, full-time mode/station, timezone). The actual regeneration of
// txSymbol[]/txEnvelope[] is always performed by radioTask, never by the
// web handler itself, because those arrays are also read every 1 ms by
// ampchange() in radioTask - having two tasks write them concurrently could
// tear a read mid-update. radioTask checks this flag on every 1 kHz tick,
// so the encoding (and therefore the UI, which polls /api/status) updates
// within at most ~1 ms of the change being made.
std::atomic<bool> encodingRefreshRequested{false};
// The radio task is the ONLY runtime owner of LEDC, station and RF duty.
// Timer stays running through idle and BLE pauses; commands wake radioTask.
std::atomic<bool> radioPauseRequested{false};
std::atomic<bool> radioResumeRequested{false};
std::atomic<bool> radioPaused{false};
std::atomic<bool> radioPauseAck{false};
std::atomic<int> radioBoundaryErrorUs{0};
std::atomic<uint32_t> radioBoundaryWorstUs{0};
std::atomic<uint32_t> radioMissedBoundaries{0};
std::atomic<uint32_t> radioProcessedSecond{0};
// RadioTask queues small snapshots; loop() performs potentially blocking Serial IO.
portMUX_TYPE radioLogMux = portMUX_INITIALIZER_UNLOCKED;
int radioApplicableCountSnapshot = 0;
int radioApplicableSchedulesSnapshot[MAX_SCHEDULES] = {};
volatile bool radioPatternLogPending = false;
volatile bool radioSecondLogPending = false;
uint8_t radioLogBits[60] = {};
int radioLogStation = -1;
uint32_t radioLogHz = 0;
struct tm radioSecondTm = {};
int radioSecondStation = -1;
void queuePatternLog(int station) {
  if (station < 0 || station >= NUM_STATIONS) return;
  portENTER_CRITICAL(&radioLogMux);
  memcpy(radioLogBits, txSymbol, sizeof(radioLogBits));
  radioLogStation = station; radioLogHz = carrierFrequencyHz;
  radioPatternLogPending = true;
  portEXIT_CRITICAL(&radioLogMux);
}
void queueSecondLog(int station) {
  if (station < 0 || station >= NUM_STATIONS) return;
  portENTER_CRITICAL(&radioLogMux);
  radioSecondTm = nowtm; radioSecondStation = station;
  radioSecondLogPending = true;
  portEXIT_CRITICAL(&radioLogMux);
}
void radioRequestRefresh() { encodingRefreshRequested = true; if (radioTaskHandle) xTaskNotifyGive(radioTaskHandle); }
bool radioSetPaused(bool pause) {
  if (!radioTaskHandle) { radioPaused = pause; return true; }
  radioPauseAck = false;
  if (pause) radioPauseRequested = true; else radioResumeRequested = true;
  xTaskNotifyGive(radioTaskHandle);
  uint32_t started = millis();
  while (!radioPauseAck && millis() - started < 2000) delay(1);
  if (!radioPauseAck) Serial.println("ERROR: radio command acknowledgement timed out");
  return radioPauseAck && radioPaused == pause;
}

// --- Adaptive NTP resync interval ---
// Rather than resyncing on a fixed schedule, this measures how much the
// board's own clock actually drifts between syncs and shortens/lengthens
// the resync interval to match - a board with a more accurate crystal gets
// resynced less often (saving a WiFi/NTP round-trip), a drifty one gets
// resynced more often, and either way the clock stays within roughly
// NTP_ERROR_BUDGET_SEC seconds of correct between syncs. See onNtpSync()
// (measures the drift on every successful sync) and configureAdaptiveNtp()
// (turns that measurement into a new resync interval).
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
  uint32_t count=clockNtpSyncCount;
  double drift=ntpDriftPpm;
  portEXIT_CRITICAL(&clockMux);
  double ppm = count < 2 ? CLOCK_INITIAL_PPM : fabs(drift);
  return CLOCK_INITIAL_UNCERTAINTY_SEC + ppm * (double)age / 1000000.0;
}
const char *clockConfidence() {
  uint32_t age = clockAgeSeconds();
  if (age == UINT32_MAX || age > CLOCK_MAX_HOLDOVER_SEC || clockEstimatedError() > 0.90) return "Unsynchronized";
  return age <= ntpIntervalSec ? "Synchronized" : "Holdover";
}
bool clockTrusted() { return strcmp(clockConfidence(), "Unsynchronized") != 0; }     // Current resync interval, recalculated by configureAdaptiveNtp()
#define NTP_INITIAL_INTERVAL_SEC 3600UL   // Resync interval before any drift has been measured yet
#define NTP_MIN_INTERVAL_SEC 900UL        // Never resync more often than this, even for a very drifty clock
#define NTP_MAX_INTERVAL_SEC 86400UL      // Never wait longer than this between resyncs, even for a very accurate clock
#define NTP_ERROR_BUDGET_SEC 0.90         // Target maximum clock error to tolerate between resyncs
#define NTP_DRIFT_EMA_ALPHA 0.35          // Smoothing factor (0-1) for the drift estimate; higher = reacts faster to new measurements, lower = more stable against noisy single measurements

//...................................................................
// Casio BLE time-only profiles. Values are persisted: do not reorder.
// 0: GW-BX5600 MIP with the model-specific four-step SP handshake.
// 1: Standard digital / hybrid (10-byte current-time write).
// 2: Analogue/MT-G experimental time-only (same inherited standard time write;
//    motor/second-dial setup is intentionally never written).
// Do not send MIP packets to other MIP models without validating their handshake.
enum CasioBleProtocol : uint8_t {
  BT_PROTOCOL_BX5600_MIP = 0,
  BT_PROTOCOL_STANDARD = 1,
  BT_PROTOCOL_ANALOGUE = 2,
  BT_PROTOCOL_COUNT = 3
};
static bool validBtProtocol(int p) { return p >= 0 && p < BT_PROTOCOL_COUNT; }
static const char *btProtocolName(int p) {
  switch (p) {
    case BT_PROTOCOL_BX5600_MIP: return "GW-BX5600 MIP";
    case BT_PROTOCOL_STANDARD: return "Standard digital/hybrid";
    case BT_PROTOCOL_ANALOGUE: return "Analogue time-only (experimental)";
    default: return "Unknown Casio protocol";
  }
}

// Casio GW-BX5600 BLE time synchronization
// Watch-initiated connection model: the ESP32 opens a scan window and
// waits for the watch to advertise its Casio Bluetooth service.
#define CASIO_ADVERTISEMENT_SERVICE "1804"
#define CASIO_WATCH_FEATURES_SERVICE "26eb000d-b012-49a8-b1f8-394fb2032b0f"
#define CASIO_SP_REQUEST_CHAR       "26eb002e-b012-49a8-b1f8-394fb2032b0f"
#define CASIO_SP_DATA_CHAR          "26eb002f-b012-49a8-b1f8-394fb2032b0f"
#define CASIO_SET_CHAR              "26eb002d-b012-49a8-b1f8-394fb2032b0f"

#define BT_SYNC_SLOT_COUNT 4
#define BT_PREPARE_MINUTES 2
#define BT_WINDOW_AFTER_MINUTES 3
#define BT_REQUEST_TIMEOUT_MS 5000UL
#define BT_SCAN_SLICE_MS 1000UL

int btSyncTimes[BT_SYNC_SLOT_COUNT] = {30, 390, 750, 1110}; // 00:30, 06:30, 12:30, 18:30
bool btSyncEnabled[BT_SYNC_SLOT_COUNT] = {true, true, true, true};
uint8_t btSyncProtocol[BT_SYNC_SLOT_COUNT] = {BT_PROTOCOL_BX5600_MIP, BT_PROTOCOL_BX5600_MIP,
                                            BT_PROTOCOL_BX5600_MIP, BT_PROTOCOL_BX5600_MIP};
uint8_t btManualProtocol = BT_PROTOCOL_BX5600_MIP;
uint8_t btActiveProtocol = BT_PROTOCOL_BX5600_MIP;
#define BT_WATCH_PROFILE_COUNT 4
uint8_t btSyncProfile[BT_SYNC_SLOT_COUNT] = {0,0,0,0};
uint8_t btManualProfile = 0;
uint8_t btActiveProfile = 0;
int btProfileSuccessYear[BT_WATCH_PROFILE_COUNT] = {-1,-1,-1,-1};
int btProfileSuccessYday[BT_WATCH_PROFILE_COUNT] = {-1,-1,-1,-1};
String btProfileAddress[BT_WATCH_PROFILE_COUNT]; // Learned watch address; may change with BLE privacy.
String btProfileName[BT_WATCH_PROFILE_COUNT];
uint8_t btProfileProtocol[BT_WATCH_PROFILE_COUNT] = {0,0,0,0};
uint8_t btLastWatchAddressType = 0xFF;
uint32_t btConnectionAttempts = 0;
uint32_t btWriteAcknowledgements = 0;
std::atomic<uint32_t> btNotifications{0};
std::atomic<uint32_t> btResponseErrors{0};
const char *btDeliveryEvidence = "No delivery yet";
bool btProfileDoneToday(int profile) {
  if (profile < 0 || profile >= BT_WATCH_PROFILE_COUNT) return false;
  time_t now = time(nullptr); struct tm day; localtime_r(&now, &day);
  return btProfileSuccessYear[profile] == day.tm_year + 1900 &&
         btProfileSuccessYday[profile] == day.tm_yday;
}
// Circular distance between two clock times: handles midnight rollover.
int btCircularDistance(int a, int b) { int d = abs(a-b); return d < 720 ? d : 1440-d; }
bool btOverlapsOtherEnabledSlot(int slot,int minute) {
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i)
    if (i!=slot && btSyncEnabled[i] && btCircularDistance(btSyncTimes[i],minute) <=
        BT_PREPARE_MINUTES + BT_WINDOW_AFTER_MINUTES + 1) return true;
  return false;
}
// Completion is per protocol: a BX5600 success does not suppress a separately
// scheduled Standard or Analogue watch. Manual sync is always possible.
int btSuccessYear[BT_PROTOCOL_COUNT] = {-1, -1, -1};
int btSuccessYday[BT_PROTOCOL_COUNT] = {-1, -1, -1};
bool btSyncDayComplete = false;
int btSyncLastYear = -1;
int btSyncLastYday = -1;
int btSyncActiveSlot = -1;
unsigned long btWindowEndMillis = 0;
int btSlotAttemptYear[BT_SYNC_SLOT_COUNT] = {-1,-1,-1,-1};
int btSlotAttemptYday[BT_SYNC_SLOT_COUNT] = {-1,-1,-1,-1};
std::atomic<bool> btWindowActive{false};
bool btManualSyncRequested = false;
// When enabled, RadioClock continuously scans for the already-paired selected
// watch while RF is idle. Pairing itself always remains an explicit action.
bool btAlwaysWaitEnabled = false;
bool btPersistentWaitActive = false;
// RF has absolute priority over BLE. radioTask sets this request when a radio
// schedule becomes due while the BLE controller is still initialized/active;
// loop() performs the NimBLE shutdown and then asks radioTask to retry.
std::atomic<bool> btRadioSuspendRequested{false};
RadioBleArbiter radioBleArbiter;
bool btSuspendedByRadio = false;
// Pair mode deliberately ignores the currently learned address for the selected
// profile, but does not erase that binding until the new watch has completed a
// successful connection/time write. This makes a failed re-pair attempt safe.
bool btPairRequested = false;
bool btPairModeActive = false;
bool btBleInitialized = false;
std::atomic<bool> btBleBusy{false};
String btLastWatchName = "";
String btLastWatchAddress = "";
String btLastSyncStatus = "Never synced";
String btLastSyncDate = "";

NimBLEScan *btScan = nullptr;
NimBLEClient *btClient = nullptr;
NimBLERemoteService *btService = nullptr;
NimBLERemoteCharacteristic *btSpRequest = nullptr;
NimBLERemoteCharacteristic *btSpData = nullptr;
NimBLERemoteCharacteristic *btSetChar = nullptr;

// Scan callbacks run on the NimBLE host task. Never mutate shared Arduino
// String objects there: publish a fixed-size discovery snapshot and let loop() consume it.
volatile bool btDiscoveryReady = false;
char btDiscoveredName[48] = {};
char btDiscoveredAddress[18] = {};
char btExpectedProfileAddress[18] = {};
uint8_t btDiscoveredAddressType = 0xFF;
portMUX_TYPE btDiscoveryMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t btDiscoveryGeneration = 0;
uint8_t btDiscoveryProtocol = BT_PROTOCOL_BX5600_MIP;

volatile bool btResponseActive = false;
volatile uint8_t btExpectedHeader = 0;
volatile size_t btResponseLength = 0;
#define BT_RESPONSE_CAPACITY 512
uint8_t btResponseBuffer[BT_RESPONSE_CAPACITY];
volatile unsigned long btLastFragmentMillis = 0;
volatile bool btResponseOverflow = false;
portMUX_TYPE btResponseMux = portMUX_INITIALIZER_UNLOCKED;

void initBluetoothSync(void);
void serviceBluetoothSync(void);
bool performGShockBX5600Sync(void);
bool performCasioStandardTimeSync(int protocol);
static bool btWatchNameMatches(int protocol, const String &name);
bool bluetoothTimeSlotConflicts(int minuteOfDay);
void resetBluetoothDayIfNeeded(void);
void startBluetoothWindow(int slot, bool persistent = false);
void stopBluetoothWindow(void);
bool radioScheduleActiveNow(void);
bool bluetoothActivityPresent(void);
void shutdownBluetoothForRadio(void);
static void disconnectGShock(void);
static bool btClientQuiescent(void);
static void releaseBtClientBarrier(void);
static bool bleOperationCancelled() { return radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned(); }
static const char *btPhase = "Idle"; // loopTask owns UI state; callbacks only publish flags.
static void setBluetoothPhase(const char *phase) { btPhase = phase; }
static bool bluetoothSettingsMutable(void);
static String bluetoothState(void);
static String bluetoothProfilesJson(void);


//...................................................................
// Output Control Variables

int buzzout = 0;   // Current buzzer output pin value (0 or 1)

/**
 * Amplitude modulation value for current second
 * 1 = radio output active (transmitting)
 * 0 = radio output reduced (no transmission)
 * Applied to current amplitude subsecond within the second frame
 */
int ampmod;

/**
 * Buzzer control flag
 * 1 = sound on (generate buzzer output)
 * 0 = sound off (suppress buzzer output)
 */
int buzzsw = 1;

//...................................................................
// WiFi Access Point Mode Configuration

bool ap_mode = false;              // WiFi mode flag: true = AP mode, false = STA mode
unsigned long last_wifi_check = 0; // Timestamp of last WiFi connection check
unsigned long last_led_blink = 0;  // Timestamp of last onboard-LED toggle (WiFi status indicator)
bool onboardLedState = false;      // Current onboard-LED output state

//...................................................................
// WiFi power management ("Always on" vs "Scheduled/power-save")
//
// In Scheduled mode, WiFi is kept off except: (1) for a fixed window right
// after boot, so the device is always reachable to configure, and (2) for a
// lead-time window before each configured schedule's start, long enough to
// reconnect and get a fresh NTP fix. WiFi is deliberately OFF during the
// scheduled transmission itself - besides the obvious power saving, this
// also avoids the WiFi radio's own TX bursts/current spikes sitting right
// next to the antenna loop during transmission (a real interference source
// we identified and worked around earlier for this exact hardware).
#define WIFI_POWER_ALWAYS_ON  0
#define WIFI_POWER_SCHEDULED  1
int wifiPowerMode = WIFI_POWER_ALWAYS_ON;   // persisted config value

#define WIFI_BOOT_ON_DURATION_MS      (5UL * 60UL * 1000UL)   // 5 minutes
#define WIFI_PRE_SCHEDULE_LEAD_MIN    10                       // minutes
#define WIFI_PERIODIC_WAKE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL) // 6 hours
#define WIFI_PERIODIC_WAKE_DURATION_MS (10UL * 60UL * 1000UL)  // 10 minutes

bool wifiRadioEnabled = true;       // Our own intended on/off state (Scheduled mode only)
unsigned long bootMillis = 0;       // Set once in setup(); anchors the post-boot on-window

//...................................................................
// Debounced config persistence (flash-wear mitigation). Declared here,
// before loop(), because loop() reads these directly - unlike functions,
// plain variables/#defines are not auto-forward-declared by the Arduino
// build system, so they must appear in the file before first use. See the
// full explanation next to writeConfigNow()/saveConfig().
#define CONFIG_SAVE_DEBOUNCE_MS  3000
bool configDirty = false;
unsigned long configDirtyBecause = 0;

// extern 
void IRAM_ATTR onTimer(void)
{
  // 1 kHz hardware-timer tick.
  // The ISR does not generate the RF carrier; LEDC does that in hardware.
  // Keeping the ISR to a single notification call minimizes interrupt
  // latency and timing jitter. The notification is only a wakeup source:
  // if several ticks accumulate they can be coalesced because radioTask
  // derives second/subsecond phase from gettimeofday().
  BaseType_t hpTaskWoken = pdFALSE;
  if (radioTaskHandle != NULL) {
    vTaskNotifyGiveFromISR(radioTaskHandle, &hpTaskWoken);
  }
  if (hpTaskWoken) portYIELD_FROM_ISR();
}

//...................................................................
void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.printf("started... (firmware %s build %s)\n", FIRMWARE_VERSION, FIRMWARE_BUILD);
  bootMillis = millis();

  // Flash-wear mitigation #1: by default, Arduino-ESP32's WiFi.begin()
  // silently writes the SSID/password into the NVS flash partition on
  // EVERY call, regardless of whether they changed. This app already
  // persists credentials itself (see loadConfig()/saveConfig() and
  // LittleFS below), so that automatic NVS write is pure redundant flash
  // wear - and with WiFi power-save mode calling WiFi.begin() again on
  // every scheduled wake-up, it would otherwise repeat many times a day.
  // Disabling it here, once, removes that entirely; nothing else in this
  // sketch relies on the WiFi library's own credential persistence.
  WiFi.persistent(false);
  pinMode(PIN_RADIO, OUTPUT);
  digitalWrite(PIN_RADIO, LOW);

  // Onboard LED (see PIN_ONBOARD_LED): used purely as a WiFi/AP status
  // indicator, independent of PIN_LED which mirrors the RF envelope. Set
  // up first so it can reflect connection attempts as soon as they start.
  pinMode(PIN_ONBOARD_LED, OUTPUT);
  digitalWrite(PIN_ONBOARD_LED, LOW);

  // Initialize LittleFS for configuration storage
  initFilesystem();
  
  // Load configuration from flash
  loadConfig();
  // Apply the configured time zone (mapped to a POSIX TZ rule so DST is
  // handled correctly) before any local-time calculations. This keeps the
  // UI and scheduler correct even before the first NTP reply.
  if (timezone_name.length() > 0) {
    applyTimezone();
  }
  loadSchedules();
  normalizeBluetoothSlots();
  initBluetoothSync();

  // Check if WiFi credentials are configured
  if (strlen(ssid) == 0) {
    Serial.println("No WiFi config found, starting AP mode for configuration...");
    startAPMode();
  } else {
    // Try to connect to WiFi
    if (ntpsync) {
      ntpstart();
    }
    // If WiFi connection established, start web server
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      Serial.printf("WiFi connected! Web server at http://%s\n", ip.toString().c_str());
      initWebServer();
      Serial.println("Web server started");
      digitalWrite(PIN_ONBOARD_LED, HIGH);  // solid = connected
    } else if (!ntpsync && strlen(ssid) > 0) {
      Serial.println("WiFi connection failed, starting AP mode...");
      startAPMode();
    }
  }

  pinMode(PIN_RADIO, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);
  buzzout = 0;
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Start the dedicated radio-timing task BEFORE the hardware timer/LEDC
  // carrier are enabled (applyCurrentSchedule() -> setstation() ->
  // starttimer() below arms the 1 kHz interrupt), so radioTaskHandle is
  // valid the instant the first tick can arrive.
  //
  // On dual-core ESP32, pin alongside Arduino loopTask on core 1 so the
  // timing task can preempt web work without occupying the Wi-Fi core. ESP32-C3
  // is single-core, so use the unpinned API there. Always check creation: the
  // timer must never be armed if no task exists to consume its notifications.
  BaseType_t taskCreated;
#if CONFIG_FREERTOS_UNICORE
  taskCreated = xTaskCreate(radioTask, "radioTask", 4096, NULL,
                            RADIO_TASK_PRIORITY, &radioTaskHandle);
#else
  taskCreated = xTaskCreatePinnedToCore(radioTask, "radioTask", 4096, NULL,
                                        RADIO_TASK_PRIORITY, &radioTaskHandle, 1);
#endif
  if (taskCreated != pdPASS || radioTaskHandle == nullptr) {
    radioTaskHandle = nullptr;
    digitalWrite(PIN_RADIO, LOW);
    digitalWrite(PIN_LED, LOW);
    digitalWrite(PIN_BUZZ, LOW);
    Serial.println("FATAL: radioTask creation failed; RF left safely off");
    return;
  }

  starttimer();
  radioRequestRefresh();
  Serial.print("radio task and aligned scheduler started.\n");
}

// Dedicated, high-priority task that performs ALL time-critical radio
// signal work. It is woken directly by the hardware timer ISR via a task
// notification. Its RF phase never depends on counting notifications:
// every wake samples gettimeofday(), so if several 1 ms notifications build
// up while a higher-priority system task runs they may be coalesced safely
// instead of being replayed late. Normal envelope edges remain locked to the
// wall-clock 100 ms boundaries.
//
// No delay() of any kind appears in this task; it blocks only on the
// hardware-timer-driven notification and always derives phase from real time.
void radioTask(void *pvParameters)
{
  int64_t lastBoundary = -1;
  int lastMinute = -1;
  int lastSecond = -1;
  int lastSlot = -1;
  bool pauseSilencePending = false; // radioTask-owned acknowledgement retry
  for (;;) {
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
    // Derive the frame phase from the wall-clock on every wake; do not
    // assume the timer was started exactly on a second boundary.
    struct timeval tv; gettimeofday(&tv, nullptr);
    int second = (int)(tv.tv_sec % 60);
    int slot = (int)(tv.tv_usec / 100000);
    if (slot > 9) slot = 9;
    int64_t boundary = tv.tv_sec;
    bool boundaryChanged = lastBoundary != boundary;
    bool frameRefreshed = false;
    bool refreshRequested = encodingRefreshRequested.exchange(false);
    if (boundaryChanged || refreshRequested) {
      if (lastBoundary >= 0 && boundary > lastBoundary + 1)
        radioMissedBoundaries += (uint32_t)(boundary - lastBoundary - 1);
      if (lastBoundary != boundary) {
        // Diagnostic: callback arrival delay within this second; not a
        // calibrated RF edge measurement, but useful for detecting overload.
        radioBoundaryErrorUs = (int)tv.tv_usec;
        if ((uint32_t)tv.tv_usec > radioBoundaryWorstUs) radioBoundaryWorstUs = tv.tv_usec;
        ++radioProcessedSecond;
      }
      lastBoundary = boundary;
      getlocaltime();
      applyCurrentSchedule();
      portENTER_CRITICAL(&radioLogMux);
      radioApplicableCountSnapshot = applicable_count;
      memcpy(radioApplicableSchedulesSnapshot, applicable_schedules, sizeof(applicable_schedules));
      portEXIT_CRITICAL(&radioLogMux);
      bool refresh = refreshRequested || nowtm.tm_min != lastMinute || lastSecond < 0;
      if (refresh && makebitpattern && last_station >= 0) {
        makebitpattern();
        frameRefreshed = true;
        if (last_station != SN_BPC) queuePatternLog(last_station);
      }
      if (last_station == SN_BPC && lastSecond != nowtm.tm_sec && (nowtm.tm_sec % 20) == 0)
        queuePatternLog(last_station);
      if (boundaryChanged && last_station >= 0) queueSecondLog(last_station);
      lastMinute = nowtm.tm_min;
      lastSecond = nowtm.tm_sec;
    }
    tssec = slot;
    if (second != nowtm.tm_sec) getlocaltime();
    if (lastSlot != slot || boundaryChanged || frameRefreshed) ampchange();
    lastSlot = slot;
    // The output buzzer represents the envelope and is not an RF carrier.
    if (ampmod && buzzsw) buzzout = !buzzout;
    else buzzout = 0;
    digitalWrite(PIN_BUZZ, buzzout ? HIGH : LOW);
  }
}

// loop() now only handles non-time-critical housekeeping: WiFi supervision,
// SNTP bookkeeping, and the web server. It contains no delay() calls and
// never touches the radio envelope directly - all of that lives in
// radioTask(), driven purely by the hardware timer.
void flushRadioLogs() {
  bool pattern, second;
  uint8_t bits[60]; int patternStation, secondStation;
  uint32_t hz; struct tm snapshot;
  portENTER_CRITICAL(&radioLogMux);
  pattern = radioPatternLogPending; second = radioSecondLogPending;
  if (pattern) { memcpy(bits,radioLogBits,sizeof(bits)); patternStation=radioLogStation; hz=radioLogHz; radioPatternLogPending=false; }
  if (second) { snapshot=radioSecondTm; secondStation=radioSecondStation; radioSecondLogPending=false; }
  portEXIT_CRITICAL(&radioLogMux);
  if (pattern && patternStation >= 0 && patternStation < NUM_STATIONS) {
    Serial.print("PATTERN: ");
    for (int i=0; i<60; ++i) Serial.print(bits[i]);
    Serial.printf("\nPATTERN REPEAT: %s | encoding=%s | frequency=%lu Hz (%.3f kHz) | frame=%lus\n",
       station_names[patternStation], stationEncodingName(patternStation),
       (unsigned long)hz, hz/1000.0, (unsigned long)(patternStation==SN_BPC?20:60));
  }
  if (second && secondStation >= 0 && secondStation < NUM_STATIONS) {
    const char *field=txFieldDescription(secondStation,snapshot.tm_sec);
    Serial.printf("TX %02d:%02d:%02d | %s | %s\n",snapshot.tm_hour,snapshot.tm_min,
                  snapshot.tm_sec,station_short[secondStation],field);
  }
}

void loop() {
  flushRadioLogs();
  static bool silenceErrorReported = false;
  if (rfSilenceFailed && !silenceErrorReported) {
    Serial.println("ERROR: LF carrier could not be silenced; Bluetooth remains OFF");
    silenceErrorReported = true;
  } else if (!rfSilenceFailed) silenceErrorReported = false;
  // WiFi.status() goes through the WiFi driver and loop() runs continuously
  // at high frequency - cache it once per pass rather than calling it
  // multiple times below for what's logically the same "is WiFi up right
  // now" question.
  bool wifiConnectedNow = (WiFi.status() == WL_CONNECTED);

  // Check WiFi connection periodically
  if (ap_mode == false && (millis() - last_wifi_check) > WIFI_CONNECT_CHECK_INTERVAL) {
    last_wifi_check = millis();
    checkWiFiConnection();
  }

  if (ntpSyncEvent.exchange(false)) {
    configureAdaptiveNtp();
  }

  // WiFi power management: in Scheduled/power-save mode, turns the radio
  // off between sync windows. Safe to call every loop() pass - it
  // internally rate-limits itself.
  updateWifiPowerManagement();

  // Bluetooth watch synchronization runs independently of Wi-Fi state.
  serviceBluetoothSync();

  // Flush any pending config change to flash once things have settled
  // (see saveConfig()/writeConfigNow() for why this is debounced).
  if (configDirty && (millis() - configDirtyBecause) >= CONFIG_SAVE_DEBOUNCE_MS) {
    configDirty = false;
    configDirtyBecause = millis();
    writeConfigNow();
  }

  // Onboard LED as a WiFi/AP status indicator (non-blocking - millis()
  // based, no delay()):
  //   - Solid ON : connected to WiFi (normal operation)
  //   - Blinking : AP (configuration) mode, or STA mode not yet connected
  //                (actively trying)
  //   - Off      : Scheduled power-save mode, intentionally sleeping
  //                between sync windows - not an error state, so it's
  //                visually distinct from the "trying to connect" blink.
  bool deliberatelyAsleep = (wifiPowerMode == WIFI_POWER_SCHEDULED) && !wifiRadioEnabled && !ap_mode;
  bool wifiUp = (!ap_mode) && wifiConnectedNow;
  if (deliberatelyAsleep) {
    if (onboardLedState) { onboardLedState = false; digitalWrite(PIN_ONBOARD_LED, LOW); }
  } else if (wifiUp) {
    if (!onboardLedState) { onboardLedState = true; digitalWrite(PIN_ONBOARD_LED, HIGH); }
  } else if (millis() - last_led_blink >= 500) {
    last_led_blink = millis();
    onboardLedState = !onboardLedState;
    digitalWrite(PIN_ONBOARD_LED, onboardLedState ? HIGH : LOW);
  }

  // Handle web server requests (in both AP mode and STA mode)
  if (ap_mode || wifiConnectedNow) {
    server.handleClient();
  }

  yield();// feed watchdog
}


//...................................................................
void starttimer(void)
{
  if (istimerstarted) stoptimer();

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
    Serial.println("ERROR: Failed to create 1 MHz hardware timer");
    return;
  }
  timerAttachInterrupt(tm0, &onTimer);
  timerAlarm(tm0, 1000, true, 0);

  istimerstarted = 1;
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
}

// Called every 0.1s (see radioTask()) to update the actual RF carrier duty
// to match this instant's envelope value: txEnvelope[current second][which
// 0.1s slot]. State 1 = full carrier, state 2 = reduced carrier (the
// station-specific switch below picks how much reduction). MSF has no
// case here deliberately, not by omission: MSF's envelope (see mb_msf())
// uses true on/off keying (0 = carrier fully off, 1 = full carrier) rather
// than a continuous carrier with duty-modulated amplitude reduction like
// the other stations, so its envelope values are never 2 - state 0 already
// falls through to duty=0 correctly via the initial value, and state 1
// takes the RF_DUTY_FULL branch above just like every other station.
void ampchange(void)
{
  if (!radioBleArbiter.rfOwned() || !carrierReady || radioPaused || last_station < 0 || last_station >= NUM_STATIONS) {
    if (carrierReady) ledcWrite(PIN_RADIO, 0);
    ampmod = 0;
    digitalWrite(PIN_LED, LOW);
    return;
  }
  if (nowtm.tm_sec < 0 || nowtm.tm_sec >= 60 || tssec < 0 || tssec >= 10) return;
  uint8_t state = txEnvelope[nowtm.tm_sec][tssec];
  uint32_t duty = 0;

  if (state == 1) {
    duty = RF_DUTY_FULL;
  } else if (state == 2) {
    switch (last_station) {
      case SN_JJY_E:
      case SN_JJY_W: duty = RF_DUTY_JJY_REDUCED; break;
      case SN_WWVB:  duty = RF_DUTY_WWVB_REDUCED; break;
      case SN_DCF77:
      case SN_BSF:   duty = RF_DUTY_DCF_REDUCED; break;
      case SN_BPC:   duty = RF_DUTY_BPC_REDUCED; break;
      default:       duty = 0; break;
    }
  }

  ledcWrite(PIN_RADIO, duty);
  // User feedback represents the modulation envelope: full carrier is ON,
  // reduced/off carrier is OFF. RF duty itself remains exactly as encoded.
  ampmod = (state == 1);
  digitalWrite(PIN_LED, ampmod ? HIGH : LOW);
}


const char* stationEncodingName(int station)
{
  switch (station) {
    case SN_JJY_E:
    case SN_JJY_W: return "JJY pulse-width AM";
    case SN_WWVB:  return "WWVB pulse-width AM";
    case SN_DCF77: return "DCF77 reduced-carrier AM";
    case SN_BSF:   return "BSF experimental - no encoded time data";
    case SN_MSF:   return "MSF A/B OOK";
    case SN_BPC:   return "BPC 2-bit pulse-width";
    default:       return "Unknown";
  }
}

// Return a human-readable description of the information represented by the
// current transmitted second. This is diagnostic output only; it does not
// change the encoded signal.
const char* txFieldDescription(int station, int second)
{
  if (station == SN_BPC) {
    int s = second % 20;
    if (s == 0) return "Sending frame marker";
    if (s == 1) return "Sending block-start second";
    if (s == 3 || s == 4) return "Sending hour";
    if (s >= 5 && s <= 7) return "Sending minute";
    if (s == 8 || s == 9) return "Sending weekday";
    if (s == 10) return "Sending PM flag/parity";
    if (s == 11 || s == 12 || s == 13) return "Sending date";
    if (s == 14 || s == 15) return "Sending month";
    if (s >= 16 && s <= 18) return "Sending year";
    return "Sending status/parity";
  }

  if (station == SN_DCF77) {
    if (second == 0 || second == 59) return "Sending minute marker";
    if (second >= 17 && second <= 20) return "Sending time-zone/status";
    if (second >= 21 && second <= 28) return "Sending minute";
    if (second >= 29 && second <= 35) return "Sending hour";
    if (second >= 36 && second <= 57) return "Sending date";
    if (second == 58) return "Sending date parity";
    return "Sending marker/status";
  }

  if (station == SN_MSF) {
    if (second == 0) return "Sending minute marker";
    if (second >= 17 && second <= 24) return "Sending year";
    if (second >= 25 && second <= 29) return "Sending month";
    if (second >= 30 && second <= 38) return "Sending date";
    if (second >= 39 && second <= 44) return "Sending hour";
    if (second >= 45 && second <= 51) return "Sending minute";
    if (second >= 53 && second <= 58) return "Sending status/parity";
    return "Sending second marker";
  }

  if (station == SN_WWVB) {
    if (second == 0 || second == 9 || second == 19 ||
        second == 29 || second == 39 || second == 49 || second == 59)
      return "Sending position marker";
    if (second >= 1 && second <= 8) return "Sending minute";
    if (second >= 12 && second <= 19) return "Sending hour";
    if (second >= 22 && second <= 33) return "Sending day of year";
    if (second >= 45 && second <= 53) return "Sending year";
    if (second >= 54 && second <= 58) return "Sending status";
    return "Sending marker/status";
  }

  // JJY uses minute, hour, day-of-year, year and weekday fields.
  if (second == 0 || second == 9 || second == 19 ||
      second == 29 || second == 39 || second == 49 || second == 59)
    return "Sending position marker";
  if (second >= 1 && second <= 8) return "Sending minute";
  if (second >= 12 && second <= 19) return "Sending hour";
  if (second >= 22 && second <= 30) return "Sending day of year";
  if (second >= 41 && second <= 49) return "Sending year";
  if (second >= 50 && second <= 52) return "Sending weekday";
  if (second >= 53 && second <= 58) return "Sending status/parity";
  return "Sending marker/status";
}


void printPatternRepeatInfo(int station)
{
  if (station < 0 || station >= NUM_STATIONS) return;
  uint32_t hz = carrierFrequencyHz;
  uint32_t frameSeconds = (station == SN_BPC) ? 20 : 60;
  Serial.printf("PATTERN REPEAT: %s | encoding=%s | frequency=%lu Hz (%.3f kHz) | frame=%lus\n",
                station_names[station], stationEncodingName(station),
                (unsigned long)hz, hz / 1000.0, (unsigned long)frameSeconds);
}

// Switches the active station while the 1 kHz scheduler remains armed:
// silences the carrier, reconfigures LEDC to the station frequency, then
// regenerates the active frame. Called only by radioTask at runtime.
void setstation(int station)
{
  if (!radioBleArbiter.rfOwned() || station < 0 || station >= NUM_STATIONS) return;

  // Only radioTask calls this at runtime; the 1 kHz timer stays armed.
  if (!silenceRfCarrier()) return;

  const uint32_t frequencies[NUM_STATIONS] = {
    40000, 60000, 60000, 77500, 77500, 60000, 68500
  };
  carrierFrequencyHz = frequencies[station];

  // Arduino-ESP32 3.x expects the LEDC clock-source configuration enum here.
  // APB clock provides a stable 80 MHz source for the LF carrier PWM.
  static bool ledcClockConfigured = false;
  if (!ledcClockConfigured) {
    ledcClockConfigured = ledcSetClockSource(LEDC_USE_APB_CLK);
    if (!ledcClockConfigured) {
      Serial.println("ERROR: Could not select LEDC APB clock");
      return;
    }
  }

  bool ok;
  if (!carrierReady) {
    ok = ledcAttachChannel(PIN_RADIO, carrierFrequencyHz,
                           RF_PWM_RESOLUTION, RF_PWM_CHANNEL);
    carrierReady = ok;
  } else {
    ok = ledcChangeFrequency(PIN_RADIO, carrierFrequencyHz,
                             RF_PWM_RESOLUTION) != 0;
  }

  if (!ok) {
    // If a live frequency change failed, detach the stale channel so a later
    // station selection can make a clean ledcAttachChannel() attempt.
    if (carrierReady) ledcDetach(PIN_RADIO);
    carrierReady = false;
    Serial.printf("ERROR: LEDC could not configure %lu Hz\n",
                  (unsigned long)carrierFrequencyHz);
    return;
  }

  ledcWrite(PIN_RADIO, 0);
  last_station = station;
  makebitpattern = st_makebits[station];  // selects mb_jjy()/mb_wwvb()/mb_dcf()/mb_msf()/mb_bpc() for this station
  makebitpattern();
  ampchange();
  queuePatternLog(station);
}

//...................................................................
// makeup bit pattern for current date, hour:min

// Arduino-ESP32's newlib does not ship the IANA zoneinfo database, so
// setting TZ to an IANA name like "Asia/Tokyo" does NOT apply the correct
// UTC offset or DST rule - tzset() silently falls back to UTC when it can't
// parse the TZ string as a POSIX rule. To make the "Time zone" selector in
// the UI actually control both the displayed clock AND the transmitted
// time, each zone offered in the UI is mapped here to an equivalent POSIX
// TZ rule, which tzset()/localtime_r() parse natively (DST transitions
// included).
static bool validTimezoneName(const String &zoneName)
{
  return zoneName == "Australia/Brisbane" || zoneName == "Australia/Sydney" ||
         zoneName == "Asia/Tokyo" || zoneName == "Asia/Shanghai" ||
         zoneName == "Europe/London" || zoneName == "America/New_York" ||
         zoneName == "America/Los_Angeles" || zoneName == "UTC";
}

static String posixTzFor(const String &zoneName)
{
  if (zoneName == "Australia/Brisbane")  return "AEST-10";                                  // no DST
  if (zoneName == "Australia/Sydney")    return "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
  if (zoneName == "Asia/Tokyo")          return "JST-9";                                    // no DST
  if (zoneName == "Asia/Shanghai")       return "CST-8";                                    // no DST
  if (zoneName == "Europe/London")       return "GMT0BST,M3.5.0/1,M10.5.0/2";
  if (zoneName == "America/New_York")    return "EST5EDT,M3.2.0/2,M11.1.0/2";
  if (zoneName == "America/Los_Angeles") return "PST8PDT,M3.2.0/2,M11.1.0/2";
  if (zoneName == "UTC")                 return "UTC0";
  return "UTC0";  // unknown zone: fall back safely rather than silently mis-timing every transmission
}

// Applies the configured time zone to the C library. Used both for the
// on-screen clock (getlocaltime()/nowtm) and for the time actually encoded
// into the radio signal (stationTime() below), so the two always agree.
static void applyTimezone(void)
{
  setenv("TZ", posixTzFor(timezone_name).c_str(), 1);
  tzset();
}

// The time encoded into the transmission is always the local time of the
// zone selected in the "Time zone" card, plus the user's "Transmission
// offset" (which is relative to THAT zone, not to UTC). This is
// deliberately the same for every station: switching between JJY, WWVB,
// DCF77, MSF or BPC changes only the bit-level FORMAT the time is encoded
// in, never which moment in time gets encoded.
//
// (JJY/BPC previously added their real-world stations' fixed civil-time
// offset - JST/CST - and DCF77/MSF previously computed German/UK DST via
// hand-rolled EU-rule math independent of the UI's "Time zone" selector.
// That made the transmitted time depend on which station was selected,
// which is exactly what should NOT happen: the "Time zone" card is now the
// single source of truth for what time is transmitted, for every format.)
// Computes the actual moment in time to encode into the radio frame:
// current UTC, optionally advanced by one minute (nextMinute - DCF77 and
// MSF transmit data for the UPCOMING minute, JJY/WWVB/BPC transmit the
// CURRENT one), plus the user's transmission offset, all converted to
// local civil time via the currently-configured TZ (see applyTimezone()).
// The station parameter is intentionally unused: per an earlier design
// change, which station/encoding is selected never changes WHICH moment
// gets transmitted, only how it's formatted into bits - every station
// transmits the same selected-timezone-plus-offset time.
static void stationTime(time_t utc, int station, bool nextMinute, struct tm &out)
{
  (void)station;  // encoding format no longer changes which time is used
  time_t tx = utc + (nextMinute ? 60 : 0);
  tx += (time_t)transmission_offset_minutes * 60;
  localtime_r(&tx, &out);  // honors the POSIX TZ rule set by applyTimezone(), including DST
}

// Zeroes both transmit buffers. Called at the start of every mb_*()
// encoder before it writes this minute's frame, so any bit position an
// encoder doesn't explicitly set (reserved/unused bits) is guaranteed to
// come out as 0 rather than leftover data from whichever station was
// selected previously.
static void clearTxFrame(void)
{
  memset(txSymbol, 0, sizeof(txSymbol));
  memset(txEnvelope, 0, sizeof(txEnvelope));
}

// Converts txSymbol[60] (already filled in by mb_jjy()) into the 0.1s-
// resolution envelope ampchange() actually drives onto the RF carrier.
// JJY pulse shape (verified against NICT's spec): a "0" bit is 0.8s at
// full carrier then 0.2s reduced; a "1" bit is 0.5s/0.5s; a marker (the
// seven required sync pulses at seconds 0,9,19,29,39,49,59) is 0.2s/0.8s.
// Envelope values: 1 = full carrier, 2 = reduced carrier (see ampchange()
// for how these map to actual PWM duty).
static void buildJjyEnvelope(void)
{
  for (int s = 0; s < 60; ++s) {
    int marker = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59);
    int highTenths = marker ? 2 : (txSymbol[s] ? 5 : 8);
    for (int k = 0; k < 10; ++k)
      txEnvelope[s][k] = (k < highTenths) ? 1 : 2;
  }
}

// WWVB's equivalent of buildJjyEnvelope() - same marker positions and
// pulse-shape ratios as JJY, but WWVB's amplitude convention is inverted
// (the modulated/"reduced" portion of each pulse comes FIRST rather than
// last), hence lowTenths counting from the start of the second instead of
// highTenths counting the full-carrier portion.
static void buildWwvbEnvelope(void)
{
  for (int s = 0; s < 60; ++s) {
    int marker = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59);
    int lowTenths = marker ? 8 : (txSymbol[s] ? 5 : 2);
    for (int k = 0; k < 10; ++k)
      txEnvelope[s][k] = (k < lowTenths) ? 2 : 1;
  }
}

// Shared BCD field encoder for the minute/hour/day-of-year fields that JJY
// and WWVB both lay out identically (verified independently against each
// station's own spec - the bit positions happen to coincide, this isn't an
// approximation). Writes MSB-first BCD digits via binarize(): minute tens/
// ones at bits 1-3/5-8, hour tens/ones at 12-13/15-18, day-of-year hundreds/
// tens/ones at 22-23/25-28/30-33. t.tm_yday is 0-indexed in C, so +1 gives
// the 1-based day-of-year both specs expect. Callers still need to fill in
// everything station-specific themselves (markers, parity, year, weekday,
// DST/leap-second flags) - this only covers the fields both stations share.
static void encodeCommon(const struct tm &t)
{
  binarize(t.tm_min / 10, 1, 3);
  binarize(t.tm_min % 10, 5, 4);
  binarize(t.tm_hour / 10, 12, 2);
  binarize(t.tm_hour % 10, 15, 4);
  int doy = t.tm_yday + 1;
  binarize(doy / 100, 22, 2);
  binarize((doy / 10) % 10, 25, 4);
  binarize(doy % 10, 30, 4);
}

void mb_jjy(void)
{
  // JJY LF standard (NICT):
  //   40 kHz (Fukushima) / 60 kHz (Hagane-yama)
  //   JST = UTC+9
  //   60 seconds/frame
  //   M/P markers = 0.2 s
  //   binary 0 = 0.8 s
  //   binary 1 = 0.5 s
  //   Minute field: 1..8, PA2: 37
  //   Hour field:   12..18, PA1: 36
  //   Day-of-year: 22..30
  //   Year: 41..48
  //   Weekday: 50..52
  //   LS1/LS2: 53/54
  //   SU1/SU2: 40/20
  //   ST1..ST6: 55..58
  //
  // This encoder intentionally transmits the ordinary time-code frame.
  // NICT replaces part of the ordinary frame with call-sign Morse at :15/:45.
  // This local receiver emulator intentionally keeps the ordinary frame continuous
  // because that is more useful for watch synchronization; it is not a literal JJY broadcast then.
  clearTxFrame();
  for (int s = 0; s < 60; ++s)
    txSymbol[s] = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59) ? SP_M : SP_0;

  time_t utc; time(&utc);
  struct tm t;
  stationTime(utc, last_station, false, t);
  encodeCommon(t);
  // NICT JJY parity positions (confirmed against the authoritative NICT
  // table): PA1 = second 36, even parity over the hour field (positions
  // 12-18, 7 bits). PA2 = second 37, even parity over the minute field
  // (positions 1-8, 8 bits).
  //
  // A previous revision of this file moved these to positions 19/36,
  // apparently on the mistaken belief that 19 held a parity bit. Position
  // 19 is actually the P2 POSITION MARKER (0.2s pulse, part of the 7
  // required sync markers at seconds 0,9,19,29,39,49,59) - overwriting it
  // with a data bit destroyed a marker a real receiver depends on to stay
  // frame-synchronized. That revision also computed the minute parity over
  // only 7 bits (positions 1-7), silently excluding position 8 (the
  // minute-ones "1" weight bit) from the checksum. Both are fixed here.
  txSymbol[36] = parity(12, 7);
  txSymbol[37] = parity(1, 8);
  binarize((t.tm_year - 100) / 10, 41, 4);
  binarize((t.tm_year - 100) % 10, 45, 4);
  binarize(t.tm_wday, 50, 3);

  // JJY reserved/control bits:
  // 40 = SU2 (daylight-saving-time state), normally 0 in Japan.
  // 53 = LS1 (leap-second announcement), 54 = LS2 (insert/remove).
  // 55-58 = service interruption information; all zero during normal service.
  // No leap second is currently scheduled, so all of these remain zero.
  txSymbol[40] = 0;
  txSymbol[53] = 0;
  txSymbol[54] = 0;
  txSymbol[55] = 0;
  txSymbol[56] = 0;
  txSymbol[57] = 0;
  txSymbol[58] = 0;

  buildJjyEnvelope();
}

void mb_wwvb(void)
{
  clearTxFrame();
  for (int s = 0; s < 60; ++s)
    txSymbol[s] = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59) ? SP_M : SP_0;

  time_t utc; time(&utc);
  struct tm t;
  // Previously used gmtime_r() directly, i.e. always transmitted raw UTC
  // regardless of the "Time zone" card. Now transmits the same
  // selected-timezone civil time as every other station.
  stationTime(utc, SN_WWVB, false, t);
  encodeCommon(t);
  binarize((t.tm_year - 100) / 10, 45, 4);
  binarize((t.tm_year - 100) % 10, 50, 4);

  // DST bit reflects whether the SELECTED transmission timezone is
  // currently observing daylight saving, not US Mountain Time specifically.
  // tm_isdst comes straight from localtime_r() using the POSIX TZ rule
  // applyTimezone() configured for the selected zone.
  bool dst = t.tm_isdst > 0;

  // WWVB legacy AM/PWM control bits.
  // 55 = leap-year indicator; 56 = leap-second warning.
  // 57/58 = DST state. UT1 correction (36-43) remains zero because ordinary
  // NTP does not provide UT1-UTC to this firmware.
  bool leapYear = ((t.tm_year + 1900) % 4 == 0 &&
                   ((t.tm_year + 1900) % 100 != 0 || (t.tm_year + 1900) % 400 == 0));
  txSymbol[55] = leapYear ? SP_1 : SP_0;
  txSymbol[56] = SP_0;
  txSymbol[57] = dst ? SP_1 : SP_0;
  txSymbol[58] = dst ? SP_1 : SP_0;

  // DUT1 sign (bits 36-38) must be the pattern 101 (positive) or 010
  // (negative) per spec - an all-zero pattern is not a valid sign code.
  // Since ordinary NTP doesn't supply UT1-UTC to this firmware, DUT1 is
  // fixed at a nominal +0.0s (magnitude bits 40-43 stay zero).
  txSymbol[36] = SP_1;
  txSymbol[37] = SP_0;
  txSymbol[38] = SP_1;

  buildWwvbEnvelope();
}

void mb_dcf(void)
{
  clearTxFrame();
  time_t utc; time(&utc);
  struct tm t;
  stationTime(utc, SN_DCF77, true, t);

  // CET/CEST bit reflects the selected transmission timezone's own DST
  // state (tm_isdst from stationTime()'s localtime_r() call), not a
  // hand-rolled EU-only DST calculation. For a non-European zone this just
  // means the flag tracks that zone's DST, which is the correct behaviour
  // now that station selection never changes which time is transmitted.
  bool dst = t.tm_isdst > 0;

  // DCF77 transmits the following minute. Bits 1-14 are operational/public-warning
  // data outside the scope of this emulator and remain zero.
  txSymbol[15] = 0;
  txSymbol[16] = 0;
  // Z1 (bit 17) / Z2 (bit 18) time-zone bits, per PTB spec: Z1=1 indicates
  // CEST (DST) in effect; Z2=1 indicates CET (standard time) in effect.
  // (Z1,Z2)=(0,1) -> CET (winter); (1,0) -> CEST (summer).
  // A previous revision had these both swapped AND inverted (assigning CET
  // to bit 17 and CEST to bit 18, backwards from spec, and with the wrong
  // polarity), which would have told any real receiver the opposite
  // zone/DST state from what was actually being transmitted.
  txSymbol[17] = dst ? 1 : 0; // Z1: CEST
  txSymbol[18] = dst ? 0 : 1; // Z2: CET
  txSymbol[19] = 0;           // No leap-second announcement available from POSIX NTP
  txSymbol[20] = 1;           // Start of time information

  rbcdize(t.tm_min, 21, 7);
  txSymbol[28] = parity(21, 7);
  rbcdize(t.tm_hour, 29, 6);
  txSymbol[35] = parity(29, 6);
  rbcdize(t.tm_mday, 36, 6);
  int isoDow = t.tm_wday == 0 ? 7 : t.tm_wday;
  rbinarize(isoDow, 42, 3);
  rbcdize(t.tm_mon + 1, 45, 5);
  rbcdize(t.tm_year - 100, 50, 8);
  txSymbol[58] = parity(36, 22);

  for (int s = 0; s < 59; ++s) {
    int reduction = (s == 20) ? 2 : (txSymbol[s] ? 2 : 1);
    for (int k = 0; k < 10; ++k)
      txEnvelope[s][k] = (k < reduction) ? 2 : 1;
  }
  // DCF77 second 59 is the minute marker: no carrier reduction is emitted
  // during that second in the normal frame, so the envelope remains full.
  for (int k = 0; k < 10; ++k) txEnvelope[59][k] = 1;
}

void mb_bsf(void)
{
  clearTxFrame();
  for (int s = 0; s < 60; ++s)
    for (int k = 0; k < 10; ++k) txEnvelope[s][k] = 1;
}


void mb_msf(void)
{
  clearTxFrame();

  time_t utc; time(&utc);
  struct tm t;
  stationTime(utc, SN_MSF, true, t);

  // DST bit (B58) reflects the selected transmission timezone's own DST
  // state rather than a hardcoded UK-only calculation.
  bool dst = t.tm_isdst > 0;

  // MSF encodes the following UK civil-time minute.
  // Each second has two 100 ms data slots: A at 100-200 ms and B at 200-300 ms.
  // Logic 0 is carrier ON; logic 1 is carrier OFF.
  uint8_t A[60] = {0};
  uint8_t B[60] = {0};

  auto setBcdMsb = [](uint8_t *dstBits, int value, int pos, int len) {
    int p = pos + len - 1;
    int remaining = len;
    while (remaining > 0) {
      int n = remaining >= 4 ? 4 : remaining;
      int digit = value % 10;
      for (int i = 0; i < n; ++i) {
        dstBits[p - i] = digit & 1;
        digit >>= 1;
      }
      value /= 10;
      p -= n;
      remaining -= n;
    }
  };

  // MSF field allocation from NPL:
  // A17-A24 year, A25-A29 month, A30-A35 day, A36-A38 weekday,
  // A39-A44 hour, A45-A51 minute.
  setBcdMsb(A, t.tm_year - 100, 17, 8);
  setBcdMsb(A, t.tm_mon + 1, 25, 5);
  setBcdMsb(A, t.tm_mday, 30, 6);
  int dow = t.tm_wday; // NPL: Sunday=0 ... Saturday=6.
  A[36] = (dow >> 2) & 1;
  A[37] = (dow >> 1) & 1;
  A[38] = dow & 1;
  setBcdMsb(A, t.tm_hour, 39, 6);
  setBcdMsb(A, t.tm_min, 45, 7);

  // DUT1 is not available from ordinary NTP time, so A/B DUT1 fields remain zero.
  // NPL specifies odd parity in B54-B57.
  auto oddParityBit = [](uint8_t *a, int first, int last) -> uint8_t {
    int ones = 0;
    for (int i = first; i <= last; ++i) ones += a[i] ? 1 : 0;
    return (ones & 1) ? 0 : 1;
  };
  B[54] = oddParityBit(A, 17, 24);
  B[55] = oddParityBit(A, 25, 35);
  B[56] = oddParityBit(A, 36, 38);
  B[57] = oddParityBit(A, 39, 51);
  B[58] = dst ? 1 : 0;
  B[53] = 0;

  // Fixed A-channel sync pattern: per NPL's spec, A53 through A58 are
  // PERMANENTLY set to 1 (with A52 and A59 fixed at 0, already satisfied
  // since A[] defaults to zero and is never written there). Together this
  // forms the fixed bit sequence 0,1,1,1,1,1,1,0 across A52-A59, which
  // never occurs elsewhere in the A channel and gives receivers a second,
  // independent way to confirm the second-00 minute marker rather than
  // relying solely on the 500ms marker pulse. This was previously left at
  // the default 0 - a real receiver checking for this pattern would not
  // have been able to use it to confirm frame sync.
  for (int i = 53; i <= 58; ++i) A[i] = 1;

  // Second 00 is the 500 ms minute marker.
  for (int k = 0; k < 10; ++k) txEnvelope[0][k] = (k < 5) ? 0 : 1;

  for (int s = 1; s < 60; ++s) {
    txEnvelope[s][0] = 0;              // mandatory 100 ms second marker
    txEnvelope[s][1] = A[s] ? 0 : 1;   // bit A
    txEnvelope[s][2] = B[s] ? 0 : 1;   // bit B
    for (int k = 3; k < 10; ++k) txEnvelope[s][k] = 1;
    txSymbol[s] = (A[s] << 1) | B[s];
  }
}

void mb_bpc(void)
{
  clearTxFrame();
  time_t utc; time(&utc);
  struct tm t;
  stationTime(utc, SN_BPC, false, t);

  int blockSecond = (t.tm_sec / 20) * 20;
  int hour12 = t.tm_hour % 12;
  int pm = t.tm_hour >= 12;
  int minute = t.tm_min;
  int day = t.tm_mday;
  int month = t.tm_mon + 1;
  int year = t.tm_year - 100;
  int dow = t.tm_wday == 0 ? 7 : t.tm_wday;

  uint8_t pair[20][2] = {};

  // BPC's exact bit layout is not officially published by NTSC/China (it
  // requires a commercial license); the assignments below are reconstructed
  // from the community-derived table on Wikipedia's "BPC (time signal)"
  // article, which cross-checks against an actual captured/decoded
  // waterfall recording rather than being purely theoretical. Treat this as
  // the best publicly available reference rather than an official spec.
  //
  // Verified per-second layout (block-relative seconds, MSbit/LSbit):
  //   01: 40,20  block-start second (00/20/40)   02: unused
  //   03-04: 8,4,2,1  hour-of-halfday (0-11)
  //   05-07: 32,16,8,4,2,1  minute (0-59, straight binary, not BCD)
  //   08: unused,4   09: 2,1        -> weekday (4,2,1 = 1-7, Mon=1..Sun=7)
  //   10: PM flag,P1(even parity over seconds 01-09)
  //   11: unused,16  12-13: 8,4,2,1 -> day of month (16,8,4,2,1 = 1-31)
  //   14-15: 8,4,2,1  month (1-12)
  //   16-18: 32,16,8,4,2,1  year (0-99, straight binary)
  //   19: 64(year MSB),P2(even parity over seconds 11-18)
  //
  // A previous revision of this encoder used a different, non-matching
  // layout: it placed the PM flag/P1 parity at second 1 instead of 10, put
  // the block-start-second field at seconds 10-11 instead of 1, never wrote
  // the weekday's weight-4 bit or the block-start field's low bit at all,
  // and - most seriously - never wrote the day-of-month's weight-1 (LSB)
  // bit, meaning every odd/even day pair (e.g. the 14th and 15th) would
  // have been transmitted identically. All of that is corrected here.
  int bsCode = blockSecond / 20; // 0, 1, or 2 -> encodes 00/20/40 as 2 bits
  pair[1][0] = (bsCode >> 1) & 1;
  pair[1][1] = bsCode & 1;
  pair[3][0] = (hour12 >> 3) & 1; pair[3][1] = (hour12 >> 2) & 1;
  pair[4][0] = (hour12 >> 1) & 1; pair[4][1] = hour12 & 1;
  pair[5][0] = (minute >> 5) & 1; pair[5][1] = (minute >> 4) & 1;
  pair[6][0] = (minute >> 3) & 1; pair[6][1] = (minute >> 2) & 1;
  pair[7][0] = (minute >> 1) & 1; pair[7][1] = minute & 1;
  pair[8][1] = (dow >> 2) & 1;
  pair[9][0] = (dow >> 1) & 1; pair[9][1] = dow & 1;
  pair[10][0] = pm;
  pair[11][1] = (day >> 4) & 1;
  pair[12][0] = (day >> 3) & 1; pair[12][1] = (day >> 2) & 1;
  pair[13][0] = (day >> 1) & 1; pair[13][1] = day & 1;
  pair[14][0] = (month >> 3) & 1; pair[14][1] = (month >> 2) & 1;
  pair[15][0] = (month >> 1) & 1; pair[15][1] = month & 1;
  pair[16][0] = (year >> 5) & 1; pair[16][1] = (year >> 4) & 1;
  pair[17][0] = (year >> 3) & 1; pair[17][1] = (year >> 2) & 1;
  pair[18][0] = (year >> 1) & 1; pair[18][1] = year & 1;

  int p1 = 0;
  for (int s = 1; s <= 9; ++s) p1 ^= pair[s][0] ^ pair[s][1];
  pair[10][1] = p1;

  int p2 = 0;
  for (int s = 11; s <= 18; ++s) p2 ^= pair[s][0] ^ pair[s][1];
  pair[19][0] = (year >> 6) & 1;
  pair[19][1] = p2;

  // Each 20-second block starts with an unmodulated second, then 100/200/300/400 ms
  // reduced-carrier symbols representing 00/01/10/11.
  for (int block = 0; block < 3; ++block) {
    int base = block * 20;
    for (int s = 0; s < 20; ++s) {
      int sec = base + s;
      if (s == 0) {
        for (int k = 0; k < 10; ++k) txEnvelope[sec][k] = 1;
        txSymbol[sec] = SP_M4;
      } else {
        int symbol = (pair[s][0] << 1) | pair[s][1];
        txSymbol[sec] = symbol;
        int reduced = symbol + 1;
        for (int k = 0; k < 10; ++k)
          txEnvelope[sec][k] = (k < reduced) ? 2 : 1;
      }
    }
  }
}

// Writes v as an MSB-first ("big-endian" bit order) binary value: the most
// significant bit lands at txSymbol[pos], the least significant at
// txSymbol[pos+len-1]. Used by JJY/WWVB for their weighted-digit fields
// (see encodeCommon()) where the highest-weight bit naturally comes first.
// (A previous revision's comment here said "little endian", which is
// backwards from the actual bit order produced - fixed.)
void
binarize(int v, int pos, int len)
{
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    txSymbol[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// Writes v as an LSB-first ("little-endian" bit order) binary value: the
// least significant bit lands at txSymbol[pos], the most significant at
// txSymbol[pos+len-1]. Used by DCF77 (see mb_dcf()/rbcdize()), which
// transmits its BCD digits least-significant-bit-first per PTB's spec.
// (A previous revision's comment here said "(big endian)", backwards from
// the actual bit order produced - fixed.)
void
rbinarize(int v, int pos, int len)
{
  for ( ; 0 < len; pos++, len--) {
    txSymbol[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// LSB-first BCD: splits v into decimal digits (ones digit first, each
// digit itself written LSB-first via rbinarize()), matching DCF77's actual
// on-air bit order. This is what mb_dcf() uses for all of its BCD fields
// (minute, hour, day, month, year).
void
rbcdize(int v, int pos, int len)
{
  int l;

  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    rbinarize(v % 10, pos, l);
    v = v / 10;
    pos = pos + l;
    len = len - l;
  }
  return;
}

// Computes even parity (the return value, XOR'd with the data, always
// yields an even total number of 1 bits) over txSymbol[pos..pos+len-1].
// Used for every station's parity/checksum bits: JJY's PA1/PA2, WWVB's
// (implicit, via its own checks), DCF77's P1/P2/P3, and MSF's B54-B57 -
// see each mb_*() function for exactly which span it covers and why, all
// individually verified against that station's own specification.
int
parity(int pos, int len)
{
  int s = 0;
  
  for (pos; 0 < len; pos++, len--) {
    s += txSymbol[pos];
  }
  return (s % 2);
}


//// calculate doy (day of year)/dow (day of week) from YY/MM/DD
//void
//setdoydow(void)
//{
//  int j0 = julian(tyear, 1, 1);      // new year day of this year
//  int j1 = julian(tyear, tmon, tday);
//  tdoy = j1 - j0 + 1; // 1..365/366
//  tdow = j1 % 7;      // 0..6
//  return;
//}
//
//// return julian date (? relative date from a day)
//// sunday is multiple of 7
//int
//julian(int y, int m, int d)
//{
//  if (m <= 2) {
//    m = m + 12;
//    y--;
//  }
//  return y * 1461 / 4 + (m + 1) * 153 / 5 + d + 6;
////  1461 / 4 == 365.25, 153 / 5 == 30.6
//}
//
//// increment tday-tmon-tyear, tdoy, tdow
//void
//incday(void)
//{
//  int year1 = tyear;   // year of next month
//  int mon1 = tmon + 1; // next month
//  if (12 < mon1) {
//    mon1 = 1;
//    year1++;
//  }
//  int day1 = tday + 1; // date# of tomorrow
//  if (julian(year1, mon1, 1) - julian(tyear, tmon, 1) < day1) {
//    tday = 1;  // date# exceeds # of date in this month
//    tmon = mon1;
//    tyear = year1;
//  } else {
//    tday = day1;
//  }
//  setdoydow(); // tdoy, tdow is updated from tyear-tmonth-tday
//  return;
//}

void
printbits60(void)
{
  Serial.print("\n");
  for (int i = 0; i < 60; i++) {
    Serial.print(txSymbol[i]);
  }
  Serial.print("\n");
  return;
}

// SNTP library callback, fired automatically every time a sync completes
// (see sntp_set_time_sync_notification_cb() in ntpstart()). Compares how
// far the wall clock had drifted from what it "should" say (based on the
// board's own monotonic timer) since the previous sync, and folds that
// into a smoothed drift estimate. Only sets a flag (ntpSyncEvent) rather
// than doing any real work itself, since this runs in SNTP's own context;
// the actual interval recalculation happens in loop() via
// configureAdaptiveNtp(), called when that flag is seen.
void onNtpSync(struct timeval *tv)
{
  uint64_t mono = (uint64_t)esp_timer_get_time();
  int64_t epochUs = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;

  if (ntpLastMonoUs != 0) {
    uint64_t elapsed = mono - ntpLastMonoUs;
    if (elapsed > 30000000ULL) {
      int64_t expected = ntpLastEpochUs + (int64_t)elapsed;
      int64_t correction = epochUs - expected;
      double ppm = ((double)correction * 1000000.0) / (double)elapsed;

      portENTER_CRITICAL(&clockMux);
      ntpDriftPpm = (ntpDriftPpm == 0.0)
        ? ppm
        : ntpDriftPpm * (1.0 - NTP_DRIFT_EMA_ALPHA) + ppm * NTP_DRIFT_EMA_ALPHA;
      ntpDriftAbsSecPerHour = fabs(ntpDriftPpm) * 3600.0 / 1000000.0;
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
  ntpSyncEvent = true;
}

// Recalculates ntpIntervalSec from the current drift estimate and applies
// it to the running SNTP client. Called from loop() shortly after each
// sync (see onNtpSync()/ntpSyncEvent). Larger measured drift -> shorter
// interval (resync sooner, before error can accumulate); smaller drift ->
// longer interval, up to NTP_MAX_INTERVAL_SEC.
void configureAdaptiveNtp(void)
{
  portENTER_CRITICAL(&clockMux);
  double samplePpm=ntpDriftPpm;
  portEXIT_CRITICAL(&clockMux);
  double drift = fabs(samplePpm) / 1000000.0;
  if (drift < 1e-9) {
    ntpIntervalSec = NTP_INITIAL_INTERVAL_SEC;
  } else {
    double seconds = NTP_ERROR_BUDGET_SEC / drift;
    if (seconds < NTP_MIN_INTERVAL_SEC) seconds = NTP_MIN_INTERVAL_SEC;
    if (seconds > NTP_MAX_INTERVAL_SEC) seconds = NTP_MAX_INTERVAL_SEC;
    ntpIntervalSec = (uint32_t)seconds;
  }

  sntp_set_sync_interval(ntpIntervalSec * 1000UL);
  sntp_restart();

}

void ntpstart(void)
{
  // Full WiFi-connect + NTP-sync sequence: connects to the configured SSID
  // (blocking, up to ~10s), then configures and waits for a first NTP fix
  // (blocking, up to another ~20s). Called once at boot, and again by
  // updateWifiPowerManagement() every time it wakes the radio for a
  // scheduled sync window. Safe to call repeatedly - each call fully
  // reconfigures SNTP and re-applies the timezone, so it behaves the same
  // whether it's the first call after boot or the fifth call of the day.
  Serial.print("Attempting to connect to Network named: ");
  Serial.println(ssid);
  wifi_connect_start=millis();
  WiFi.begin(ssid, passwd);
  WiFi.setSleep(wifiPowerMode != WIFI_POWER_ALWAYS_ON);

  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; ++i) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) {
    ntpsync = 0;
    Serial.println("\nWiFi connection failed");
    return;
  }

  sntp_set_time_sync_notification_cb(onNtpSync);
  // IMMED avoids calling the time trusted while an SNTP slew is still ongoing.
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  // configTzTime() configures SNTP with the selected POSIX TZ rule directly.
  // When NTP is restarted later by Scheduled Wi-Fi power management, pause
  // radioTask first so setenv()/tzset() cannot race localtime() while a frame
  // is being generated. At boot radioTask does not exist yet, so no pause is
  // needed.
  bool pausedForTimeConfig = false;
  if (radioTaskHandle != NULL) {
    if (!radioSetPaused(true)) {
      ntpsync = 0;
      Serial.println("\nERROR: Could not pause RF for NTP/timezone configuration");
      return;
    }
    pausedForTimeConfig = true;
  }

  String tzRule = posixTzFor(timezone_name);
  configTzTime(tzRule.c_str(), "pool.ntp.org", "time.nist.gov");

  if (pausedForTimeConfig) {
    if (!radioSetPaused(false)) {
      ntpsync = 0;
      Serial.println("\nERROR: Could not resume RF after NTP/timezone configuration");
      return;
    }
  }
  radioRequestRefresh();
  sntp_set_sync_interval(NTP_INITIAL_INTERVAL_SEC * 1000UL);

  struct tm ntpProbe = {};
  for (int i = 0; i < 20 && !getLocalTime(&ntpProbe, 1000); ++i) {
    Serial.print(".");
  }

  if (getLocalTime(&ntpProbe, 1000)) {
    ntpsync = 1;
    Serial.println("\nNTP synchronized");
  } else {
    ntpsync = 0;
    Serial.println("\nNTP synchronization failed");
  }
}

// Cleanly shuts down networking: stops the SNTP client, disconnects WiFi,
// and powers the radio off entirely (WIFI_OFF, not just disconnected - this
// actually powers down the WiFi hardware for real power savings). Used by
// updateWifiPowerManagement() to end a scheduled sync window in Power-save
// mode.
void
ntpstop(void)
{
  // Wi-Fi sleep does not invalidate a previously synchronized holdover clock.
  esp_sntp_stop();
  if (webServerStarted) { server.stop(); webServerStarted=false; }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}


void
setlocaltime(void)
{
  time_t nowtime = mktime(&nowtm) /*+ TZ */;
  struct timeval tv = {
    .tv_sec = nowtime
  };
  settimeofday(&tv, NULL);
  getlocaltime(); // to make wday/yday
}


void
getlocaltime(void)
{
  // Deliberately NOT using the Arduino-ESP32 getLocalTime() helper here: it
  // silently blocks the caller for up to 5 seconds if system time has not
  // yet been set (e.g. before the first successful NTP reply). Since this
  // function is called once per second from the high-priority radio task,
  // any such stall would freeze the transmitted time code and the RF
  // envelope for seconds at a time. time()/localtime_r() never block and
  // honour the TZ environment variable set via setenv()/tzset(), which is
  // all we need once the timezone has been applied in setup().
  time_t nowtime;
  time(&nowtime);
  localtime_r(&nowtime, &nowtm);
}

//...................................................................
// New functions for web UI and configuration

// Flash-wear mitigation #2: uses LittleFS rather than SPIFFS. SPIFFS is
// deprecated by Espressif for exactly the reasons relevant here - it has
// weaker wear-leveling than LittleFS, and a write interrupted by power loss
// (e.g. someone unplugs the board mid-save) can corrupt the whole file
// rather than just losing the in-progress write. LittleFS is a drop-in
// replacement in modern Arduino-ESP32 with the same File/open()/read API
// used below, so no other code needed to change.

//...................................................................
// GW-BX5600 BLE protocol implementation

static bool btWatchNameMatches(int protocol, const String &advertisedName) {
  String name = advertisedName;
  name.toUpperCase();
  // Some GW-BX5600 advertisements have no complete local name. Retain the
  // original MIP discovery behaviour in that case; Standard/Analogue require
  // positive model identification before a time packet is ever written.
  if (!name.length()) return protocol == BT_PROTOCOL_BX5600_MIP;
  if (protocol == BT_PROTOCOL_BX5600_MIP) return name.indexOf("GW-BX5600") >= 0;
  if (protocol == BT_PROTOCOL_STANDARD) {
    const char *models[] = {"GW-B5600", "GMW-B5000", "GA-B2100", "DW-B5600",
                            "GM-B2100", "GST-B500", "MSG-B100", "G-B001",
                            "MRG-B5000", "GCW-B5000", "ECB-10", "ECB-20", "ECB-30"};
    for (const char *model : models) if (name.indexOf(model) >= 0) return true;
    return false;
  }
  if (protocol == BT_PROTOCOL_ANALOGUE) {
    const char *models[] = {"MTG-B1000", "MTG-B3000", "MTG-B3100", "GST-B100"};
    for (const char *model : models) if (name.indexOf(model) >= 0) return true;
    return false;
  }
  return false;
}

class GShockScanCallbacks : public NimBLEScanCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    if (!advertisedDevice || !btWindowActive || btBleBusy) return;
    if (!advertisedDevice->haveServiceUUID() ||
        !advertisedDevice->isAdvertisingService(NimBLEUUID(CASIO_ADVERTISEMENT_SERVICE))) return;

    String name = advertisedDevice->haveName() ? String(advertisedDevice->getName().c_str()) : "";
    portENTER_CRITICAL(&btDiscoveryMux);
    uint8_t protocol = btDiscoveryProtocol;
    uint32_t generation = btDiscoveryGeneration;
    portEXIT_CRITICAL(&btDiscoveryMux);
    if (!btWatchNameMatches(protocol, name)) return;
    const String address = advertisedDevice->getAddress().toString().c_str();

    portENTER_CRITICAL(&btDiscoveryMux);
    bool expectedMatches = !btExpectedProfileAddress[0] ||
                           strcasecmp(btExpectedProfileAddress, address.c_str()) == 0;
    if (generation == btDiscoveryGeneration && btWindowActive && expectedMatches && !btDiscoveryReady) {
      strlcpy(btDiscoveredName, name.c_str(), sizeof(btDiscoveredName));
      strlcpy(btDiscoveredAddress, address.c_str(), sizeof(btDiscoveredAddress));
      btDiscoveredAddressType = advertisedDevice->getAddressType();
      btDiscoveryReady = true;
    }
    portEXIT_CRITICAL(&btDiscoveryMux);
  }
};

// NimBLE calls onDisconnect before its GAP handler clears the connection
// handle and releases any blocked GATT procedure. Publish the reuse barrier
// from a subsequent host event, rather than from inside that callback.
static std::atomic<bool> btClientConnected{false};
static std::atomic<bool> btClientConnectionPending{false};
static std::atomic<bool> btClientQuiescentReady{true};
static ble_npl_event btClientTeardownBarrierEvent{};
static ble_npl_callout btClientPreemptionCallout{};
static bool btClientBarrierEventInitialized = false;
static bool btClientPreemptionInitialized = false;
static std::atomic<bool> btClientPreemptionEnabled{false};
static std::atomic<bool> btClientTerminationRequested{false};

// Both loop() and the host monitor can request termination. Only one submits
// the GAP operation per connection state; no caller destroys client objects.
static bool requestBtClientTermination(void) {
  if (!btClient) return true;
  bool expected = false;
  if (!btClientTerminationRequested.compare_exchange_strong(expected, true)) return true;
  bool requested = true;
  if (btClient->getConnHandle() != BLE_HS_CONN_HANDLE_NONE)
    requested = btClient->disconnect();
  else if (btClientConnectionPending.load(std::memory_order_acquire))
    requested = btClient->cancelConnect();
  if (!requested) btClientTerminationRequested.store(false, std::memory_order_release);
  return requested;
}

static void drainBtClientPreemption(void) {
  if (!btClientPreemptionInitialized) return;
  ble_npl_callout_stop(&btClientPreemptionCallout);
#if !CONFIG_BT_LE_CONTROLLER_NPL_OS_PORTING_SUPPORT
  // The classic ESP32's NPL callout exposes its event. Stop its timer and
  // remove a previously queued expiry before teardown/cache reuse.
  ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &btClientPreemptionCallout.ev);
#endif
}

static void btClientPreemptionCheck(ble_npl_event *event) {
  (void)event;
  if (!btClientPreemptionEnabled.load(std::memory_order_acquire)) return;
  if (bleOperationCancelled()) {
    // Executed on the NimBLE host, so a loopTask blocked in synchronous GATT
    // is released by the normal GAP disconnect/ATT cancellation path.
    if (!requestBtClientTermination())
      Serial.println("BT: RF-priority host disconnect failed; retrying with RF off");
  }
  if (btClientPreemptionEnabled.load(std::memory_order_acquire) &&
      ble_npl_callout_reset(&btClientPreemptionCallout, ble_npl_time_ms_to_ticks32(20)) != BLE_NPL_OK) {
    btClientPreemptionEnabled.store(false, std::memory_order_release);
    Serial.println("BT: RF preemption monitor stopped; terminating connection safely");
    requestBtClientTermination();
  }
}

static void btClientTeardownBarrier(ble_npl_event *event) {
  (void)event;
  // This host event runs after GAP teardown; any queued monitor expiry can no
  // longer initiate a connection or touch a characteristic cache.
  drainBtClientPreemption();
  btClientQuiescentReady.store(true, std::memory_order_release);
}

static bool initializeBtClientBarrier(void) {
  if (!btClientBarrierEventInitialized) {
    ble_npl_event_init(&btClientTeardownBarrierEvent, btClientTeardownBarrier, nullptr);
    btClientBarrierEventInitialized = true;
  }
  if (!btClientPreemptionInitialized) {
    if (ble_npl_callout_init(&btClientPreemptionCallout, nimble_port_get_dflt_eventq(),
                            btClientPreemptionCheck, nullptr) != 0) {
      Serial.println("BT: RF preemption monitor allocation failed; connection disabled");
      return false;
    }
    btClientPreemptionInitialized = true;
  }
  return true;
}

// Call only while the host is running and after btClientQuiescent(). Events
// and timers must not retain the old host event queue across deinit/reinit.
static void releaseBtClientBarrier(void) {
  btClientPreemptionEnabled.store(false, std::memory_order_release);
  drainBtClientPreemption();
  if (btClientPreemptionInitialized) {
    ble_npl_callout_deinit(&btClientPreemptionCallout);
    btClientPreemptionInitialized = false;
    btClientPreemptionCallout = {};
  }
  if (btClientBarrierEventInitialized) {
    ble_npl_event_deinit(&btClientTeardownBarrierEvent);
    btClientBarrierEventInitialized = false;
    btClientTeardownBarrierEvent = {};
  }
}

static bool btClientQuiescent(void) {
  return !btClient ||
         (btClientQuiescentReady.load(std::memory_order_acquire) &&
          !btClientConnectionPending.load(std::memory_order_acquire) &&
          !btClientConnected.load(std::memory_order_acquire) &&
          btClient->getConnHandle() == BLE_HS_CONN_HANDLE_NONE);
}

class GShockClientCallbacks : public NimBLEClientCallbacks {
public:
  void onConnect(NimBLEClient *client) override {
    (void)client;
    // A cancel-connect racing with successful establishment must be followed
    // by an actual disconnect; let the monitor/loop submit the new operation.
    btClientTerminationRequested.store(false, std::memory_order_release);
    btClientConnected.store(true, std::memory_order_release);
    btClientConnectionPending.store(false, std::memory_order_release);
    Serial.println("BT: Casio watch connected");
  }
  void onConnectFail(NimBLEClient *client, int reason) override {
    (void)client;
    Serial.printf("BT: Casio connection failed (reason=%d)\n", reason);
    publishDisconnected();
  }
  void onDisconnect(NimBLEClient *client, int reason) override {
    (void)client;
    Serial.printf("BT: Casio watch disconnected (reason=%d)\n", reason);
    publishDisconnected();
  }
  void onMTUChange(NimBLEClient *client, uint16_t mtu) override {
    (void)client;
    Serial.printf("BT: negotiated ATT MTU=%u\n", mtu);
  }
private:
  static void publishDisconnected(void) {
    // Never delete the client, its services or characteristics in a callback.
    btClientPreemptionEnabled.store(false, std::memory_order_release);
    if (btClientPreemptionInitialized) ble_npl_callout_stop(&btClientPreemptionCallout);
    btClientConnected.store(false, std::memory_order_release);
    btClientConnectionPending.store(false, std::memory_order_release);
    btClientQuiescentReady.store(false, std::memory_order_release);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &btClientTeardownBarrierEvent);
  }
};
static GShockClientCallbacks gshockClientCallbacks;

static void gshockNotifyCallback(NimBLERemoteCharacteristic *characteristic,
                                 uint8_t *data, size_t length, bool isNotify) {
  (void)isNotify;
  // TIME also notifies on this watch. Only SP_DATA belongs to this transfer;
  // never append an unrelated characteristic's notification to a reply.
  if (!characteristic || characteristic->getUUID() != NimBLEUUID(CASIO_SP_DATA_CHAR) ||
      !data || !length) return;
  portENTER_CRITICAL(&btResponseMux);
  // First packet has the response header; continuation packets need not.
  // Buffer bounds are strict: never pass silently truncated watch data back.
  if (btResponseActive && (btResponseLength != 0 || data[0] == btExpectedHeader)) {
    if (length > sizeof(btResponseBuffer) - btResponseLength) {
      btResponseOverflow = true;
      ++btResponseErrors;
      btResponseActive = false;
    } else {
      memcpy(btResponseBuffer + btResponseLength, data, length);
      btResponseLength += length;
      btLastFragmentMillis = millis();
      ++btNotifications;
    }
  }
  portEXIT_CRITICAL(&btResponseMux);
}

static void clearBtDiscovery(void) {
  portENTER_CRITICAL(&btDiscoveryMux);
  ++btDiscoveryGeneration;
  btDiscoveryReady = false;
  btDiscoveredName[0] = '\0';
  btDiscoveredAddress[0] = '\0';
  btDiscoveredAddressType = 0xFF;
  portEXIT_CRITICAL(&btDiscoveryMux);
}

static bool consumeBtDiscovery(String &name, String &address, uint8_t &addressType) {
  char nameCopy[sizeof(btDiscoveredName)];
  char addressCopy[sizeof(btDiscoveredAddress)];
  bool ready;
  portENTER_CRITICAL(&btDiscoveryMux);
  ready = btDiscoveryReady;
  if (ready) {
    memcpy(nameCopy, btDiscoveredName, sizeof(nameCopy));
    memcpy(addressCopy, btDiscoveredAddress, sizeof(addressCopy));
    addressType = btDiscoveredAddressType;
    btDiscoveryReady = false;
  }
  portEXIT_CRITICAL(&btDiscoveryMux);
  if (!ready) return false;
  nameCopy[sizeof(nameCopy)-1] = '\0';
  addressCopy[sizeof(addressCopy)-1] = '\0';
  name = nameCopy;
  address = addressCopy;
  return address.length() == 17;
}

static void cancelBtResponse(void) {
  portENTER_CRITICAL(&btResponseMux);
  btResponseActive = false;
  portEXIT_CRITICAL(&btResponseMux);
}

static int btDeferredSlot = -1;
static unsigned long btShutdownRetryMillis = 0;
static bool btInitFailed = false;

// In NimBLE 2.5.1, init() may return false after enabling the controller but
// before starting the host. deinit() is then a no-op. Clean up that exact
// partial-init case using Espressif's controller API, preserving all checks.
static bool stopPartiallyInitializedBluetooth(void) {
  if (NimBLEDevice::isInitialized()) return false;
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      esp_bt_controller_disable() != ESP_OK) return false;
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      esp_bt_controller_deinit() != ESP_OK) return false;
  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
}

bool radioScheduleActiveNow(void) {
  if (full_time_tx || radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned()) return true;
  time_t epoch = time(nullptr); struct tm local; localtime_r(&epoch, &local);
  int minute = local.tm_hour * 60 + local.tm_min;
  for (int i=0;i<schedule_count;++i)
    if (minute >= schedules[i].start_min && minute < schedules[i].end_min) return true;
  return false;
}

bool bluetoothActivityPresent(void) { return radioBleArbiter.bleOwned(); }

void initBluetoothSync(void) {
  if (btBleInitialized || (btShutdownRetryMillis && (long)(millis()-btShutdownRetryMillis)<0) ||
      !radioBleArbiter.acquireBle()) return;
  if (!NimBLEDevice::init("RadioClock G-Shock Server")) {
    // An unsuccessful init may still leave the controller enabled. Keep the
    // ownership gate closed unless the physical controller is verified off.
    btInitFailed = true;
    bool off = stopPartiallyInitializedBluetooth();
    radioBleArbiter.finishBleShutdown(off);
    btLastSyncStatus = "Bluetooth initialization failed";
    btRadioSuspendRequested = !off;
    btShutdownRetryMillis = millis() + 1000;
    Serial.println("ERROR: BLE initialization failed; RF requires confirmed controller shutdown");
    return;
  }
  btInitFailed = false;
  btShutdownRetryMillis = 0;
  btBleInitialized = true;
  btScan = NimBLEDevice::getScan();
  static GShockScanCallbacks scanCallbacks;
  btScan->setScanCallbacks(&scanCallbacks, false);
  btScan->setActiveScan(true);
  btScan->setInterval(80);
  btScan->setWindow(60);
  btScan->setMaxResults(0);
  Serial.println("BT: NimBLE Casio time-only service ready");
}

void shutdownBluetoothForRadio(void) {
  if (!radioBleArbiter.bleOwned()) return;
  if (btShutdownRetryMillis && (long)(millis()-btShutdownRetryMillis) < 0) return;
  // Preserve explicit requests across RF preemption. Automatic windows retain
  // their slot, and Always Wait is restored from its persistent setting.
  if (btPairModeActive) btPairRequested = true;
  else if (btSyncActiveSlot >= 0) btDeferredSlot = btSyncActiveSlot;
  else if (!btPersistentWaitActive && btWindowActive) btManualSyncRequested = true;
  stopBluetoothWindow();
  cancelBtResponse();
  disconnectGShock();
  if (!btClientQuiescent()) {
    btLastSyncStatus = "RF blocked: Bluetooth disconnect teardown did not finish";
    setBluetoothPhase("Bluetooth shutdown failed; RF off");
    Serial.println("ERROR: BLE disconnect teardown incomplete; LF carrier remains OFF");
    btShutdownRetryMillis = millis() + 1000;
    return;
  }
  // false retains the scan/client allocations, including callback storage.
  // No GATT operation is active here, and the host teardown barrier completed.
  releaseBtClientBarrier();
  bool stopped = !NimBLEDevice::isInitialized() ?
                 stopPartiallyInitializedBluetooth() : NimBLEDevice::deinit(false);
  bool off = stopped && !NimBLEDevice::isInitialized() &&
             esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
  btBleInitialized = NimBLEDevice::isInitialized();
  if (!off) {
    btLastSyncStatus = "RF blocked: BLE controller shutdown failed";
    setBluetoothPhase("Bluetooth shutdown failed; RF off");
    Serial.println("ERROR: BLE controller is not confirmed OFF; LF carrier remains OFF");
    btShutdownRetryMillis = millis() + 1000;
    return;
  }
  btShutdownRetryMillis = 0;
  btInitFailed = false;
  btSuspendedByRadio = true;
  btRadioSuspendRequested = false;
  radioBleArbiter.finishBleShutdown(true);
  setBluetoothPhase("Paused for radio transmission");
  Serial.println("BT: host/controller OFF; LF radio may start");
  radioRequestRefresh();
}


void resetBluetoothDayIfNeeded(void) {
  time_t epoch;
  time(&epoch);
  struct tm day;
  localtime_r(&epoch, &day);
  btSyncDayComplete = false; // Informational: any watch profile delivered today.
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i)
    if (btProfileDoneToday(i)) btSyncDayComplete = true;
}

static int btMinutesOfDay(const struct tm &t) {
  return t.tm_hour * 60 + t.tm_min;
}

bool bluetoothTimeSlotConflicts(int minuteOfDay) {
  if (full_time_tx) return true;
  for (int m = -BT_PREPARE_MINUTES; m <= BT_WINDOW_AFTER_MINUTES; ++m) {
    int test = minuteOfDay + m;
    while (test < 0) test += 1440;
    while (test >= 1440) test -= 1440;
    for (int i = 0; i < schedule_count; ++i) {
      if (schedules[i].end_min > schedules[i].start_min &&
          test >= schedules[i].start_min &&
          test < schedules[i].end_min) return true;
    }
  }
  return false;
}

void startBluetoothWindow(int slot, bool persistent) {
  if (!btBleInitialized || bleOperationCancelled() || btBleBusy || btWindowActive) return;
  if (slot >= 0) {
    btPairModeActive = false;
    if (slot >= BT_SYNC_SLOT_COUNT || !btSyncEnabled[slot] ||
        btProfileDoneToday(btSyncProfile[slot]) || bluetoothTimeSlotConflicts(btSyncTimes[slot])) return;
    time_t epoch=time(nullptr); struct tm local; localtime_r(&epoch,&local);
    btSlotAttemptYear[slot]=local.tm_year+1900;
    btSlotAttemptYday[slot]=local.tm_yday;
    btSyncActiveSlot = slot;
    btActiveProtocol = btSyncProtocol[slot];
    btActiveProfile = btSyncProfile[slot];
  } else {
    btSyncActiveSlot = -1;
    btActiveProtocol = btManualProtocol;
    btActiveProfile = btManualProfile;
  }
  // Only explicit pairing may accept an unbound/replacement watch, in every
  // window type (manual, automatic and Always Wait).
  if (!btPairModeActive && (btProfileAddress[btActiveProfile].length() != 17 ||
      btProfileProtocol[btActiveProfile] != btActiveProtocol)) {
    btLastSyncStatus = "Pair Watch first for the selected profile and protocol";
    setBluetoothPhase("No watch paired");
    btSyncActiveSlot = -1;
    return;
  }
  btPersistentWaitActive = persistent;
  btLastWatchAddress = "";
  btLastWatchAddressType = 0xFF;
  clearBtDiscovery();
  portENTER_CRITICAL(&btDiscoveryMux);
  btDiscoveryProtocol = btActiveProtocol;
  if (!btPairModeActive)
    strlcpy(btExpectedProfileAddress, btProfileAddress[btActiveProfile].c_str(), sizeof(btExpectedProfileAddress));
  else btExpectedProfileAddress[0] = '\0';
  btWindowActive = true;
  portEXIT_CRITICAL(&btDiscoveryMux);
  int remainingMinutes = BT_PREPARE_MINUTES+BT_WINDOW_AFTER_MINUTES+2;
  if (slot>=0) {
    time_t epoch=time(nullptr); struct tm local; localtime_r(&epoch,&local);
    int start=(btSyncTimes[slot]-BT_PREPARE_MINUTES+1440)%1440;
    int elapsed=(btMinutesOfDay(local)-start+1440)%1440;
    if (elapsed >= remainingMinutes) { stopBluetoothWindow(); return; }
    remainingMinutes-=elapsed;
  }
  btWindowEndMillis = millis()+(unsigned long)remainingMinutes*60000UL;
  if (!persistent && WiFi.status() == WL_CONNECTED) sntp_restart();
  setBluetoothPhase(btPairModeActive ? "Pairing" :
                    slot >= 0 ? "Automatic sync window active" : "Waiting for watch");
  Serial.printf("BT: %s for Watch %u using %s protocol\n",
                persistent ? "Always Wait listening" : btPhase,
                btActiveProfile+1, btProtocolName(btActiveProtocol));
}

void stopBluetoothWindow(void) {
  btWindowActive = false;
  if (btScan && btBleInitialized && btScan->isScanning()) btScan->stop();
  clearBtDiscovery();
  btSyncActiveSlot = -1;
  btPairModeActive = false;
  btPersistentWaitActive = false;
  setBluetoothPhase("Idle");
}

// Arm notification reception BEFORE sending the BLE request; otherwise a fast
// response can arrive while writeValue() is still returning and be discarded.
static void prepareBtResponse(uint8_t header) {
  portENTER_CRITICAL(&btResponseMux);
  btResponseActive = false;
  btExpectedHeader = header;
  btResponseLength = 0;
  btResponseOverflow = false;
  btLastFragmentMillis = millis();
  btResponseActive = true;
  portEXIT_CRITICAL(&btResponseMux);
}

// Dynamic response assembly: first fragment identifies the request; collect
// continuations until an idle gap, with a minimum size and hard overflow guard.
static bool waitBt(size_t minimum) {
  uint32_t started = millis();
  while (millis() - started < BT_REQUEST_TIMEOUT_MS) {
    if (bleOperationCancelled() || !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
        !btClientConnected.load(std::memory_order_acquire)) {
      cancelBtResponse();
      Serial.println("BT: response wait cancelled (RF priority or watch disconnected)");
      return false;
    }
    portENTER_CRITICAL(&btResponseMux);
    size_t n = btResponseLength;
    unsigned long last = btLastFragmentMillis;
    bool overflow = btResponseOverflow;
    portEXIT_CRITICAL(&btResponseMux);
    if (overflow) { cancelBtResponse(); return false; }
    if (n >= minimum && millis() - last >= 300UL) {
      cancelBtResponse();
      return true;
    }
    delay(5);
  }
  cancelBtResponse();
  ++btResponseErrors;
  Serial.printf("BT: response timeout (expected header=0x%02x, minimum=%u bytes)\n",
                btExpectedHeader, (unsigned)minimum);
  return false;
}

static size_t copyBt(uint8_t *dst, size_t maxLen) {
  portENTER_CRITICAL(&btResponseMux);
  size_t n = btResponseLength < maxLen ? btResponseLength : maxLen;
  memcpy(dst, btResponseBuffer, n);
  portEXIT_CRITICAL(&btResponseMux);
  return n;
}

// Write mode is a protocol requirement, not a capability fallback: SP_REQUEST
// is a command without an ATT response; SP_DATA and TIME require ATT responses.
static const char *btGattStage = "idle";

static int btWriteComplete(uint16_t connection, const ble_gatt_error *error,
                           ble_gatt_attr *attribute, void *arg) {
  (void)connection;
  (void)attribute;
  auto *task = static_cast<NimBLETaskData *>(arg);
  *static_cast<uint16_t *>(task->m_pBuf) = error->att_handle;
  NimBLEUtils::taskRelease(*task, error->status);
  return 0;
}

static bool writeBt(NimBLERemoteCharacteristic *c, const uint8_t *d, size_t n,
                    bool withResponse) {
  if (!c || !d || !n || bleOperationCancelled() ||
      !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
      !btClientConnected.load(std::memory_order_acquire)) return false;
  if (withResponse ? !c->canWrite() : !c->canWriteNoResponse()) return false;
  const uint16_t mtu = btClient->getMTU();
  Serial.printf("BT: %s %s header=0x%02x bytes=%u MTU=%u\n", btGattStage,
                withResponse ? "write-response" : "write-no-response", d[0], (unsigned)n, mtu);
  // These protocol packets are single ATT writes. NimBLE's writeValue() can
  // fall back to a truncated prefix when a peer rejects Prepare/Execute Write.
  // Never report delivery of a partial packet or silently change its write mode.
  if (mtu < 3 || n > (size_t)(mtu - 3)) {
    Serial.printf("BT: %s rejected: %u-byte packet requires MTU >= %u\n",
                  btGattStage, (unsigned)n, (unsigned)(n + 3));
    return false;
  }
  int status = BLE_HS_ENOTCONN;
  uint16_t errorHandle = 0;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (bleOperationCancelled() || !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
        !btClientConnected.load(std::memory_order_acquire)) return false;
    if (!withResponse) {
      status = ble_gattc_write_no_rsp_flat(btClient->getConnHandle(), c->getHandle(), d, n);
    } else {
      NimBLETaskData task(nullptr, 0, &errorHandle);
      status = ble_gattc_write_flat(btClient->getConnHandle(), c->getHandle(), d, n,
                                   btWriteComplete, &task);
      if (status == 0) {
        // Keep callback storage alive until ATT completes or GAP cancels it.
        // The independent host monitor still preempts this wait for RF demand.
        NimBLEUtils::taskWait(task, BLE_NPL_TIME_FOREVER);
        status = task.m_flags;
      }
      if (status == BLE_HS_EDONE) status = 0;
    }
    if (status == 0) {
      // WNR success confirms local submission only; WR success is an ATT ack.
      if (withResponse) ++btWriteAcknowledgements;
      return true;
    }
    const bool needsSecurity = status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
                               status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHOR) ||
                               status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC);
    if (attempt == 0 && withResponse && needsSecurity && !bleOperationCancelled() &&
        btClientPreemptionEnabled.load(std::memory_order_acquire) &&
        btClientConnected.load(std::memory_order_acquire) && btClient->secureConnection()) {
      Serial.printf("BT: %s retrying full packet after securing connection\n", btGattStage);
      continue;
    }
    break;
  }
  Serial.printf("BT: %s GATT %s failed (UUID=%s status=%d/0x%04x error-handle=0x%04x)\n",
                btGattStage, withResponse ? "write-response" : "write-no-response",
                c->getUUID().toString().c_str(), status, (unsigned)status, errorHandle);
  if (status >= 0x100 && status < 0x200)
    Serial.printf("BT: ATT error=0x%02x\n", status - 0x100);
  return false;
}

static void disconnectGShock(void) {
  cancelBtResponse();
  btClientPreemptionEnabled.store(false, std::memory_order_release);
  if (btClient && !btClientQuiescent()) {
    if (!requestBtClientTermination())
      Serial.println("BT: client termination request failed; retaining client");
    const uint32_t started = millis();
    while (!btClientQuiescent() && millis() - started < 2000UL) {
      // A late onConnect after cancelConnect resets the request guard, allowing
      // this wait to terminate the established connection safely as well.
      requestBtClientTermination();
      delay(5);
    }
    if (!btClientQuiescent())
      Serial.println("BT: teardown barrier timeout; reconnect and RF remain blocked");
  }
  // These aliases are owned by loop(); clearing them never destroys NimBLE's
  // retained service objects. Cache destruction happens only on a later safe
  // reconnect, after the host barrier and all synchronous GATT calls returned.
  btService = nullptr;
  btSpRequest = nullptr;
  btSpData = nullptr;
  btSetChar = nullptr;
  btBleBusy = !btClientQuiescent();
}

static bool validateBtCharacteristic(const char *label, const char *uuid,
                                     NimBLERemoteCharacteristic *c, bool withResponse) {
  if (!c) {
    Serial.printf("BT: %s missing (UUID=%s)\n", label, uuid);
    return false;
  }
  String properties;
  if (c->canWriteNoResponse()) properties += "write-no-response ";
  if (c->canWrite()) properties += "write-response ";
  if (c->canNotify()) properties += "notify ";
  if (c->canIndicate()) properties += "indicate ";
  properties.trim();
  if (!properties.length()) properties = "no supported properties";
  Serial.printf("BT: %s found [%s]\n", label, properties.c_str());
  if (withResponse ? !c->canWrite() : !c->canWriteNoResponse()) {
    Serial.printf("BT: %s wrong properties (requires %s; UUID=%s)\n", label,
                  withResponse ? "write-response" : "write-no-response", uuid);
    return false;
  }
  return true;
}

static bool connectGShock(int protocol) {
  if (btLastWatchAddress.length() != 17 || !btBleInitialized || bleOperationCancelled()) return false;
  if (btClient && !btClientQuiescent()) {
    disconnectGShock();
    if (!btClientQuiescent()) return false;
  }
  btBleBusy = true;
  setBluetoothPhase("Connecting");
  if (!btClient) {
    btClient = NimBLEDevice::createClient();
    if (!btClient) {
      btBleBusy = false;
      Serial.println("BT: NimBLE client allocation failed");
      return false;
    }
    btClient->setClientCallbacks(&gshockClientCallbacks, false);
    btClient->setSelfDelete(false, false);
    btClient->setConnectTimeout(5000);
    btClient->setConnectRetries(0); // RF cancellation and each attempt stay bounded.
  }
  if (!initializeBtClientBarrier()) {
    btBleBusy = false;
    return false;
  }
  if (protocol == BT_PROTOCOL_BX5600_MIP && !NimBLEDevice::setMTU(517)) {
    Serial.println("BT: preferred ATT MTU setup failed");
    disconnectGShock();
    return false;
  }
  btClientTerminationRequested.store(false, std::memory_order_release);
  ++btConnectionAttempts;
  btClientQuiescentReady.store(false, std::memory_order_release);
  btClientConnectionPending.store(true, std::memory_order_release);
  NimBLEAddress peer(std::string(btLastWatchAddress.c_str()), btLastWatchAddressType);
  // Start asynchronously so RF demand can cancel the connection promptly.
  // deleteAttributes=true refreshes the cache only after the previous barrier.
  if (!btClient->connect(peer, true, true)) {
    btClientConnectionPending.store(false, std::memory_order_release);
    btClientQuiescentReady.store(true, std::memory_order_release);
    Serial.println("BT: connection could not be started");
    disconnectGShock();
    return false;
  }
  btClientPreemptionEnabled.store(true, std::memory_order_release);
  if (ble_npl_callout_reset(&btClientPreemptionCallout, ble_npl_time_ms_to_ticks32(20)) != BLE_NPL_OK) {
    Serial.println("BT: RF preemption monitor could not start; connection cancelled");
    disconnectGShock();
    return false;
  }
  const uint32_t started = millis();
  while (btClientConnectionPending.load(std::memory_order_acquire) &&
         !bleOperationCancelled() && millis() - started < 5500UL) delay(5);
  if (bleOperationCancelled() || !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
      !btClientConnected.load(std::memory_order_acquire)) {
    Serial.println("BT: connection cancelled, timed out or failed");
    disconnectGShock();
    return false;
  }
  if (protocol == BT_PROTOCOL_BX5600_MIP) {
    // onConnect may precede asynchronous MTU completion. Largest SP packet is
    // 133 bytes and must fit in a single Write Request (MTU >= 136).
    const uint32_t mtuStarted = millis();
    while (btClient->getMTU() < CasioBxProtocol::kMinimumMtu &&
           !bleOperationCancelled() && btClientConnected.load(std::memory_order_acquire) &&
           btClientPreemptionEnabled.load(std::memory_order_acquire) &&
           millis() - mtuStarted < 2000UL) delay(5);
    Serial.printf("BT: GW-BX5600 ATT MTU=%u (required >= %u)\n",
                  btClient->getMTU(), (unsigned)CasioBxProtocol::kMinimumMtu);
    if (btClient->getMTU() < CasioBxProtocol::kMinimumMtu) {
      Serial.println("BT: MTU exchange insufficient; no SP handshake sent");
      disconnectGShock();
      return false;
    }
  }
  btService = btClient->getService(NimBLEUUID(CASIO_WATCH_FEATURES_SERVICE));
  if (bleOperationCancelled() || !btService) {
    Serial.println("BT: Casio feature service missing or discovery cancelled");
    disconnectGShock();
    return false;
  }
  btSetChar = btService->getCharacteristic(NimBLEUUID(CASIO_SET_CHAR));
  bool valid = validateBtCharacteristic("TIME", CASIO_SET_CHAR, btSetChar, true);
  // Only the BX5600 MIP path needs SP request/response characteristics.
  if (protocol == BT_PROTOCOL_BX5600_MIP && !bleOperationCancelled()) {
    btSpRequest = btService->getCharacteristic(NimBLEUUID(CASIO_SP_REQUEST_CHAR));
    if (!bleOperationCancelled())
      btSpData = btService->getCharacteristic(NimBLEUUID(CASIO_SP_DATA_CHAR));
    const bool requestValid = validateBtCharacteristic("SP_REQUEST", CASIO_SP_REQUEST_CHAR, btSpRequest, false);
    const bool dataValid = validateBtCharacteristic("SP_DATA", CASIO_SP_DATA_CHAR, btSpData, true);
    valid = valid && requestValid && dataValid;
    // Subscribe before sending any request so a fast response cannot be lost.
    bool subscribed = false;
    if (valid && !bleOperationCancelled()) {
      if (btSpData->canNotify()) subscribed = btSpData->subscribe(true, gshockNotifyCallback, true);
      else if (btSpData->canIndicate()) subscribed = btSpData->subscribe(false, gshockNotifyCallback, true);
      if (!subscribed) Serial.println("BT: SP_DATA notification/indication subscription failed");
    }
    valid = valid && subscribed;
  }
  if (!valid || bleOperationCancelled() || !btClientPreemptionEnabled.load(std::memory_order_acquire) ||
      !btClientConnected.load(std::memory_order_acquire)) {
    disconnectGShock();
    return false;
  }
  Serial.println("BT: protocol characteristics OK");
  setBluetoothPhase("Syncing");
  return true;
}

static size_t bxRequest(const uint8_t *req, size_t reqLen, uint8_t header,
                        size_t minimum, uint8_t *out, size_t maxOut) {
  if (!out || maxOut < minimum || bleOperationCancelled()) return 0;
  prepareBtResponse(header);
  if (!writeBt(btSpRequest, req, reqLen, false)) {
    cancelBtResponse();
    ++btResponseErrors;
    return 0;
  }
  if (!waitBt(minimum)) return 0;
  // Do not accept a valid-looking prefix of a response that would be truncated.
  portENTER_CRITICAL(&btResponseMux);
  const size_t received = btResponseLength;
  portEXIT_CRITICAL(&btResponseMux);
  if (received > maxOut || bleOperationCancelled()) {
    ++btResponseErrors;
    return 0;
  }
  size_t len = copyBt(out, maxOut);
  Serial.printf("BT: %s response header=0x%02x bytes=%u\n", btGattStage,
                len ? out[0] : 0, (unsigned)len);
  if (len < minimum || out[0] != header) {
    ++btResponseErrors;
    return 0;
  }
  return len;
}

bool performGShockBX5600Sync(void) {
  if (!connectGShock(BT_PROTOCOL_BX5600_MIP)) return false;
  uint8_t r[BT_RESPONSE_CAPACITY];
  uint8_t cities[CasioBxProtocol::kCitiesSize];
  uint8_t settings[CasioBxProtocol::kSettingsWriteSize];
  uint8_t dst[CasioBxProtocol::kDstWriteSize];
  bool ok = false;

  do {
    btGattStage = "BX step 1 settings";
    const uint8_t req1[] = {0x05,0x1D,0x00,0x1D,0x00,0x24,0x00,0x24,0x01,0x24,0x02};
    size_t n1 = bxRequest(req1, sizeof(req1), 0x05, 101, r, sizeof(r));
    if (!n1) break;
    if (!CasioBxProtocol::splitSettings(r, n1, settings, sizeof(settings), cities, sizeof(cities))) {
      Serial.println("BT: BX step 1 malformed settings/city records");
      ++btResponseErrors;
      break;
    }
    // Send only the two 0x1D records; save the three 0x24 city records for step 2.
    if (!writeBt(btSpData, settings, sizeof(settings), true)) break;

    btGattStage = "BX step 2 DST/cities";
    const uint8_t req2[] = {0x03,0x1E,0x00,0x1E,0x00,0x1E,0x00};
    size_t n2 = bxRequest(req2, sizeof(req2), 0x03, 28, r, sizeof(r));
    if (!n2) break;
    if (!CasioBxProtocol::buildDst(r, n2, cities, sizeof(cities), dst, sizeof(dst))) {
      Serial.println("BT: BX step 2 malformed DST/city records");
      ++btResponseErrors;
      break;
    }
    if (!writeBt(btSpData, dst, sizeof(dst), true)) break;

    btGattStage = "BX step 3 alarms";
    uint8_t req3[13] = {0x06};
    for (int i = 0; i < 6; ++i) {
      int idx = (i / 2) + ((i % 2) ? 6 : 0);
      req3[1 + i * 2] = 0x1F;
      req3[2 + i * 2] = (uint8_t)idx;
    }
    size_t n3 = bxRequest(req3, sizeof(req3), 0x06, 133, r, sizeof(r));
    if (!n3) break;
    if (!CasioBxProtocol::validAlarms(r, n3)) {
      Serial.println("BT: BX step 3 malformed alarm records");
      ++btResponseErrors;
      break;
    }
    if (!writeBt(btSpData, r, n3, true)) break;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t epoch = tv.tv_sec;
    struct tm local;
    localtime_r(&epoch, &local);
    int year = local.tm_year + 1900;
    uint8_t tc[11] = {
      0x09,(uint8_t)(year&255),(uint8_t)((year>>8)&255),
      (uint8_t)(local.tm_mon+1),(uint8_t)local.tm_mday,
      (uint8_t)local.tm_hour,(uint8_t)local.tm_min,(uint8_t)local.tm_sec,
      (uint8_t)(local.tm_wday==0?7:local.tm_wday),
      (uint8_t)((tv.tv_usec/1000UL*256UL)/1000UL),0x01
    };
    btGattStage = "BX step 4 TIME";
    if (!writeBt(btSetChar, tc, sizeof(tc), true)) break;

    ok = true;
    btDeliveryEvidence = "ATT write acknowledged; watch display unverified";
    Serial.printf("BT: GW-BX5600 time command delivered %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, local.tm_mon+1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
  } while (false);

  disconnectGShock();
  btGattStage = "idle";
  return ok;
}

bool performCasioStandardTimeSync(int protocol) {
  if (protocol != BT_PROTOCOL_STANDARD && protocol != BT_PROTOCOL_ANALOGUE) return false;
  if (!connectGShock(protocol)) return false;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t epoch = tv.tv_sec;
  struct tm local;
  localtime_r(&epoch, &local);
  int year = local.tm_year + 1900;
  const uint8_t tc[10] = {
    0x09, (uint8_t)(year & 255), (uint8_t)((year >> 8) & 255),
    (uint8_t)(local.tm_mon + 1), (uint8_t)local.tm_mday,
    (uint8_t)local.tm_hour, (uint8_t)local.tm_min, (uint8_t)local.tm_sec,
    (uint8_t)local.tm_wday, 0x01
  };
  btGattStage = "Casio TIME";
  bool ok = writeBt(btSetChar, tc, sizeof(tc), true);
  if (ok) btDeliveryEvidence = "ATT write acknowledged; watch display unverified";
  if (ok) Serial.printf("BT: %s time write acknowledged, %04d-%02d-%02d %02d:%02d:%02d\n",
                        btProtocolName(protocol), year, local.tm_mon + 1, local.tm_mday,
                        local.tm_hour, local.tm_min, local.tm_sec);
  disconnectGShock();
  btGattStage = "idle";
  return ok;
}

bool attemptBluetoothSync(void) {
  if (full_time_tx) {
    btLastSyncStatus = "Blocked: full-time radio transmission";
    return false;
  }
  if (!clockTrusted()) {
    btLastSyncStatus = "Blocked: NTP clock not trusted; awaiting a confirmed SNTP update";
    return false;
  }
  if (bleOperationCancelled() || !radioBleArbiter.bleOwned()) {
    btLastSyncStatus = "Deferred: radio transmission has priority";
    return false;
  }
  const int protocol = btActiveProtocol;
  Serial.printf("BT: connecting with %s time-only protocol\n", btProtocolName(protocol));
  bool ok = (protocol == BT_PROTOCOL_BX5600_MIP)
              ? performGShockBX5600Sync() : performCasioStandardTimeSync(protocol);
  if (ok) {
    time_t syncEpoch=time(nullptr); struct tm syncLocal; localtime_r(&syncEpoch,&syncLocal);
    btSuccessYear[protocol] = syncLocal.tm_year + 1900;
    btSuccessYday[protocol] = syncLocal.tm_yday;
    btProfileSuccessYear[btActiveProfile] = syncLocal.tm_year + 1900;
    btProfileSuccessYday[btActiveProfile] = syncLocal.tm_yday;
    if (btPairModeActive) {
      btProfileAddress[btActiveProfile] = btLastWatchAddress;
      btProfileName[btActiveProfile] = btLastWatchName;
      btProfileProtocol[btActiveProfile] = btActiveProtocol;
    }
    btSyncDayComplete = true;
    if (protocol == BT_PROTOCOL_BX5600_MIP) {
      btSyncLastYear = btSuccessYear[protocol];
      btSyncLastYday = btSuccessYday[protocol];
    }
    char d[16]; strftime(d,sizeof(d),"%Y-%m-%d",&syncLocal);
    btLastSyncDate = d;
    btLastSyncStatus = String("Watch ") + (btActiveProfile+1) + ": " + btProtocolName(protocol) + " - time write delivered (watch unverified)";
    saveConfig();
  } else {
    btLastSyncStatus = String(btProtocolName(protocol)) + " - sync attempt failed";
  }
  return ok;
}

void serviceBluetoothSync(void) {
  // This check precedes EVERY initialization, scan, connect and window action.
  // radioTask keeps running during GATT, so a newly due RF schedule aborts BLE.
  if (bleOperationCancelled() || btRadioSuspendRequested ||
      (!btBleInitialized && radioBleArbiter.bleOwned())) {
    shutdownBluetoothForRadio();
    return;
  }
  if (btSuspendedByRadio) {
    btSuspendedByRadio = false;
    Serial.println("BT: LF radio finished; restoring Bluetooth workflow");
  }
  if (!btBleInitialized) initBluetoothSync();
  if (!btBleInitialized || bleOperationCancelled()) return;
  resetBluetoothDayIfNeeded();
  // A previously timed-out host teardown may finish later. Only loopTask can
  // clear the quarantine, after the queued GAP barrier completes.
  if (btBleBusy && btClientQuiescent()) disconnectGShock();

  if (btPairRequested && !btBleBusy) {
    stopBluetoothWindow();
    btPairRequested = false;
    btManualSyncRequested = false;
    btPairModeActive = true;
    btLastSyncStatus = String("Pairing Watch ") + (btManualProfile + 1);
    startBluetoothWindow(-1);
  } else if (btManualSyncRequested && !btBleBusy) {
    // Serialized on loopTask: stop callbacks, discard stale discovery, and
    // reopen the window. Repeated HTTP requests never delete a client.
    stopBluetoothWindow();
    btManualSyncRequested = false;
    btLastSyncStatus = "Waiting for watch";
    startBluetoothWindow(-1);
  }

  // Scheduled windows can preempt passive Always Wait, preserving the selected
  // manual profile. They cannot preempt an explicit manual/pairing window.
  if (!btBleBusy && (!btWindowActive || btPersistentWaitActive)) {
    time_t epoch=time(nullptr); struct tm local; localtime_r(&epoch,&local);
    int nm = btMinutesOfDay(local);
    for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) {
      int prep=(btSyncTimes[i]-BT_PREPARE_MINUTES+1440)%1440;
      bool deferred = btDeferredSlot == i;
      if (!deferred && nm != prep && nm != (prep+1)%1440) continue;
      if (!btSyncEnabled[i] || btProfileDoneToday(btSyncProfile[i])) continue;
      if (!deferred && btSlotAttemptYear[i]==local.tm_year+1900 && btSlotAttemptYday[i]==local.tm_yday) continue;
      stopBluetoothWindow();
      if (deferred) btDeferredSlot = -1;
      startBluetoothWindow(i);
      if (btWindowActive) break;
    }
  }

  if (btPersistentWaitActive && !btAlwaysWaitEnabled) stopBluetoothWindow();
  if (!btWindowActive && !btBleBusy && btAlwaysWaitEnabled &&
      btProfileAddress[btManualProfile].length() == 17 &&
      btProfileProtocol[btManualProfile] == btManualProtocol) startBluetoothWindow(-1, true);

  if (btWindowActive && !btBleBusy) {
    if (!btPersistentWaitActive && (long)(millis()-btWindowEndMillis) >= 0) {
      stopBluetoothWindow();
      btLastSyncStatus = String(btProtocolName(btActiveProtocol)) + " - no watch connection";
      setBluetoothPhase("Idle");
      return;
    }
    String discoveredName, discoveredAddress; uint8_t discoveredType = 0xFF;
    if (consumeBtDiscovery(discoveredName, discoveredAddress, discoveredType)) {
      if (btScan && btScan->isScanning()) btScan->stop();
      btLastWatchName = discoveredName;
      btLastWatchAddress = discoveredAddress;
      btLastWatchAddressType = discoveredType;
      btWindowActive = false;
      bool ok = attemptBluetoothSync();
      if (bleOperationCancelled()) {
        // Keep the kind of interrupted window available to shutdown capture.
        btWindowActive = true;
        shutdownBluetoothForRadio();
      } else if (!ok) {
        btLastWatchAddress = "";
        clearBtDiscovery();
        btWindowActive = true;
        setBluetoothPhase(btPairModeActive ? "Pairing" : "Waiting for watch");
      } else {
        stopBluetoothWindow();
        setBluetoothPhase("Idle");
      }
      return;
    }
    if (btScan && !bleOperationCancelled() && !btScan->isScanning()) {
      if (!btScan->start(BT_SCAN_SLICE_MS, false, true)) {
        btLastSyncStatus = "Bluetooth scan start failed";
        setBluetoothPhase("Waiting for watch");
      } else if (!btPairModeActive && btSyncActiveSlot < 0) setBluetoothPhase("Scanning");
    }
  }
}


void initFilesystem(void)
{
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  Serial.println("LittleFS Mounted Successfully");
}

void loadConfig(void)
{
  File configFile = LittleFS.open(CONFIG_FILE, "r");
  if (!configFile) {
    strcpy(ssid, ""); strcpy(passwd, "");
    timezone_name = DEFAULT_TZ_NAME;
    full_time_tx = false;
    wifiPowerMode = WIFI_POWER_ALWAYS_ON;
    return;
  }
  configFile.readStringUntil('\n').toCharArray(ssid, sizeof(ssid));
  configFile.readStringUntil('\n').toCharArray(passwd, sizeof(passwd));
  String tz = configFile.readStringUntil('\n'); tz.trim();

  // Accept old numeric configurations as well as the new IANA timezone names.
  if (tz == "32400") timezone_name = "Asia/Tokyo";
  else if (tz == "36000") timezone_name = "Australia/Brisbane";
  else if (tz == "39600") timezone_name = "Australia/Sydney";
  else if (tz == "28800") timezone_name = "Asia/Shanghai";
  else if (tz == "0") timezone_name = "Europe/London";
  else if (tz == "-18000") timezone_name = "America/New_York";
  else if (tz == "-28800") timezone_name = "America/Los_Angeles";
  else if (tz.length()) timezone_name = tz;
  else timezone_name = DEFAULT_TZ_NAME;
  // A hand-edited/corrupt legacy config must not leave the UI showing one
  // zone while posixTzFor() silently falls back to another.
  if (!validTimezoneName(timezone_name)) {
    Serial.println("Unsupported stored timezone; reverting to default");
    timezone_name = DEFAULT_TZ_NAME;
  }

  String tx = configFile.readStringUntil('\n'); tx.trim();
  full_time_tx = (tx == "1" || tx.equalsIgnoreCase("true"));

  String fs = configFile.readStringUntil('\n'); fs.trim();
  if (fs.length()) full_time_station = fs.toInt();
  if (full_time_station < SN_JJY_E || full_time_station > SN_BPC) full_time_station = SN_JJY_E;

  // Optional sixth configuration line. Zero preserves the official station time.
  String tso = configFile.readStringUntil('\n'); tso.trim();
  if (tso.length()) transmission_offset_minutes = tso.toInt();
  if (transmission_offset_minutes < -720 || transmission_offset_minutes > 840)
    transmission_offset_minutes = 0;

  // Optional seventh configuration line: WiFi power mode (0=Always On,
  // 1=Scheduled/power-save). Missing or unparsable defaults to Always On,
  // so existing configs without this line behave exactly as before.
  String wpm = configFile.readStringUntil('\n'); wpm.trim();
  wifiPowerMode = wpm.length() ? wpm.toInt() : WIFI_POWER_ALWAYS_ON;
  if (wifiPowerMode != WIFI_POWER_ALWAYS_ON && wifiPowerMode != WIFI_POWER_SCHEDULED)
    wifiPowerMode = WIFI_POWER_ALWAYS_ON;
  // Optional Bluetooth settings. Missing lines retain the Casio defaults.
  for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
    String line = configFile.readStringUntil('\n'); line.trim();
    if (line.length()) btSyncEnabled[i] = (line == "1" || line.equalsIgnoreCase("true"));
  }
  String bty = configFile.readStringUntil('\n'); bty.trim();
  if (bty.length()) btSyncLastYear = bty.toInt();
  String btd = configFile.readStringUntil('\n'); btd.trim();
  if (btd.length()) btSyncLastYday = btd.toInt();
  String bts = configFile.readStringUntil('\n'); bts.trim();
  if (bts.length()) btLastSyncDate = bts;

  // Older V2.9 multi-protocol fields, kept in their original position.
  // Existing installations default every slot and the manual button to BX5600.
  for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
    String p = configFile.readStringUntil('\n'); p.trim();
    if (p.length() && validBtProtocol(p.toInt())) btSyncProtocol[i] = (uint8_t)p.toInt();
  }
  String mp = configFile.readStringUntil('\n'); mp.trim();
  if (mp.length() && validBtProtocol(mp.toInt())) btManualProtocol = (uint8_t)mp.toInt();
  // Legacy files could have today's year/day persisted by a normal config
  // save even when no sync occurred. Trust the legacy completion fields only
  // if their calendar date agrees with the stored last-success date.
  int oldYear = 0, oldMonth = 0, oldDay = 0;
  if (sscanf(btLastSyncDate.c_str(), "%d-%d-%d", &oldYear, &oldMonth, &oldDay) == 3 &&
      oldYear >= 2020 && oldMonth >= 1 && oldMonth <= 12 && oldDay >= 1 && oldDay <= 31) {
    struct tm completed = {};
    completed.tm_year = oldYear - 1900;
    completed.tm_mon = oldMonth - 1;
    completed.tm_mday = oldDay;
    completed.tm_hour = 12;
    completed.tm_isdst = -1;
    mktime(&completed);
    if (oldYear == btSyncLastYear && completed.tm_yday == btSyncLastYday) {
      btSuccessYear[BT_PROTOCOL_BX5600_MIP] = btSyncLastYear;
      btSuccessYday[BT_PROTOCOL_BX5600_MIP] = btSyncLastYday;
    }
  }
  for (int p = BT_PROTOCOL_STANDARD; p < BT_PROTOCOL_COUNT; ++p) {
    String yearLine = configFile.readStringUntil('\n'); yearLine.trim();
    String dayLine = configFile.readStringUntil('\n'); dayLine.trim();
    if (yearLine.length()) btSuccessYear[p] = yearLine.toInt();
    if (dayLine.length()) btSuccessYday[p] = dayLine.toInt();
  }

  // Reliability-pass appended fields: slot times, watch-profile selection, independent
  // completion flags and learned addresses/names. Older files end here.
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) {
    String v=configFile.readStringUntil('\n'); v.trim();
    if (v.length()) { int m=v.toInt(); if (m>=0 && m<1440) btSyncTimes[i]=m; }
  }
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) {
    String v=configFile.readStringUntil('\n'); v.trim();
    if (v.length()) { int p=v.toInt(); if (p>=0 && p<BT_WATCH_PROFILE_COUNT) btSyncProfile[i]=p; }
  }
  String manual=configFile.readStringUntil('\n'); manual.trim();
  if (manual.length() && manual.toInt()>=0 && manual.toInt()<BT_WATCH_PROFILE_COUNT)
    btManualProfile=(uint8_t)manual.toInt();
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    String y=configFile.readStringUntil('\n'); y.trim();
    String d=configFile.readStringUntil('\n'); d.trim();
    if (y.length() && d.length()) { btProfileSuccessYear[i]=y.toInt(); btProfileSuccessYday[i]=d.toInt(); }
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    String a=configFile.readStringUntil('\n'); a.trim();
    if (a.length()==17) btProfileAddress[i]=a;
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    String n=configFile.readStringUntil('\n'); n.trim(); n.replace("\r","");
    if (n.length()<48) btProfileName[i]=n;
  }
  String alwaysWait = configFile.readStringUntil('\n'); alwaysWait.trim();
  btAlwaysWaitEnabled = (alwaysWait == "1" || alwaysWait.equalsIgnoreCase("true"));
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    // Older bindings inherit their existing manual/scheduled profile protocol.
    btProfileProtocol[i] = (i == btManualProfile) ? btManualProtocol : (uint8_t)BT_PROTOCOL_BX5600_MIP;
    for (int slot=0;slot<BT_SYNC_SLOT_COUNT;++slot)
      if (btSyncProfile[slot] == i) { btProfileProtocol[i] = btSyncProtocol[slot]; break; }
    String protocol = configFile.readStringUntil('\n'); protocol.trim();
    if (protocol.length() && validBtProtocol(protocol.toInt())) btProfileProtocol[i] = protocol.toInt();
  }
  // One-time V2.9 completion migration (existing four times all use Watch 1).
  if (btProfileSuccessYear[0]<0 && btSuccessYear[BT_PROTOCOL_BX5600_MIP]>=2020) {
    btProfileSuccessYear[0]=btSuccessYear[BT_PROTOCOL_BX5600_MIP];
    btProfileSuccessYday[0]=btSuccessYday[BT_PROTOCOL_BX5600_MIP];
  }
  for (int i=0; ssid[i]; i++) if (ssid[i]=='\r'||ssid[i]=='\n') ssid[i]='\0';
  for (int i=0; passwd[i]; i++) if (passwd[i]=='\r'||passwd[i]=='\n') passwd[i]='\0';
  configFile.close();
  Serial.printf("Loaded config: SSID=%s, TZ=%s, Full TX=%s, WiFi mode=%s\n",
                ssid, timezone_name.c_str(), full_time_tx ? "ON":"OFF",
                wifiPowerMode == WIFI_POWER_SCHEDULED ? "Scheduled" : "Always on");
}

// Flash-wear mitigation #3: debounced config persistence. Several settings
// (timezone, transmission offset, full-time toggle, WiFi power mode) apply
// immediately on change and each POST unconditionally calls saveConfig() at
// the end of the handler. A person clicking around a few times a session is
// negligible wear, but nothing previously stopped a scripted/automated
// integration from calling the API rapidly and hammering flash with a full
// config rewrite on every single change. saveConfig() now just marks the
// config dirty; the actual flash write is coalesced by writeConfigNow()
// (called from loop()) after CONFIG_SAVE_DEBOUNCE_MS of no further changes.
// In-memory settings still take effect the instant they're changed (nothing
// here delays behavior) - only the flash write itself is debounced, so the
// only real cost of this is that a change made within the debounce window
// of a power loss could need to be re-applied after reboot.
// (CONFIG_SAVE_DEBOUNCE_MS/configDirty/configDirtyBecause are declared up
// near the other globals, above loop(), since loop() reads them directly
// and - unlike functions - plain variables/#defines aren't auto-forward-
// declared by the Arduino build system.)

void writeConfigNow(void)
{
  String blob;
  blob.reserve(700);
  auto line = [&](const String &v) { blob += v; blob += '\n'; };
  line(ssid); line(passwd); line(timezone_name);
  line(full_time_tx ? "1":"0"); line(String(full_time_station));
  line(String(transmission_offset_minutes)); line(String(wifiPowerMode));
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) line(btSyncEnabled[i] ? "1":"0");
  line(String(btSyncLastYear)); line(String(btSyncLastYday)); line(btLastSyncDate);
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) line(String((int)btSyncProtocol[i]));
  line(String((int)btManualProtocol));
  for (int i=BT_PROTOCOL_STANDARD;i<BT_PROTOCOL_COUNT;++i) {
    line(String(btSuccessYear[i])); line(String(btSuccessYday[i]));
  }
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) line(String(btSyncTimes[i]));
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) line(String((int)btSyncProfile[i]));
  line(String((int)btManualProfile));
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    line(String(btProfileSuccessYear[i])); line(String(btProfileSuccessYday[i]));
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) line(btProfileAddress[i]);
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    String n=btProfileName[i]; n.replace("\r", ""); n.replace("\n", ""); line(n);
  }
  line(btAlwaysWaitEnabled ? "1" : "0");
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) line(String(btProfileProtocol[i]));
  File f=LittleFS.open(CONFIG_TEMP_FILE,"w");
  if (!f) { Serial.println("Config save failed: temp open"); configDirty=true; return; }
  size_t written=f.print(blob); f.flush(); f.close();
  if (written!=blob.length() || !LittleFS.rename(CONFIG_TEMP_FILE,CONFIG_FILE)) {
    Serial.println("Config save failed: old file retained"); configDirty=true;
    return;
  }
  Serial.println("Config saved atomically");
}

void saveConfig(void)
{
  configDirty = true;
  configDirtyBecause = millis();
}

static bool validScheduleJson(JsonVariantConst v, TimeSchedule &out) {
  if (!v.is<JsonObjectConst>()) return false;
  JsonObjectConst o=v.as<JsonObjectConst>();
  if (!o["station"].is<int>() || !o["start"].is<int>() || !o["end"].is<int>()) return false;
  int st=o["station"].as<int>(), a=o["start"].as<int>(), b=o["end"].as<int>();
  if (st<0 || st>=NUM_STATIONS || a<0 || a>=1440 || b<=a || b>1440) return false;
  out.station=st; out.start_min=a; out.end_min=b; return true;
}
void normalizeBluetoothSlots() {
  bool changed=false;
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) {
    bool earlierOverlap=false;
    for (int j=0;j<i;++j) if (btSyncEnabled[j] &&
        btCircularDistance(btSyncTimes[i],btSyncTimes[j]) <= BT_PREPARE_MINUTES+BT_WINDOW_AFTER_MINUTES+1)
      earlierOverlap=true;
    if (btSyncEnabled[i] && (bluetoothTimeSlotConflicts(btSyncTimes[i]) || earlierOverlap)) {
      btSyncEnabled[i]=false; changed=true;
    }
  }
  if (changed) { Serial.println("BT: conflicting automatic slots disabled"); saveConfig(); }
}
void loadSchedules(void)
{
  schedule_count=0;
  File f=LittleFS.open(STATION_CONFIG_FILE,"r");
  if (!f) {
    // Preserve original first-install JJY behaviour, now normalized against BLE.
    schedules[0]={SN_JJY_E,0,1440}; schedule_count=1;
    normalizeBluetoothSlots();
    Serial.println("No schedule file: default all-day JJY, conflicting Bluetooth disabled");
    return;
  }
  DynamicJsonDocument doc(8192);
  DeserializationError err=deserializeJson(doc,f); f.close();
  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size()>MAX_SCHEDULES) {
    Serial.println("Invalid schedule: fail safe to RF-off until resaved");
    return;
  }
  JsonArray a=doc.as<JsonArray>();
  TimeSchedule candidate[MAX_SCHEDULES]; int count=0;
  for (JsonVariant v:a) {
    if (!validScheduleJson(v.as<JsonVariantConst>(),candidate[count])) {
      Serial.println("Invalid schedule entry: RF-off fail safe");
      return;
    }
    ++count;
  }
  memcpy(schedules,candidate,count*sizeof(TimeSchedule));
  schedule_count=count; // [] intentionally means no radio schedule, RF remains off.
  normalizeBluetoothSlots();
  Serial.printf("Loaded %d validated schedules\n",schedule_count);
}

bool saveSchedules(void)
{
  File f=LittleFS.open(SCHEDULE_TEMP_FILE,"w");
  if (!f) { Serial.println("Schedule save failed: temp open"); return false; }
  DynamicJsonDocument doc(8192);
  JsonArray a=doc.to<JsonArray>();
  for (int i=0;i<schedule_count;++i) {
    JsonObject o=a.createNestedObject();
    o["station"]=schedules[i].station;
    o["start"]=schedules[i].start_min;
    o["end"]=schedules[i].end_min;
  }
  size_t expected=measureJson(doc),written=serializeJson(doc,f);
  f.flush(); f.close();
  if (expected!=written || !LittleFS.rename(SCHEDULE_TEMP_FILE,STATION_CONFIG_FILE)) {
    Serial.println("Schedule save failed: previous file retained"); return false;
  }
  normalizeBluetoothSlots();
  saveConfig();
  radioRequestRefresh();
  Serial.println("Schedules saved atomically"); return true;
}

void applyCurrentSchedule(void)
{
  // Never broadcast a bogus date from an unsynchronized power-on clock.
  // During holdover the conservative drift/error budget determines trust.
  if (!clockTrusted()) {
    applicable_count=0; current_schedule_index=-1;
    last_station=-1; makebitpattern=nullptr;
    if (!silenceRfCarrier()) return;
    digitalWrite(PIN_LED,LOW); ampmod=0;
    radioBleArbiter.releaseRf();
    btRadioSuspendRequested = false;
    return;
  }
  // Full-time mode ignores schedules, but continues transmitting the
  // selected station's normal clock/time-code signal 24/7.
  if (full_time_tx) {
    applicable_count = 0;
    current_schedule_index = -1;
    if (!radioBleArbiter.requestRf()) {
      btRadioSuspendRequested = true;
      if (carrierReady) ledcWrite(PIN_RADIO, 0);
      last_station = -1; makebitpattern = nullptr; ampmod = 0;
      digitalWrite(PIN_LED, LOW);
      return;
    }
    if (!carrierReady || last_station != full_time_station) setstation(full_time_station);
    return;
  }

  // Calculate current minute of day (0-1439)
  int current_min = nowtm.tm_hour * 60 + nowtm.tm_min;
  
  // Find all applicable schedules for current time
  applicable_count = 0;
  for (int i = 0; i < schedule_count; i++) {
    if (current_min >= schedules[i].start_min && 
        current_min < schedules[i].end_min) {
      applicable_schedules[applicable_count] = i;
      applicable_count++;
    }
  }
  
  int new_station = -1;  // No matching schedule means RF is OFF.
  
  if (applicable_count == 0) {
    // No matching scheduled period: stop RF, not a fallback transmission.
    current_schedule_index = -1;
  } else if (applicable_count == 1) {
    // Single schedule, use it
    current_schedule_index = 0;
    new_station = schedules[applicable_schedules[0]].station;
  } else {
    // Multiple schedules - rotate through them. The active set can shrink
    // between seconds; clamp immediately so a stale index can never select a
    // schedule that is no longer applicable.
    unsigned long now = millis();
    if (current_schedule_index < 0 || current_schedule_index >= applicable_count) {
      current_schedule_index = 0;
      last_rotation_time = now;
      // Schedule-rotation diagnostics are deferred; never print in radioTask.
    }
      // Serial.printf("now: %lu\n", now);
      // Serial.printf("last_rotation_time: %lu\n", last_rotation_time);
    // Check if it's time to rotate to next schedule
    if ((now - last_rotation_time) >= (ROTATION_INTERVAL_MINUTES * 60000UL)) {
      current_schedule_index++;

      if (current_schedule_index >= applicable_count) {
        current_schedule_index = 0;
      }
      last_rotation_time = now;
      // Station change is reported in the next queued pattern log.
    }
    
    new_station = schedules[applicable_schedules[current_schedule_index]].station;
  }
  
  if (new_station >= 0 && !radioBleArbiter.requestRf()) {
    btRadioSuspendRequested = true;
    if (carrierReady) ledcWrite(PIN_RADIO, 0);
    last_station = -1; makebitpattern = nullptr; ampmod = 0;
    digitalWrite(PIN_LED, LOW);
    return;
  }
  if (new_station < 0) {
    // Turn hardware OFF before allowing the other core to initialize BLE.
    if (!silenceRfCarrier()) return;
    radioBleArbiter.releaseRf();
    btRadioSuspendRequested = false;
  }
  // Change station if needed
  if (new_station != last_station || (new_station >= 0 && !carrierReady)) {
    if (new_station < 0) {
      last_station = -1;
      makebitpattern = nullptr;
      if (carrierReady) ledcWrite(PIN_RADIO, 0);
      digitalWrite(PIN_LED, LOW);
      ampmod = 0;
    } else {
      setstation(new_station);
    }
  }
}

// Returns true if, in Scheduled/power-save mode, WiFi should currently be
// powered on for a pre-transmission sync window.
//
// Schedule start times (schedules[i].start_min, minutes since local
// midnight) are compared directly against nowtm - which, per stationTime()/
// getlocaltime(), always reflects the currently SELECTED time zone. So a
// schedule set for "1:00" with Tokyo selected means 1:00 Tokyo time, not
// UTC or the device's own location - this was already true structurally
// once NTP setup was made timezone-safe, since nowtm and the
// schedule matching in applyCurrentSchedule() both key off the same
// zone-aware local clock. The wake-window check below uses that same clock.
bool shouldWifiBeOnForSchedule(void)
{
  // Bluetooth watch sync also needs Wi-Fi available during its short
  // preparation/connection window when Scheduled power-save is enabled.
  // This gives the BLE sync an opportunity to request a fresh NTP sync without
  // requiring a radio transmission schedule at the same time.
  if (!full_time_tx) {
    time_t epochNow=time(nullptr); struct tm wifiLocal; localtime_r(&epochNow,&wifiLocal);
    int current_min = wifiLocal.tm_hour * 60 + wifiLocal.tm_min;
    for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
      if (!btSyncEnabled[i] || btProfileDoneToday(btSyncProfile[i])) continue;
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

// Called from loop() every few seconds when wifiPowerMode == WIFI_POWER_SCHEDULED.
// Turns the WiFi radio fully on/off to match shouldWifiBeOnForSchedule(),
// with a mandatory on-window for the first WIFI_BOOT_ON_DURATION_MS after
// boot so the device is always reachable to be configured. Never runs while
// in AP configuration mode.
void updateWifiPowerManagement(void)
{
  static int appliedPowerMode = -1;
  if (WiFi.getMode() != WIFI_OFF && appliedPowerMode != wifiPowerMode) {
    if (WiFi.setSleep(wifiPowerMode != WIFI_POWER_ALWAYS_ON)) appliedPowerMode = wifiPowerMode;
  }
  if (ap_mode) return;
  if (wifiPowerMode != WIFI_POWER_SCHEDULED) {
    if (!wifiRadioEnabled) { wifiRadioEnabled=true; ntpstart(); }
    return;
  }

  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 5000) return;
  lastCheck = now;

  bool wantOn = (now - bootMillis < WIFI_BOOT_ON_DURATION_MS) || shouldWifiBeOnForSchedule();

  if (wantOn && !wifiRadioEnabled) {
    Serial.println("WiFi power-save: waking radio for sync window");
    wifiRadioEnabled = true;
    ntpstart();
    if (WiFi.status() == WL_CONNECTED) {
      initWebServer();
      digitalWrite(PIN_ONBOARD_LED, HIGH);
    }
  } else if (!wantOn && wifiRadioEnabled) {
    Serial.println("WiFi power-save: sync window closed, powering down radio");
    wifiRadioEnabled = false;
    ntpstop(); // stops SNTP cleanly, then disconnects and powers off the radio
  }
}

void checkWiFiConnection(void)
{
  if (ap_mode) {
    return;  // Already in AP mode
  }
  // In Scheduled power-save mode, WiFi being disconnected while
  // wifiRadioEnabled is false is intentional - don't fight our own sleep
  // by trying to reconnect here.
  if (wifiPowerMode == WIFI_POWER_SCHEDULED && !wifiRadioEnabled) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_connect_start=0;
    if (!webServerStarted) initWebServer();
    return;
  }
  // Check if WiFi is still connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost, attempting to reconnect...");
    if (wifi_connect_start==0) wifi_connect_start=millis();
    WiFi.reconnect();
  }
  
  // If connection takes too long, switch to AP mode
  if (WiFi.status() != WL_CONNECTED && wifi_connect_start &&
      (millis() - wifi_connect_start) > WIFI_CONNECT_TIMEOUT) {
    Serial.println("WiFi connection timeout, starting AP mode...");
    startAPMode();
  }
}

void startAPMode(void)
{
  radioSetPaused(true);  // Command radioTask to silence RF during AP setup
  ap_mode = true;
  if (webServerStarted) { server.stop(); webServerStarted=false; }
  WiFi.mode(WIFI_AP);
  // Use the WiFi MAC for the AP suffix (works on ESP32 and ESP32-C3).
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ap_name[32];
  snprintf(ap_name, sizeof(ap_name), "%s_%02X%02X%02X",
           DEVICENAME_PREFIX, mac[3], mac[4], mac[5]);
  
  WiFi.softAP(ap_name, "12345678");  // Open AP or with default password
  
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP Mode started. SSID: %s, IP: %s\n", ap_name, IP.toString().c_str());
  
  initWebServer();
  Serial.println("Web server started");
}

void stopAPMode(void)
{
  ap_mode = false;
  if (webServerStarted) { server.stop(); webServerStarted=false; }
  WiFi.softAPdisconnect(true);
  // Resume STA mode web server after connecting to Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(wifiPowerMode != WIFI_POWER_ALWAYS_ON);
  radioSetPaused(false);  // Resume radio via radioTask
  Serial.println("AP mode stopped, web server continues on WiFi");
}

String jsonQuoted(const String &value) {
  DynamicJsonDocument d(value.length() * 2 + 64);
  d.set(value);
  String j; serializeJson(d,j); return j;
}
static bool bluetoothSettingsMutable(void) {
  if (btBleBusy || btPairModeActive || btPairRequested) return false;
  if (btWindowActive) stopBluetoothWindow();
  btManualSyncRequested = false;
  return true;
}

static String bluetoothState(void) {
  if (btRadioSuspendRequested || radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned())
    return String(btPhase).indexOf("failed") >= 0 ? btPhase : "Paused for radio transmission";
  if (btBleBusy) return btPhase;
  if (btPairRequested || btPairModeActive) return "Pairing";
  if (btManualSyncRequested) return "Waiting for watch";
  if (btWindowActive && btSyncActiveSlot >= 0) return "Automatic sync window active";
  if (btWindowActive) return btScan && btScan->isScanning() ? "Scanning" : "Waiting for watch";
  if (btAlwaysWaitEnabled && btProfileAddress[btManualProfile].length() != 17) return "No watch paired";
  return btPhase;
}

static String bluetoothProfilesJson(void) {
  String json = "[";
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    if (i) json += ",";
    json += "{\"id\":" + String(i) + ",\"bound\":" +
            String(btProfileAddress[i].length()==17 ? "true" : "false") +
            ",\"name\":" + jsonQuoted(btProfileName[i]) +
            ",\"address\":" + jsonQuoted(btProfileAddress[i]) +
            ",\"protocol\":" + String(btProfileProtocol[i]) +
            ",\"done_today\":" + String(btProfileDoneToday(i) ? "true" : "false") + "}";
  }
  return json + "]";
}

void initWebServer(void)
{
  if (webServerStarted) return;
  if (!webRoutesRegistered) {
  // Register routes only once. Restart listening after Wi-Fi sleep/AP switch.
  // Serve index page
  server.on("/", HTTP_GET, []() {
    sendIndexPage();
  });
  
  // API endpoints
  server.on("/api/config", HTTP_GET, []() {
    String json = "{\"ssid\":" + jsonQuoted(String(ssid)) + ",\"timezone\":" +
                  jsonQuoted(timezone_name) + ",\"full_time_tx\":" +
                  String(full_time_tx ? "true" : "false") +
                  ",\"full_time_station\":" + String(full_time_station) +
                  ",\"transmission_offset_minutes\":" + String(transmission_offset_minutes) +
                  ",\"wifi_power_mode\":" + String(wifiPowerMode);
    json += ",\"bt_day_complete\":" + String(btSyncDayComplete ? "true" : "false");
    json += ",\"bt_manual_protocol\":" + String((int)btManualProtocol);
    json += ",\"bt_manual_profile\":" + String((int)btManualProfile);
    json += ",\"bt_active_protocol\":" + String((int)btActiveProtocol);
    json += ",\"bt_active_profile\":" + String((int)btActiveProfile);
    json += ",\"bt_last_sync_status\":" + jsonQuoted(btLastSyncStatus) +
            ",\"bt_last_sync_date\":" + jsonQuoted(btLastSyncDate);
    json += ",\"bt_times\":[";
    for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
      if (i > 0) json += ",";
      json += "{\"minute\":" + String(btSyncTimes[i]) +
              ",\"enabled\":" + String(btSyncEnabled[i] ? "true" : "false") +
              ",\"protocol\":" + String((int)btSyncProtocol[i]) +
              ",\"profile\":" + String((int)btSyncProfile[i]) +
              ",\"done_today\":" + String(btProfileDoneToday(btSyncProfile[i]) ? "true" : "false") +
              ",\"conflict\":" + String((bluetoothTimeSlotConflicts(btSyncTimes[i]) || btOverlapsOtherEnabledSlot(i,btSyncTimes[i])) ? "true" : "false") + "}";
    }
    json += "],\"bt_profiles\":" + bluetoothProfilesJson();
    json += ",\"bt_always_wait\":" + String(btAlwaysWaitEnabled ? "true" : "false");
    json += ",\"bt_state\":" + jsonQuoted(bluetoothState());
    json += ",\"bt_pairing\":" + String(btPairModeActive || btPairRequested ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/config", HTTP_POST, []() {
    // Settings can be updated independently. Full-time TX therefore takes
    // effect immediately without requiring the Save settings button.
    bool wifiChanged = false;
    bool encodingChanged = false;

    if (server.hasArg("ssid")) {
      String requestedSsid = server.arg("ssid");
      if (requestedSsid.length() >= sizeof(ssid)) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"SSID is too long\"}"); return;
      }
      if (requestedSsid != String(ssid)) {
        requestedSsid.toCharArray(ssid, sizeof(ssid));
        wifiChanged = true;
      }
    }
    if (server.hasArg("password") && server.arg("password").length()>0) {
      String requestedPassword = server.arg("password");
      if (requestedPassword.length() >= sizeof(passwd)) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Wi-Fi password is too long\"}"); return;
      }
      requestedPassword.toCharArray(passwd, sizeof(passwd));
      wifiChanged = true;
    }
    if (server.hasArg("timezone")) {
      String requestedZone = server.arg("timezone");
      if (!validTimezoneName(requestedZone)) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Unsupported time zone\"}"); return;
      }
      if (requestedZone != timezone_name) {
        if (!radioSetPaused(true)) {
          server.send(503, "application/json", "{\"status\":\"error\",\"message\":\"RF pause timed out\"}"); return;
        }
        timezone_name = requestedZone;
        applyTimezone();
        radioSetPaused(false);
        encodingChanged = true;
      }
    }
    if (server.hasArg("full_time_tx")) {
      String v = server.arg("full_time_tx");
      bool requestedFull = (v == "1" || v.equalsIgnoreCase("true"));
      if (requestedFull) {
        for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
          if (btSyncEnabled[i]) {
            server.send(409, "application/json",
                        "{\"status\":\"error\",\"message\":\"Disable Bluetooth automatic sync before enabling full-time radio transmission\"}");
            return;
          }
        }
      }
      full_time_tx = requestedFull;
      encodingChanged = true;
    }
    if (server.hasArg("full_time_station")) {
      int station = server.arg("full_time_station").toInt();
      full_time_station = station >= SN_JJY_E && station <= SN_BPC ? station : SN_JJY_E;
      encodingChanged = true;
    }
    if (server.hasArg("transmission_offset_minutes")) {
      int offset = server.arg("transmission_offset_minutes").toInt();
      transmission_offset_minutes = offset >= -720 && offset <= 840 ? offset : 0;
      encodingChanged = true;
    }
    if (server.hasArg("wifi_power_mode")) {
      int m = server.arg("wifi_power_mode").toInt();
      wifiPowerMode = (m == WIFI_POWER_SCHEDULED) ? WIFI_POWER_SCHEDULED : WIFI_POWER_ALWAYS_ON;
      // No immediate WiFi action needed here: the request just arrived over
      // WiFi, so the radio is already on. updateWifiPowerManagement() in
      // loop() takes over the on/off decisions from this point onward.
      // (saveConfig() runs unconditionally below.)
    }

    if (server.hasArg("bt_always_wait")) {
      String v = server.arg("bt_always_wait");
      if (v != "0" && v != "1" && v != "true" && v != "false") {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid Always Wait value\"}"); return;
      }
      btAlwaysWaitEnabled = v == "1" || v == "true";
    }
    if (server.hasArg("bt_manual_protocol")) {
      int p = server.arg("bt_manual_protocol").toInt();
      if (server.arg("bt_manual_protocol") != String(p) || !validBtProtocol(p) ||
          !bluetoothSettingsMutable()) {
        server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"invalid protocol or Bluetooth window is active\"}");
        return;
      }
      btManualProtocol = (uint8_t)p;
      saveConfig();
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
    if (server.hasArg("bt_manual_profile")) {
      int p=server.arg("bt_manual_profile").toInt();
      if (server.arg("bt_manual_profile") != String(p) || p<0 || p>=BT_WATCH_PROFILE_COUNT || !bluetoothSettingsMutable()) {
        server.send(409,"application/json","{\"status\":\"error\",\"message\":\"invalid/busy profile\"}");return;
      }
      btManualProfile=(uint8_t)p; saveConfig();
      server.send(200,"application/json","{\"status\":\"ok\"}");return;
    }
    if (server.hasArg("bt_rebind_profile")) {
      // Legacy clients must use the explicit, transactional Pair Watch route.
      server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Use Pair Watch; the existing binding is retained until replacement succeeds\"}");
      return;
    }
    if (server.hasArg("bt_slot")) {
      int slot = server.arg("bt_slot").toInt();
      if (slot < 0 || slot >= BT_SYNC_SLOT_COUNT) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid Bluetooth slot\"}");
        return;
      }
      if (server.hasArg("bt_slot_time")) {
        String requested=server.arg("bt_slot_time");
        int m=requested.toInt();
        if (requested != String(m) || m<0 || m>=1440 ||
            bluetoothTimeSlotConflicts(m) || btOverlapsOtherEnabledSlot(slot,m) || !bluetoothSettingsMutable()) {
          server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Time invalid, busy, or overlaps a radio/Bluetooth window\"}"); return;
        }
        btSyncTimes[slot]=m; saveConfig();
        server.send(200,"application/json","{\"status\":\"ok\"}");return;
      }
      if (server.hasArg("bt_slot_profile")) {
        int p=server.arg("bt_slot_profile").toInt();
        if (server.arg("bt_slot_profile") != String(p) || p<0 || p>=BT_WATCH_PROFILE_COUNT || !bluetoothSettingsMutable()) {
          server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Invalid/busy watch profile\"}");return;
        }
        // One logical watch has one protocol across its enabled sync slots.
        for (int i=0;i<BT_SYNC_SLOT_COUNT;++i)
          if (i!=slot && btSyncEnabled[i] && btSyncProfile[i]==p &&
              btSyncProtocol[i]!=btSyncProtocol[slot]) {
            server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Watch profile uses another protocol\"}");return;
          }
        btSyncProfile[slot]=p; saveConfig();
        server.send(200,"application/json","{\"status\":\"ok\"}");return;
      }
      if (server.hasArg("bt_slot_protocol")) {
        int p = server.arg("bt_slot_protocol").toInt();
        if (server.arg("bt_slot_protocol") != String(p) || !validBtProtocol(p) ||
            !bluetoothSettingsMutable()) {
          server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"invalid protocol or Bluetooth window is active\"}");
          return;
        }
        for (int i=0;i<BT_SYNC_SLOT_COUNT;++i)
          if (i!=slot && btSyncEnabled[i] && btSyncProfile[i]==btSyncProfile[slot] &&
              btSyncProtocol[i]!=p) {
            server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Watch profile uses another protocol\"}");return;
          }
        btSyncProtocol[slot] = (uint8_t)p;
        saveConfig();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
        return;
      }
      if (!server.hasArg("bt_slot_enabled")) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bluetooth slot update missing enabled value\"}");
        return;
      }
      bool en = server.hasArg("bt_slot_enabled") &&
                (server.arg("bt_slot_enabled") == "1" ||
                 server.arg("bt_slot_enabled").equalsIgnoreCase("true"));
      if (en && btProfileDoneToday(btSyncProfile[slot])) {
        server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"This watch protocol already synced today\"}");
        return;
      }
      if (en && (bluetoothTimeSlotConflicts(btSyncTimes[slot]) ||
                 btOverlapsOtherEnabledSlot(slot,btSyncTimes[slot]))) {
        server.send(409, "application/json",
                    "{\"status\":\"error\",\"message\":\"Bluetooth time conflicts with a radio transmission schedule\"}");
        return;
      }
      btSyncEnabled[slot] = en;
      saveConfig();
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }


    if (encodingChanged) {
      // Hand the actual regeneration off to radioTask (see
      // encodingRefreshRequested above) rather than touching
      // txSymbol[]/txEnvelope[] here, since this handler runs on the web
      // server task and those arrays are read by radioTask every 1 ms.
      // radioTask picks this up on its very next tick, so the transmitted
      // signal (and therefore the UI, which polls /api/status once a
      // second) reflects the change essentially immediately.
      radioRequestRefresh();
    } else {
      // Nothing encoding-related changed, but a schedule/station switch may
      // still be needed the ordinary way (e.g. re-evaluate full_time_tx
      // toggling off with no other field changed is covered above; this
      // branch only runs when only WiFi credentials were posted).
      radioRequestRefresh();
    }
    saveConfig();

    server.send(200, "application/json",
                String("{\"status\":\"ok\",\"full_time_tx\":") +
                (full_time_tx ? "true" : "false") +
                ",\"station\":" + String(last_station) + "}");

    // Only reconnect when Wi-Fi credentials were changed.
    if (wifiChanged) {
      Serial.printf("Connecting to WiFi: %s\n", ssid);
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(wifiPowerMode != WIFI_POWER_ALWAYS_ON);
      wifi_connect_start=millis();
      WiFi.begin(ssid, passwd);
    }
  });

  server.on("/api/stations", HTTP_GET, []() {
    String json = "[";
    for (int i = 0; i < NUM_STATIONS; i++) {
      if (i > 0) json += ",";
      json += "{\"id\":" + String(i) + ",\"name\":\"" + String(station_names[i]) + "\",\"encoding\":\"" + String(stationEncodingName(i)) + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  
  // Returns the full schedule list. Used by the UI's load() to populate the
  // "Transmission schedules" card.
  server.on("/api/schedules", HTTP_GET, []() {
    String json = "[";
    for (int i = 0; i < schedule_count; i++) {
      if (i > 0) json += ",";
      json += "{\"station\":" + String(schedules[i].station) +
              ",\"start\":" + String(schedules[i].start_min) +
              ",\"end\":" + String(schedules[i].end_min) + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  
  // Adds a single schedule entry. NOTE: the bundled web UI never calls this
  // - saveSchedules() in the JS always POSTs the entire list to the plural
  // /api/schedules endpoint below instead, replacing everything at once.
  // Left in place as a simpler per-item option for any external script/
  // integration that might prefer it over resending the whole list, but if
  // you're looking for what the UI's "Save schedules" button actually
  // calls, that's the POST /api/schedules handler further down.
  server.on("/api/schedule", HTTP_POST, []() {
    DynamicJsonDocument doc(256);
    if (!server.hasArg("plain") || deserializeJson(doc,server.arg("plain"))) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"invalid JSON\"}"); return;
    }
    TimeSchedule next;
    if (!validScheduleJson(doc.as<JsonVariantConst>(),next) || schedule_count>=MAX_SCHEDULES) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"invalid schedule / limit reached\"}"); return;
    }
    if (!radioSetPaused(true)) { server.send(503,"application/json","{\"status\":\"error\",\"message\":\"RF pause timed out\"}"); return; }
    schedules[schedule_count++]=next;
    bool saved=saveSchedules();
    if (!saved) --schedule_count;
    radioSetPaused(false);
    if (!saved) { server.send(500,"application/json","{\"status\":\"error\"}"); return; }
    server.send(200,"application/json","{\"status\":\"ok\"}");
  });

  // Replaces the ENTIRE schedule list in one request - this is what the
  // UI's "Save schedules" button actually calls (see saveSchedules() in the
  // JS), sending every row currently shown, every time.
  server.on("/api/schedules", HTTP_POST, []() {
    if (!server.hasArg("plain") || server.arg("plain").length()>8192) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"missing/oversized JSON\"}"); return;
    }
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc,server.arg("plain")) || !doc.is<JsonArray>() ||
        doc.as<JsonArray>().size()>MAX_SCHEDULES) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"invalid schedule list\"}"); return;
    }
    TimeSchedule next[MAX_SCHEDULES]; int n=0;
    for (JsonVariant v:doc.as<JsonArray>()) {
      if (!validScheduleJson(v.as<JsonVariantConst>(),next[n])) {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"invalid station or time range\"}"); return;
      }
      ++n;
    }
    if (!radioSetPaused(true)) { server.send(503,"application/json","{\"status\":\"error\",\"message\":\"RF pause timed out\"}"); return; }
    TimeSchedule previous[MAX_SCHEDULES]; memcpy(previous,schedules,sizeof(previous));
    int oldCount=schedule_count;
    memcpy(schedules,next,n*sizeof(TimeSchedule)); schedule_count=n;
    bool saved=saveSchedules();
    if (!saved) { memcpy(schedules,previous,sizeof(previous)); schedule_count=oldCount; }
    radioSetPaused(false);
    if (!saved) { server.send(500,"application/json","{\"status\":\"error\",\"message\":\"flash save failed\"}");return; }
    server.send(200,"application/json","{\"status\":\"ok\"}");
  });

  // Deletes a single schedule by index. Like the singular POST above, the
  // bundled UI doesn't call this either - it just removes the row locally
  // and resends the whole list via POST /api/schedules instead. Kept for
  // the same reason.
  server.on("/api/schedule", HTTP_DELETE, []() {
    if (server.hasArg("index")) {
      int idx = server.arg("index").toInt();
      if (idx >= 0 && idx < schedule_count) {
        if (!radioSetPaused(true)) { server.send(503,"application/json","{\"status\":\"error\",\"message\":\"RF pause timed out\"}"); return; }
        TimeSchedule previous[MAX_SCHEDULES]; memcpy(previous,schedules,sizeof(previous));
        int oldCount=schedule_count;
        for (int i = idx; i < schedule_count - 1; i++) schedules[i] = schedules[i + 1];
        --schedule_count;
        bool saved=saveSchedules();
        if (!saved) { memcpy(schedules,previous,sizeof(previous)); schedule_count=oldCount; }
        radioSetPaused(false);
        if (!saved) { server.send(500,"application/json","{\"status\":\"error\"}"); return; }
        server.send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid index\"}");
      }
    } else {
      server.send(400, "application/json", "{\"error\":\"missing index\"}");
    }
  });
  
  // Primary status endpoint, polled once a second by the UI's tick(). Reports
  // both the plain local clock (time/date, in the selected timezone) and the
  // actual civil time currently being encoded into the radio signal
  // (tx_time) - these can differ by up to a minute for DCF77/MSF, which
  // transmit the UPCOMING minute rather than the current one (tx_next_minute
  // tells the UI which case it's in).
  server.on("/api/status", HTTP_GET, []() {
    // Build response with rotation information
    // Use the ESP32's configured IANA timezone for the displayed local time.
    time_t displayEpoch = time(nullptr);
    struct tm displayLocal;
    localtime_r(&displayEpoch, &displayLocal);
    char localTime[16];
    char localDate[16];
    strftime(localTime, sizeof(localTime), "%H:%M:%S", &displayLocal);
    strftime(localDate, sizeof(localDate), "%Y-%m-%d", &displayLocal);

    // Calculate exactly which civil time the active encoder is using.
    // DCF77 and MSF encode the following minute; the other current encoders
    // use the current minute/frame. The user's transmission offset is included.
    time_t utcNow;
    time(&utcNow);
    bool nextMinute = (last_station == SN_DCF77 || last_station == SN_MSF);
    struct tm txTm;
    stationTime(utcNow, last_station, nextMinute, txTm);
    char txTime[32];
    strftime(txTime, sizeof(txTime), "%Y-%m-%d %H:%M:%S", &txTm);

    // This handler is polled once a second by every open browser tab (see
    // tick() in the UI), making it the hottest of all the web handlers.
    // Reserving the String's capacity upfront avoids the repeated
    // grow-and-copy reallocations that would otherwise happen on nearly
    // every one of the += appends below - cheap to do, and removes
    // needless heap churn from a handler that runs continuously.
    String json;
    json.reserve(512);
    json = "{\"time\":\"" + String(localTime) + "\",\"date\":\"" + String(localDate) + "\"";
    json += ",\"timezone\":\"" + timezone_name + "\"";
    json += ",\"tx_time\":\"" + String(txTime) + "\"";
    
    // Publish one coherent schedule set, rather than racing radioTask updates.
    int count, activeSchedules[MAX_SCHEDULES];
    portENTER_CRITICAL(&radioLogMux);
    count = radioApplicableCountSnapshot;
    memcpy(activeSchedules, radioApplicableSchedulesSnapshot, sizeof(activeSchedules));
    portEXIT_CRITICAL(&radioLogMux);
    json += ",\"applicable_schedules\":[";
    for (int i = 0; i < count; i++) {
      if (i > 0) json += ",";
      json += String(activeSchedules[i]);
    }
    json += "]";
    
    json += ",\"full_time_tx\":" + String(full_time_tx ? "true" : "false");
  json += ",\"ntp_interval\":" + String(ntpIntervalSec);
    portENTER_CRITICAL(&clockMux);
    double driftSnapshot = ntpDriftPpm;
    portEXIT_CRITICAL(&clockMux);
    json += ",\"ntp_drift_ppm\":" + String(driftSnapshot, 3);
    json += ",\"station\":" + String(last_station);
    json += ",\"radio_active\":" + String(radioBleArbiter.rfOwned() && last_station >= 0 && !radioPaused ? "true" : "false");
    json += ",\"tx_next_minute\":" + String(last_station >= 0 && nextMinute ? "true" : "false");
    json += ",\"bt_day_complete\":" + String(btSyncDayComplete ? "true" : "false");
    json += ",\"bt_manual_protocol\":" + String((int)btManualProtocol);
    json += ",\"bt_manual_profile\":" + String((int)btManualProfile);
    json += ",\"bt_active_protocol\":" + String((int)btActiveProtocol);
    json += ",\"bt_active_profile\":" + String((int)btActiveProfile);
    json += ",\"bt_last_sync_status\":" + jsonQuoted(btLastSyncStatus);
    json += ",\"bt_always_wait\":" + String(btAlwaysWaitEnabled ? "true" : "false");
    json += ",\"bt_state\":" + jsonQuoted(bluetoothState());
    json += ",\"bt_profiles\":" + bluetoothProfilesJson();
    json += ",\"wifi_power_mode\":" + String(wifiPowerMode);
    json += ",\"bt_pairing\":" + String((btPairModeActive || btPairRequested) ? "true" : "false");
    json += ",\"bt_last_sync_date\":\"" + btLastSyncDate + "\"";
    json += ",\"bt_times\":[";
    for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
      if (i > 0) json += ",";
      json += "{\"minute\":" + String(btSyncTimes[i]) +
              ",\"enabled\":" + String(btSyncEnabled[i] ? "true" : "false") +
              ",\"protocol\":" + String((int)btSyncProtocol[i]) +
              ",\"profile\":" + String((int)btSyncProfile[i]) +
              ",\"done_today\":" + String(btProfileDoneToday(btSyncProfile[i]) ? "true" : "false") +
              ",\"conflict\":" + String((bluetoothTimeSlotConflicts(btSyncTimes[i]) || btOverlapsOtherEnabledSlot(i,btSyncTimes[i])) ? "true" : "false") + "}";
    }
    json += "]";
    json += ",\"clock_state\":\"" + String(clockConfidence()) + "\"";
    json += ",\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/diagnostics", HTTP_GET, []() {
    DynamicJsonDocument d(2048);
    d["firmware"]=FIRMWARE_VERSION;
    d["uptime_sec"]=millis()/1000UL;
    d["heap_free"]=ESP.getFreeHeap();
    d["heap_min_free"]=ESP.getMinFreeHeap();
    d["clock_state"]=clockConfidence();
    d["ntp_age_sec"]=clockAgeSeconds()==UINT32_MAX?-1:(int32_t)clockAgeSeconds();
    portENTER_CRITICAL(&clockMux);
    uint32_t syncCountSnapshot = clockNtpSyncCount;
    portEXIT_CRITICAL(&clockMux);
    d["ntp_sync_count"]=syncCountSnapshot;
    d["clock_error_est_sec"]=clockEstimatedError();
    d["ntp_interval_sec"]=ntpIntervalSec.load();
    d["wifi_connected"]=WiFi.status()==WL_CONNECTED;
    d["wifi_ip"]=WiFi.localIP().toString();
    d["radio_active"]=radioBleArbiter.rfOwned() && last_station>=0 && !radioPaused;
    d["bt_controller_off"]=esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_IDLE;
    d["bt_rf_requested"]=radioBleArbiter.rfRequested();
    d["bt_state"]=bluetoothState();
    d["bt_always_wait"]=btAlwaysWaitEnabled;
    d["radio_paused"]=radioPaused.load();
    d["rf_silence_failed"]=rfSilenceFailed.load();
    d["timer_running"]=istimerstarted!=0;
    d["carrier_hz"]=carrierFrequencyHz.load();
    d["station"]=last_station.load();
    d["boundary_delay_us"]=radioBoundaryErrorUs.load();
    d["boundary_delay_worst_us"]=radioBoundaryWorstUs.load();
    d["missed_second_boundaries"]=radioMissedBoundaries.load();
    d["bt_window_active"]=btWindowActive.load();
    d["bt_pair_mode"]=btPairModeActive || btPairRequested;
    d["bt_connection_attempts"]=btConnectionAttempts;
    d["bt_acked_writes"]=btWriteAcknowledgements;
    d["bt_notifications"]=btNotifications.load();
    d["bt_response_errors"]=btResponseErrors.load();
    d["bt_delivery_evidence"]=btDeliveryEvidence;
    d["littlefs_total"]=LittleFS.totalBytes();
    d["littlefs_used"]=LittleFS.usedBytes();
    String j; serializeJson(d,j); server.send(200,"application/json",j);
  });

  server.on("/api/settings", HTTP_POST, []() {
    if (!server.hasArg("bt_always_wait")) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Missing Always Wait setting\"}"); return;
    }
    String v=server.arg("bt_always_wait");
    if (v!="0" && v!="1" && v!="true" && v!="false") {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid Always Wait value\"}"); return;
    }
    btAlwaysWaitEnabled=(v=="1" || v=="true");
    if (!btAlwaysWaitEnabled && btPersistentWaitActive && !btBleBusy) {
      stopBluetoothWindow(); setBluetoothPhase("Idle");
    }
    saveConfig();
    server.send(200,"application/json","{\"status\":\"ok\"}");
  });

  server.on("/api/bluetooth-pair", HTTP_POST, []() {
    if (radioScheduleActiveNow()) {
      server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Radio transmission has priority; pair after RF finishes\"}"); return;
    }
    if (!clockTrusted()) {
      server.send(503,"application/json","{\"status\":\"error\",\"message\":\"No trusted NTP clock yet. Synchronize NTP first.\"}"); return;
    }
    if (btPairRequested || btPairModeActive) {
      server.send(200,"application/json","{\"status\":\"ok\",\"state\":\"Pairing\",\"message\":\"Pairing window already active\"}"); return;
    }
    if (btBleBusy) {
      server.send(409,"application/json", "{\"status\":\"error\",\"message\":" + jsonQuoted(bluetoothState()) + "}"); return;
    }
    if (!validBtProtocol(btManualProtocol) || btManualProfile>=BT_WATCH_PROFILE_COUNT) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid watch profile or protocol\"}"); return;
    }
    stopBluetoothWindow();
    btManualSyncRequested=false;
    btPairRequested=true;
    btLastSyncStatus=String("Pairing Watch ")+(btManualProfile+1)+": put the watch into pairing mode";
    server.send(200,"application/json","{\"status\":\"ok\",\"state\":\"Pairing\",\"message\":\"Pairing window opened; previous binding retained until time delivery succeeds\"}");
  });

  server.on("/api/bluetooth-sync", HTTP_POST, []() {
    if (radioScheduleActiveNow()) {
      server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Radio transmission has priority; retry after RF finishes\"}"); return;
    }
    if (!clockTrusted()) {
      server.send(503,"application/json","{\"status\":\"error\",\"message\":\"No trusted NTP clock yet. Synchronize NTP first.\"}"); return;
    }
    if (btPairRequested || btPairModeActive) {
      server.send(409,"application/json","{\"status\":\"error\",\"state\":\"Pairing\",\"message\":\"Pairing is active; finish pairing before Sync Now\"}"); return;
    }
    if (btProfileAddress[btManualProfile].length()!=17 || btProfileProtocol[btManualProfile]!=btManualProtocol) {
      server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Pair Watch first for the selected profile and protocol\"}"); return;
    }
    // If a GATT transaction is finishing, queue the retry; otherwise next loop
    // stops/reopens the current passive/manual/automatic window safely.
    btManualSyncRequested=true;
    if (!btBleBusy) btLastSyncStatus="Waiting for watch";
    server.send(200,"application/json","{\"status\":\"ok\",\"state\":"+jsonQuoted(bluetoothState())+",\"message\":\"Manual sync window will restart safely\"}");
  });

  // Request an immediate SNTP client restart from the web interface.
  server.on("/api/ntp-sync", HTTP_POST, []() {
    if (WiFi.status() != WL_CONNECTED) {
      server.send(503, "application/json",
                  "{\"status\":\"error\",\"message\":\"Wi-Fi is not connected\"}");
      return;
    }
    // Restart the SNTP state machine without changing the learned adaptive interval.
    sntp_restart();
    Serial.println("NTP sync requested from web UI");
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"message\":\"NTP synchronization requested\"}");
  });

  webRoutesRegistered=true;
  }
  server.begin(); webServerStarted=true;
}

static const uint8_t INDEX_HTML_GZ[] PROGMEM = {
  0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xbd, 0x7d, 0xd9, 0x76, 0xdb, 0x48, 0xb2, 0xe0, 0xbb, 0xbf,
  0x02, 0x66, 0xb9, 0x0c, 0xa2, 0x0d, 0x52, 0x24, 0x45, 0x2d, 0x26, 0x45, 0xea, 0x7a, 0xed, 0xaa, 0xbe, 0x2e, 0xdb, 0x63,
  0xa9, 0xba, 0xba, 0x8f, 0xc7, 0x47, 0x06, 0x89, 0xa4, 0x88, 0x16, 0x08, 0xb0, 0x01, 0x50, 0x4b, 0x51, 0x3c, 0xa7, 0x9e,
  0xe6, 0xcc, 0xc3, 0xbc, 0xdc, 0x3b, 0xf7, 0x65, 0xe6, 0x03, 0xe6, 0x0f, 0xe6, 0xcc, 0xfb, 0xfd, 0x94, 0xfa, 0x92, 0x89,
  0x88, 0x5c, 0x90, 0x09, 0x80, 0x14, 0x65, 0xbb, 0xbb, 0x17, 0x8b, 0x48, 0xe4, 0x12, 0x19, 0x19, 0x7b, 0x46, 0x26, 0x8e,
  0x1e, 0xbe, 0x7c, 0xf7, 0xe2, 0xf4, 0xaf, 0xef, 0x5f, 0x59, 0xd3, 0x6c, 0x16, 0x0e, 0x1f, 0x1c, 0xe1, 0x1f, 0x2b, 0xf4,
  0xa2, 0xf3, 0x41, 0x8d, 0x45, 0x35, 0x2c, 0x60, 0x9e, 0x0f, 0x7f, 0x66, 0x2c, 0xf3, 0xac, 0xf1, 0xd4, 0x4b, 0x52, 0x96,
  0x0d, 0x6a, 0x3f, 0x9f, 0xbe, 0x6e, 0x1c, 0xd6, 0x64, 0x71, 0xe4, 0xcd, 0xd8, 0xa0, 0x76, 0x19, 0xb0, 0xab, 0x79, 0x9c,
  0x64, 0x35, 0x6b, 0x1c, 0x47, 0x19, 0x8b, 0xa0, 0xda, 0x55, 0xe0, 0x67, 0xd3, 0x81, 0xcf, 0x2e, 0x83, 0x31, 0x6b, 0xd0,
  0x83, 0x1b, 0x44, 0x41, 0x16, 0x78, 0x61, 0x23, 0x1d, 0x7b, 0x21, 0x1b, 0xb4, 0x5d, 0xd9, 0xaa, 0x31, 0x09, 0xb2, 0xc1,
  0x38, 0xbe, 0x64, 0x49, 0xa1, 0xdb, 0x71, 0x1c, 0xc6, 0x09, 0x54, 0x9f, 0xb2, 0x19, 0xd3, 0xba, 0x0e, 0x83, 0xf3, 0x69,
  0x66, 0xf9, 0x5e, 0x72, 0x81, 0xf5, 0xb3, 0x20, 0x0b, 0xd9, 0xf0, 0x83, 0xe7, 0x07, 0xf1, 0x8b, 0x30, 0x1e, 0x5f, 0x1c,
  0xed, 0xf0, 0x92, 0x07, 0x47, 0x69, 0x76, 0x83, 0x7f, 0x7b, 0x49, 0x1c, 0x67, 0xcb, 0x07, 0x56, 0xa3, 0x31, 0x3a, 0xef,
  0x7d, 0x37, 0xe9, 0x4e, 0xf6, 0x27, 0x5e, 0xbf, 0xd1, 0x98, 0x7b, 0x11, 0x0b, 0xe1, 0x99, 0xfe, 0x23, 0x9f, 0x3b, 0x50,
  0x70, 0x38, 0xf1, 0x26, 0x63, 0x28, 0x08, 0xa2, 0x8b, 0xde, 0x77, 0xed, 0x56, 0xfb, 0xb0, 0x73, 0x08, 0x4f, 0xb3, 0x45,
  0xc6, 0xfc, 0xde, 0x77, 0xfb, 0xfb, 0x07, 0xad, 0xc3, 0x3d, 0x78, 0x0e, 0x83, 0x88, 0xf5, 0xbe, 0x63, 0x5d, 0x76, 0xc0,
  0xc6, 0x7d, 0xea, 0x3c, 0xf1, 0x22, 0xa8, 0xd0, 0xde, 0xdb, 0x63, 0x0c, 0xfb, 0xa3, 0x67, 0xe8, 0xaf, 0xd5, 0xea, 0x32,
  0x36, 0x82, 0x82, 0xf3, 0x38, 0x86, 0xf7, 0xad, 0xfd, 0x83, 0xfd, 0xee, 0x81, 0x78, 0x44, 0x80, 0xd8, 0x78, 0xe2, 0x4f,
  0x76, 0xa1, 0xe0, 0xca, 0x4b, 0xa2, 0xde, 0x77, 0xa3, 0xbd, 0x2e, 0x8c, 0x20, 0x1e, 0x09, 0xe0, 0xc9, 0xc4, 0x83, 0xf6,
  0x34, 0x84, 0x07, 0x1d, 0x8c, 0xba, 0x9d, 0xdd, 0x36, 0x56, 0x80, 0x27, 0x7a, 0xcf, 0x26, 0xbb, 0x93, 0x0e, 0x3c, 0xc7,
  0x93, 0x49, 0xef, 0xbb, 0xee, 0xc1, 0x5e, 0x77, 0x1f, 0xbb, 0x4f, 0xa7, 0x9e, 0x1f, 0x5f, 0xf5, 0x5a, 0x56, 0x7b, 0x7f,
  0x7e, 0x6d, 0x75, 0x3b, 0xf0, 0x4f, 0x72, 0x3e, 0xf2, 0xea, 0xed, 0x7d, 0xb7, 0xd3, 0x75, 0xbb, 0x2d, 0xb7, 0xd9, 0x3a,
  0x74, 0xa8, 0xd7, 0x04, 0x50, 0xb7, 0x48, 0x7b, 0xed, 0xc3, 0xf9, 0x35, 0xb4, 0xcb, 0xbc, 0x79, 0xaf, 0x0b, 0x4d, 0x1e,
  0xac, 0x1e, 0xfc, 0xcb, 0x8c, 0xf9, 0x81, 0x57, 0x9f, 0x27, 0x6c, 0xc2, 0x92, 0xb4, 0xa1, 0xaf, 0x46, 0x0f, 0xb1, 0xef,
  0x2c, 0x4d, 0xd4, 0xb6, 0x46, 0xad, 0x49, 0x7b, 0x3f, 0x47, 0x6d, 0xbb, 0x03, 0xb8, 0xdb, 0xd5, 0x50, 0xdb, 0x3e, 0x68,
  0x4f, 0x3a, 0x0a, 0xb5, 0x93, 0x0e, 0xac, 0xc5, 0x41, 0x8e, 0xda, 0xa7, 0x87, 0x5e, 0x67, 0xb4, 0xab, 0x50, 0xdb, 0x39,
  0xd8, 0xed, 0x74, 0xbb, 0x3a, 0x6a, 0x0f, 0xbb, 0x9e, 0x3f, 0xd1, 0x51, 0xbb, 0xd7, 0x39, 0x1c, 0x51, 0x01, 0x47, 0xed,
  0xc1, 0x1e, 0x6b, 0x79, 0x3a, 0x6a, 0x5b, 0x87, 0xbb, 0xa3, 0xce, 0x81, 0x42, 0xed, 0x84, 0x8d, 0x0f, 0xbb, 0x23, 0x0d,
  0xb5, 0xbb, 0x5e, 0xe7, 0x69, 0x4b, 0x43, 0xed, 0xc4, 0x87, 0x82, 0x51, 0x8e, 0xda, 0xdd, 0x51, 0xbb, 0xdd, 0x6e, 0x49,
  0xd4, 0x2a, 0xf8, 0x14, 0x6a, 0x3b, 0x2d, 0xc0, 0xea, 0x9e, 0x42, 0x6d, 0xcb, 0xc5, 0xff, 0x36, 0x3b, 0x87, 0xce, 0x83,
  0xd5, 0xea, 0xc1, 0x1f, 0x96, 0xa3, 0xf8, 0xba, 0x91, 0x06, 0xbf, 0x06, 0xd1, 0x79, 0x6f, 0x14, 0x27, 0x3e, 0x4b, 0x1a,
  0x50, 0xb2, 0x42, 0x2e, 0x5b, 0xa6, 0xe3, 0x24, 0x0e, 0xc3, 0xc6, 0x88, 0x4d, 0xbd, 0xcb, 0x20, 0x4e, 0x7a, 0xe9, 0x0c,
  0x10, 0x39, 0x5d, 0x8d, 0x62, 0xff, 0x66, 0x39, 0xf3, 0x92, 0xf3, 0x20, 0xea, 0xb5, 0xfa, 0x23, 0x6f, 0x7c, 0x71, 0x9e,
  0xc4, 0x0b, 0x98, 0xfb, 0xa5, 0x97, 0xd4, 0x11, 0xc9, 0x4e, 0x9f, 0x56, 0x41, 0x3c, 0x03, 0x1a, 0x9d, 0xfe, 0x04, 0xd8,
  0xa2, 0xd7, 0xde, 0x9b, 0x5f, 0xef, 0xb4, 0x9b, 0xdd, 0x3d, 0x2b, 0xbd, 0x49, 0x33, 0x36, 0x6b, 0x2c, 0x02, 0xb7, 0xe1,
  0xcd, 0xe7, 0x21, 0x6b, 0xf0, 0x02, 0xf7, 0x39, 0x60, 0xf5, 0xe2, 0x27, 0x6f, 0x7c, 0x42, 0x8f, 0xaf, 0xa1, 0x91, 0x5b,
  0x3b, 0x61, 0xe7, 0x31, 0xb3, 0x7e, 0xfe, 0xb1, 0xe6, 0xa6, 0x5e, 0x94, 0x36, 0x52, 0x96, 0x04, 0x93, 0xd5, 0x83, 0xd1,
  0x22, 0xcb, 0xe2, 0x08, 0xb8, 0x75, 0xbe, 0xc8, 0xdc, 0x94, 0x85, 0x6c, 0x9c, 0x2d, 0x69, 0x90, 0x20, 0x9a, 0x42, 0x8d,
  0x6c, 0xc5, 0x2b, 0x2c, 0x67, 0x41, 0xd4, 0x98, 0x32, 0xe4, 0x45, 0x01, 0x0e, 0x90, 0x8e, 0xb3, 0x7a, 0xd0, 0x4c, 0xa7,
  0x2c, 0x0c, 0x61, 0x16, 0xd7, 0x9c, 0xed, 0x7b, 0xed, 0xf6, 0x21, 0xe0, 0xa9, 0x2f, 0xa6, 0xe5, 0x2d, 0xb2, 0xb8, 0x3f,
  0xf7, 0x7c, 0x1f, 0xd1, 0x42, 0x08, 0x44, 0xc2, 0xb3, 0xba, 0xdd, 0xf9, 0xf5, 0xaa, 0x39, 0xf3, 0xd2, 0x6c, 0xe9, 0x07,
  0xe9, 0x3c, 0xf4, 0x6e, 0x7a, 0x93, 0x90, 0x5d, 0xf7, 0x3d, 0x60, 0xf6, 0xa8, 0x11, 0x00, 0xc8, 0x69, 0x6f, 0x0c, 0xdc,
  0xcf, 0x92, 0xfe, 0xdf, 0x16, 0x69, 0x16, 0x4c, 0x6e, 0x1a, 0x42, 0x1e, 0xf4, 0xd2, 0xb9, 0x07, 0x22, 0x66, 0xc4, 0xb2,
  0x2b, 0xc6, 0xa2, 0xfe, 0x39, 0xd0, 0x2f, 0x92, 0xbc, 0x18, 0x0f, 0x50, 0x0e, 0xc0, 0xce, 0x88, 0xba, 0x01, 0x36, 0xa2,
  0x9c, 0x3b, 0x47, 0xa0, 0x3e, 0x3a, 0x08, 0x50, 0x18, 0x9f, 0xc7, 0x4b, 0x3e, 0x0d, 0x64, 0xa1, 0xbe, 0x98, 0x2f, 0xfd,
  0x16, 0x6b, 0x2a, 0xd9, 0x67, 0x17, 0x8b, 0xf2, 0x15, 0x43, 0x22, 0xf6, 0x92, 0xc6, 0x39, 0xbe, 0x86, 0x5e, 0xeb, 0xed,
  0xee, 0x9e, 0xcf, 0xce, 0x5d, 0xb1, 0x90, 0x08, 0x86, 0xe3, 0x7e, 0x77, 0x30, 0xd9, 0xdb, 0xf7, 0x9f, 0x3a, 0x7d, 0x09,
  0xd0, 0x79, 0x12, 0xf8, 0x7d, 0xf8, 0x05, 0xf3, 0x31, 0x00, 0xe2, 0x8b, 0x8e, 0xa2, 0x80, 0x96, 0xbb, 0x71, 0xc5, 0xc1,
  0x78, 0xda, 0x02, 0x22, 0x41, 0x22, 0x93, 0x04, 0x89, 0x98, 0x24, 0x9c, 0x12, 0x3d, 0x76, 0xda, 0xee, 0xd3, 0xae, 0xdb,
  0xd9, 0x7d, 0x0a, 0x34, 0xb9, 0xe7, 0xc8, 0xd9, 0x5b, 0xd3, 0x36, 0x2d, 0x27, 0x92, 0x26, 0xa3, 0x15, 0xe8, 0x23, 0xac,
  0x72, 0x29, 0xdb, 0xcd, 0xb6, 0x5c, 0xaa, 0xd6, 0x4a, 0xb4, 0x48, 0x67, 0x1e, 0xac, 0xa8, 0x04, 0x72, 0x84, 0x52, 0xd6,
  0xa0, 0x43, 0x62, 0x60, 0x47, 0x62, 0x3c, 0x8b, 0xe7, 0xbd, 0x5d, 0x44, 0x1e, 0x2c, 0x50, 0x04, 0xc4, 0x13, 0x00, 0xad,
  0x6c, 0x83, 0x71, 0x94, 0x3f, 0x15, 0xbd, 0xe6, 0xb0, 0xb6, 0xa9, 0x57, 0x1f, 0x24, 0x0e, 0x5f, 0x91, 0xa7, 0xf9, 0x82,
  0x3c, 0x2d, 0xad, 0xc7, 0x5e, 0xeb, 0x7b, 0x7d, 0x39, 0x04, 0xf7, 0x52, 0xf3, 0x26, 0xca, 0x87, 0xa5, 0xfe, 0xb2, 0xdd,
  0x19, 0x1d, 0xec, 0x7b, 0xfc, 0x25, 0xf0, 0xbe, 0xf1, 0x6e, 0xd2, 0xea, 0x76, 0x77, 0x0f, 0x01, 0x7b, 0x40, 0xfc, 0xf1,
  0x72, 0xd3, 0x0a, 0xef, 0xd2, 0x0a, 0x0b, 0x95, 0x61, 0xb5, 0xbe, 0x87, 0x9f, 0x7e, 0xe7, 0xe9, 0xee, 0x53, 0x10, 0x11,
  0xf0, 0x7b, 0xb7, 0xdb, 0x6d, 0xed, 0x75, 0xad, 0x76, 0xab, 0xf5, 0xbd, 0xa3, 0x2f, 0xa8, 0x09, 0x76, 0x07, 0xf8, 0x20,
  0x67, 0x8f, 0x2e, 0x4d, 0x4b, 0x2d, 0x30, 0xc7, 0x0b, 0x7f, 0x28, 0x10, 0x0d, 0xfe, 0xd3, 0x00, 0x8c, 0x42, 0x49, 0xc6,
  0x50, 0x54, 0x2f, 0x66, 0x11, 0x10, 0x25, 0x2c, 0xfc, 0x24, 0xb1, 0xda, 0x13, 0x8e, 0xe1, 0x0e, 0x92, 0x2d, 0xea, 0xd9,
  0x49, 0x08, 0xbd, 0x4d, 0x03, 0xdf, 0x07, 0x86, 0x99, 0xc7, 0x69, 0x80, 0x6b, 0xd4, 0x4b, 0x18, 0xb4, 0x0d, 0x2e, 0x99,
  0x98, 0x6a, 0xcf, 0x9b, 0xc0, 0xca, 0x2c, 0x25, 0x97, 0xd5, 0x6a, 0x79, 0x4d, 0x6f, 0x94, 0xc2, 0x00, 0x19, 0xeb, 0xf3,
  0x75, 0xe8, 0xec, 0xb5, 0xf2, 0x95, 0xe0, 0x0f, 0x1b, 0xd7, 0x02, 0x4b, 0xc1, 0x0c, 0x50, 0x88, 0x1b, 0x07, 0xc9, 0x38,
  0x64, 0x2e, 0x57, 0x4f, 0xbb, 0x1d, 0xb7, 0x7d, 0xb0, 0xeb, 0x76, 0xf6, 0xf6, 0xdc, 0xe6, 0xae, 0xe3, 0x66, 0x40, 0x80,
  0xc0, 0xe0, 0x09, 0xd4, 0xb3, 0xf6, 0xf7, 0x01, 0x73, 0x09, 0x8d, 0xd2, 0x38, 0xc0, 0x51, 0x90, 0xd2, 0x1a, 0xed, 0xd6,
  0x1e, 0xa2, 0x2c, 0x0e, 0x90, 0x90, 0x1a, 0xec, 0x12, 0x6a, 0xa6, 0xbd, 0x28, 0x8e, 0xd8, 0xaa, 0xc9, 0x6e, 0xd8, 0x28,
  0x89, 0xaf, 0x34, 0x72, 0x47, 0xae, 0xee, 0x67, 0xec, 0x3a, 0x6b, 0x50, 0xc7, 0x93, 0x38, 0x99, 0xf5, 0x16, 0xf3, 0x39,
  0x4b, 0xc6, 0x5e, 0xca, 0xfa, 0x21, 0xcb, 0xb0, 0x0f, 0x14, 0x28, 0x88, 0xfe, 0x66, 0xbb, 0xcd, 0x66, 0x72, 0xa9, 0xc6,
  0x07, 0xfe, 0xc1, 0x84, 0x19, 0xec, 0x77, 0xd8, 0x42, 0x0e, 0x09, 0xce, 0xb3, 0x60, 0xc6, 0xb8, 0x84, 0x3c, 0x68, 0xb5,
  0xac, 0x71, 0xe8, 0xcd, 0xe6, 0xf5, 0x2e, 0xc0, 0xe7, 0x1e, 0x5c, 0x5e, 0xb9, 0x07, 0x30, 0xa2, 0xb3, 0xd3, 0xb6, 0x16,
  0x41, 0x63, 0x16, 0x47, 0x31, 0x09, 0x2b, 0xf7, 0xe4, 0xf5, 0x4f, 0xf0, 0xbb, 0xf1, 0x81, 0x9d, 0x2f, 0x42, 0x2f, 0x71,
  0x7f, 0x62, 0x51, 0x18, 0xbb, 0xea, 0x75, 0x11, 0x8e, 0x46, 0xb3, 0xb5, 0x0f, 0x80, 0x08, 0xae, 0x3c, 0x00, 0xf6, 0x26,
  0x4e, 0x07, 0x72, 0x85, 0xb5, 0x5e, 0x0a, 0xf8, 0xfc, 0x96, 0xbf, 0xe7, 0xfb, 0x2b, 0x5a, 0x3a, 0x98, 0xab, 0xcf, 0x96,
  0xdb, 0xd0, 0x87, 0x46, 0x1c, 0x6d, 0xc4, 0x28, 0x67, 0x4d, 0xb9, 0xe6, 0x82, 0x39, 0x7f, 0x05, 0x45, 0xe3, 0xb3, 0xeb,
  0x5e, 0x5b, 0xf6, 0x9e, 0x79, 0xd9, 0x92, 0xaf, 0x70, 0xaf, 0x0d, 0xd0, 0x00, 0x31, 0x04, 0xbe, 0x10, 0x39, 0xb0, 0x6c,
  0xf2, 0xff, 0xcd, 0x76, 0xd7, 0x31, 0x96, 0xbd, 0x54, 0xa1, 0x75, 0xe0, 0x28, 0x5a, 0x47, 0xe6, 0xb6, 0xda, 0xdd, 0xb2,
  0x5c, 0xdd, 0x13, 0x72, 0xd5, 0x4f, 0xe2, 0x39, 0x58, 0x8a, 0x21, 0x40, 0x04, 0x22, 0x68, 0x91, 0xd4, 0x01, 0x05, 0x8e,
  0x06, 0x91, 0x35, 0x2a, 0x48, 0x28, 0x6d, 0xd5, 0xbb, 0xb9, 0x3e, 0x40, 0x9a, 0x21, 0x5d, 0x93, 0x37, 0x04, 0x3c, 0x47,
  0x12, 0x8d, 0xa3, 0xa7, 0xb0, 0xd0, 0x13, 0xbd, 0x6d, 0xfb, 0x3e, 0x14, 0x43, 0x0b, 0xa5, 0xd3, 0x08, 0x50, 0x04, 0xf0,
  0xd3, 0xdf, 0x17, 0xc1, 0xf8, 0x62, 0x9b, 0x05, 0x49, 0xd8, 0x9c, 0x79, 0x59, 0xbd, 0xeb, 0xc2, 0xaa, 0x38, 0x4a, 0x0f,
  0xc9, 0xa5, 0xc7, 0x79, 0xc0, 0xda, 0x93, 0x2e, 0xe3, 0x7d, 0x36, 0xc6, 0x5e, 0x62, 0x88, 0x2b, 0x2e, 0x22, 0xc8, 0xe0,
  0x72, 0xfa, 0xa5, 0x35, 0xe2, 0x6f, 0x51, 0x70, 0x39, 0x45, 0x34, 0xef, 0x6b, 0x72, 0x87, 0x70, 0x8e, 0xea, 0x9d, 0xf3,
  0x77, 0x4b, 0x0e, 0x16, 0x7a, 0x23, 0x16, 0x2e, 0x37, 0xcb, 0x68, 0x04, 0xb7, 0x80, 0x80, 0x7b, 0xa0, 0x6f, 0x8f, 0xcd,
  0xe4, 0x60, 0x97, 0x5e, 0xb8, 0xe0, 0x6c, 0xa5, 0xba, 0xda, 0x6b, 0xe9, 0xab, 0x88, 0x10, 0x5f, 0x4d, 0x41, 0x8d, 0x50,
  0x7b, 0x06, 0x3c, 0x7f, 0x95, 0x78, 0xf3, 0x92, 0x7c, 0xa3, 0xc1, 0x55, 0x21, 0x98, 0x26, 0xc1, 0x3c, 0x0d, 0x52, 0xb0,
  0x35, 0xc0, 0x1d, 0x29, 0x4a, 0x86, 0x3b, 0x95, 0xda, 0x57, 0x0c, 0xf8, 0xa0, 0x19, 0x79, 0x97, 0x4b, 0x25, 0x43, 0xc1,
  0x86, 0x19, 0x5f, 0xdc, 0x90, 0x08, 0x43, 0xc5, 0xa7, 0x78, 0xcc, 0x30, 0xfb, 0xb8, 0xc9, 0x3d, 0x0b, 0xae, 0xeb, 0x41,
  0x64, 0xa5, 0xc0, 0x40, 0xae, 0xbe, 0xbe, 0xd6, 0x53, 0xd0, 0x31, 0x9a, 0x80, 0x74, 0xaa, 0xf9, 0x04, 0x97, 0xf6, 0x7e,
  0xb4, 0xb0, 0xa7, 0xd1, 0x02, 0xfe, 0xde, 0x9e, 0x70, 0xf7, 0x72, 0xc2, 0x2d, 0xe8, 0x2e, 0x61, 0x9c, 0x74, 0x2b, 0xfc,
  0x90, 0x3d, 0xa7, 0x68, 0xac, 0xed, 0x23, 0x81, 0x03, 0xba, 0x46, 0x59, 0x24, 0x05, 0x8d, 0x81, 0x17, 0x6d, 0xd2, 0x55,
  0x6b, 0x56, 0x98, 0x4e, 0x5b, 0x9b, 0x0e, 0x18, 0x0a, 0x08, 0x48, 0x89, 0x44, 0xc7, 0x8b, 0x24, 0x85, 0x6e, 0x84, 0x1a,
  0x91, 0x83, 0x37, 0xbd, 0x31, 0x6a, 0xc4, 0x32, 0x83, 0x91, 0xe5, 0xad, 0x8f, 0xcc, 0x57, 0x04, 0x16, 0x19, 0x9d, 0x5a,
  0xc5, 0xe9, 0x5c, 0x0d, 0x61, 0x91, 0xec, 0xc9, 0x10, 0x51, 0xab, 0x26, 0x22, 0xf3, 0x1e, 0x72, 0xa1, 0xdd, 0x71, 0x81,
  0x6c, 0xc1, 0xb4, 0x06, 0x5f, 0x03, 0x11, 0x2d, 0x44, 0x04, 0xc9, 0x33, 0x92, 0x04, 0xd4, 0x9a, 0x37, 0x42, 0xbb, 0x38,
  0xb2, 0xf6, 0xfb, 0xdf, 0x40, 0x38, 0xf0, 0x37, 0xfc, 0xc1, 0x31, 0x8c, 0x77, 0x73, 0x85, 0xa5, 0x08, 0x2f, 0xad, 0xf0,
  0x2e, 0x98, 0x9f, 0x04, 0x60, 0xf3, 0x0a, 0x35, 0x93, 0x0e, 0x65, 0x7b, 0xa7, 0xd1, 0x16, 0xef, 0xb2, 0x69, 0x50, 0x35,
  0x85, 0x2e, 0x7f, 0x6d, 0x4d, 0x3b, 0x3a, 0xbb, 0xe6, 0x26, 0x3e, 0xca, 0x27, 0xac, 0x80, 0xd1, 0x8a, 0xf5, 0x86, 0x26,
  0x16, 0xa0, 0xb8, 0x4f, 0xb2, 0x6d, 0x1c, 0x88, 0x4e, 0xd9, 0x81, 0x20, 0x9a, 0x4c, 0x17, 0xa3, 0xe5, 0x9d, 0x36, 0xaa,
  0x84, 0x8b, 0x4b, 0x6b, 0x80, 0x2e, 0xe5, 0x56, 0x30, 0xc5, 0x25, 0xbe, 0xd8, 0x18, 0x69, 0x1d, 0x2a, 0x63, 0xa4, 0x3c,
  0x76, 0x6e, 0x91, 0xc8, 0xc1, 0xf7, 0xb9, 0xaa, 0x68, 0x91, 0xdb, 0x83, 0x6a, 0x6e, 0x91, 0x36, 0xe6, 0x81, 0x66, 0xc6,
  0x07, 0x11, 0x99, 0xfd, 0x9b, 0x2c, 0xf2, 0x03, 0x8d, 0x71, 0xb0, 0xbf, 0x76, 0xd9, 0xb4, 0x7b, 0xfa, 0xf4, 0xa9, 0xe9,
  0xf7, 0x68, 0x34, 0xd6, 0x31, 0x59, 0x04, 0x9c, 0xe8, 0xcd, 0x9a, 0x02, 0xc4, 0x7b, 0x59, 0xbc, 0xae, 0x74, 0xe0, 0x4b,
  0x96, 0x3b, 0xef, 0x99, 0xfb, 0xfb, 0xe6, 0x68, 0x58, 0xe6, 0x98, 0x8d, 0xd1, 0xed, 0x2f, 0x37, 0xe6, 0xc1, 0x00, 0xb3,
  0x31, 0x96, 0x15, 0x1a, 0x17, 0xdc, 0x02, 0xe1, 0xc7, 0x79, 0xa5, 0x71, 0xa1, 0xc8, 0x6c, 0xd9, 0x1b, 0x31, 0x58, 0x59,
  0xa6, 0x1b, 0xd2, 0x5c, 0xad, 0x1e, 0xe4, 0x46, 0xf3, 0xc1, 0x5d, 0x26, 0x33, 0x88, 0x27, 0x14, 0x76, 0x2f, 0x70, 0xa0,
  0xd5, 0x03, 0xae, 0x87, 0xd7, 0x5a, 0x3b, 0x9d, 0x0d, 0x8e, 0x54, 0x59, 0x95, 0x0a, 0xfa, 0x26, 0xf2, 0x9e, 0x04, 0x2c,
  0xf4, 0x97, 0x05, 0xc2, 0x47, 0xaf, 0xcb, 0x08, 0x06, 0x08, 0xb7, 0x1e, 0x1c, 0x99, 0xbe, 0x16, 0x03, 0xe8, 0xe6, 0xb6,
  0xdb, 0x76, 0x5a, 0xa6, 0xbd, 0x96, 0x70, 0x2a, 0xa2, 0x1c, 0xba, 0xfc, 0xa6, 0x96, 0xf1, 0x22, 0xa3, 0x38, 0x11, 0x89,
  0x58, 0x02, 0xaf, 0x37, 0x89, 0xc7, 0x8b, 0x54, 0x00, 0xc9, 0x1f, 0x84, 0xf2, 0x68, 0x18, 0x0b, 0x44, 0xbe, 0xb7, 0x29,
  0xb8, 0xf0, 0xbf, 0x28, 0xbc, 0xd6, 0xe9, 0x5b, 0xde, 0x06, 0x0c, 0x2f, 0x53, 0xdf, 0x02, 0x67, 0x79, 0xc4, 0xd9, 0xa9,
  0x29, 0x77, 0x90, 0x77, 0x90, 0x2d, 0x48, 0xe6, 0x20, 0x11, 0xf7, 0xc8, 0x50, 0xd0, 0x4c, 0x09, 0x2e, 0xb2, 0x35, 0xf5,
  0x96, 0x63, 0x4c, 0xd7, 0x6d, 0x1b, 0x14, 0x59, 0xbb, 0x25, 0xed, 0xe5, 0xe2, 0xba, 0x9a, 0xaa, 0x6c, 0x3b, 0xde, 0x44,
  0x1c, 0x13, 0x3c, 0xbd, 0x29, 0x5a, 0x2e, 0x4b, 0x69, 0x3f, 0x90, 0xb7, 0x15, 0xb1, 0x34, 0xad, 0x37, 0x9f, 0x1e, 0xf2,
  0x1a, 0xcd, 0x79, 0x12, 0xc0, 0x4c, 0x6e, 0x2a, 0xb8, 0x81, 0xc2, 0x72, 0xba, 0x73, 0xcb, 0x1b, 0xa4, 0xf1, 0x24, 0xab,
  0x5a, 0x09, 0x41, 0x15, 0xd5, 0x24, 0xc0, 0x9b, 0xfa, 0x5e, 0x74, 0x0e, 0xe0, 0x6c, 0xcf, 0x78, 0x38, 0x05, 0x58, 0x0b,
  0x6f, 0x14, 0x32, 0x7f, 0x19, 0xa3, 0xec, 0xcc, 0x6e, 0x7a, 0xcd, 0x3d, 0x89, 0x94, 0x28, 0xce, 0x1a, 0x5e, 0x08, 0x76,
  0x19, 0xf3, 0xf9, 0x00, 0xc4, 0x3c, 0x1a, 0x45, 0xa3, 0xb0, 0x64, 0xe7, 0x33, 0x94, 0x81, 0x05, 0x65, 0xb2, 0x16, 0x8f,
  0x1b, 0x69, 0x5e, 0x2e, 0x57, 0x85, 0x63, 0x43, 0x01, 0x26, 0x18, 0x6c, 0x89, 0xdd, 0xf7, 0xda, 0xfd, 0xaf, 0x36, 0x74,
  0x9e, 0x6a, 0xe4, 0x71, 0x28, 0x2c, 0x78, 0xc1, 0x9c, 0xbb, 0x87, 0x77, 0x12, 0x0a, 0xc1, 0xb2, 0xd6, 0xe0, 0x59, 0xc7,
  0x97, 0x06, 0x1b, 0x21, 0x0a, 0xba, 0x95, 0x81, 0x66, 0x40, 0x6b, 0x16, 0x9f, 0x9f, 0x87, 0xac, 0x81, 0xfe, 0xf8, 0x37,
  0x8a, 0xf0, 0x6d, 0x16, 0x36, 0xeb, 0x96, 0xeb, 0x4e, 0x87, 0x93, 0x58, 0x53, 0x40, 0x3b, 0x8e, 0xe7, 0x37, 0x45, 0xc7,
  0xd2, 0x7c, 0x49, 0xce, 0xe3, 0x9d, 0xa1, 0xb1, 0x82, 0x74, 0xd6, 0x24, 0x01, 0xa7, 0x82, 0xab, 0x20, 0x1b, 0x4f, 0x97,
  0xa5, 0x50, 0x4c, 0xbf, 0xa0, 0xac, 0x79, 0xff, 0x22, 0x1e, 0x79, 0xa8, 0x05, 0x5d, 0x0e, 0x85, 0xb4, 0x21, 0x59, 0x86,
  0x21, 0x56, 0xd9, 0xa7, 0x45, 0xa2, 0x71, 0x59, 0x0e, 0xdd, 0x04, 0x51, 0xca, 0x32, 0x20, 0x36, 0xe5, 0x78, 0xf4, 0x25,
  0xb7, 0xb4, 0xfa, 0x9a, 0x80, 0x97, 0x51, 0xc1, 0x82, 0xb0, 0x57, 0x66, 0x46, 0x4b, 0xe1, 0xb3, 0x82, 0x9e, 0x60, 0x4d,
  0x80, 0x79, 0xd7, 0x8f, 0x5d, 0x8e, 0xcf, 0x95, 0xac, 0x0b, 0x54, 0x65, 0xa6, 0x3c, 0x23, 0x96, 0xe0, 0x1d, 0x36, 0xdb,
  0x87, 0xa9, 0x1c, 0xa6, 0x42, 0xcd, 0xae, 0x8d, 0x57, 0x69, 0x91, 0x5c, 0xfa, 0x1d, 0xb2, 0x49, 0x46, 0xde, 0x9d, 0xf4,
  0xf2, 0x8c, 0xf8, 0x5f, 0x29, 0x46, 0x47, 0xba, 0x79, 0x2d, 0xdd, 0xcb, 0x5d, 0x00, 0xf0, 0x6a, 0xca, 0xa0, 0x6a, 0x6b,
  0xc2, 0x35, 0x54, 0xe3, 0x32, 0x48, 0x03, 0x10, 0x56, 0x4f, 0x24, 0xb6, 0xa4, 0x5e, 0xeb, 0x14, 0xa8, 0x5a, 0xa8, 0x2d,
  0xf1, 0x1a, 0x8d, 0x29, 0xc4, 0xe1, 0x6e, 0x4e, 0x3d, 0xa2, 0x57, 0x29, 0xfc, 0xf2, 0x0e, 0xa5, 0x10, 0xdc, 0xaf, 0x14,
  0x82, 0x46, 0xe3, 0xf1, 0x94, 0x8d, 0x2f, 0xb4, 0xb6, 0xeb, 0x44, 0xfc, 0xe6, 0x66, 0x72, 0x29, 0x72, 0xab, 0x96, 0x7e,
  0xa1, 0x43, 0xf3, 0x97, 0x3a, 0x7a, 0x0d, 0x28, 0x0e, 0x2e, 0xe6, 0xba, 0xf7, 0xdd, 0x29, 0xca, 0x27, 0xb4, 0x64, 0xcb,
  0x61, 0xb0, 0x5d, 0x0c, 0x0f, 0x10, 0x4b, 0x55, 0x58, 0xe1, 0xf0, 0x26, 0x8e, 0x62, 0xde, 0xe9, 0xc4, 0x9b, 0x05, 0xe1,
  0x4d, 0xef, 0x1e, 0x41, 0xb7, 0x55, 0xd3, 0x0f, 0x2e, 0x69, 0xd2, 0x92, 0xe4, 0xab, 0xcc, 0x13, 0x2e, 0x5b, 0x64, 0x38,
  0x86, 0x6c, 0x6c, 0xd4, 0x18, 0x80, 0x00, 0x7f, 0x01, 0x12, 0x21, 0x0c, 0xd2, 0xcc, 0x05, 0x2c, 0x00, 0x82, 0xf1, 0x67,
  0xc1, 0xb3, 0xe3, 0x56, 0xc1, 0x4a, 0x55, 0xdf, 0xc6, 0xf1, 0x13, 0xee, 0x5e, 0x1b, 0xe3, 0x9d, 0x6e, 0xbb, 0xd9, 0x05,
  0xa7, 0xcf, 0x12, 0x51, 0x3b, 0xe2, 0x73, 0x65, 0x6b, 0xe8, 0x62, 0x94, 0x45, 0xfe, 0xbd, 0x2c, 0xb1, 0x5d, 0xdd, 0xae,
  0xe8, 0x6c, 0xb0, 0xe7, 0x73, 0xd8, 0x2d, 0x32, 0x5e, 0x0a, 0xe6, 0xdf, 0xaa, 0x09, 0xe0, 0x67, 0x37, 0xba, 0x4d, 0xe3,
  0x7b, 0x29, 0x34, 0xd8, 0x76, 0x70, 0x0a, 0x78, 0x93, 0x47, 0x44, 0xd3, 0x31, 0xb7, 0x3f, 0x8c, 0xb5, 0x7e, 0x40, 0x58,
  0x5e, 0xde, 0x67, 0x96, 0x7a, 0x64, 0xbd, 0xb8, 0x5b, 0x53, 0x9c, 0x25, 0xae, 0x20, 0x08, 0x83, 0xad, 0x62, 0xa8, 0xdc,
  0x12, 0x6b, 0xee, 0x6e, 0xb1, 0x2a, 0xa2, 0x67, 0x60, 0x40, 0x76, 0xbf, 0xd0, 0x13, 0x45, 0xfc, 0x54, 0x5b, 0xee, 0x06,
  0x55, 0x79, 0x3b, 0xaa, 0x06, 0xf9, 0x3a, 0x65, 0x97, 0xe6, 0x01, 0xbc, 0x00, 0xbe, 0x6d, 0xd0, 0x5e, 0xc6, 0x16, 0x93,
  0xc3, 0xc9, 0xe4, 0x11, 0xe2, 0xfd, 0x79, 0x95, 0xba, 0x5e, 0x89, 0x3e, 0x03, 0x10, 0xbd, 0xc2, 0x92, 0xda, 0xef, 0xe6,
  0x12, 0x76, 0xbf, 0x42, 0xc5, 0x1e, 0x6e, 0xb5, 0x57, 0x26, 0xb6, 0x4f, 0x5c, 0xb1, 0xa9, 0x6d, 0xec, 0x9a, 0xdc, 0xb5,
  0x63, 0xa6, 0x89, 0x96, 0x3d, 0xc4, 0x1d, 0x07, 0x91, 0xbb, 0x65, 0xc5, 0xa0, 0xa2, 0x7c, 0x4b, 0xe1, 0xf4, 0xfb, 0x46,
  0x04, 0x69, 0xb7, 0xc4, 0x0b, 0xb3, 0xe9, 0x3d, 0xc2, 0x38, 0xbb, 0x79, 0x94, 0xec, 0x29, 0x0f, 0x45, 0x63, 0x07, 0xa0,
  0x52, 0x96, 0x5f, 0x66, 0x64, 0x6e, 0x62, 0xe7, 0x5d, 0x63, 0x00, 0x2b, 0xcd, 0x92, 0x38, 0x3a, 0x5f, 0xeb, 0x3b, 0xa2,
  0x74, 0x36, 0xaa, 0xa3, 0x75, 0x53, 0x08, 0x86, 0x57, 0x71, 0xe3, 0x1c, 0xc4, 0xbd, 0xee, 0xbb, 0xc3, 0x33, 0x39, 0x3e,
  0xfd, 0x2b, 0x00, 0x0d, 0xb4, 0x06, 0xf3, 0x2e, 0x7a, 0xf4, 0x6f, 0x03, 0x0b, 0x0c, 0xfd, 0xda, 0x1a, 0xb7, 0xdb, 0x6d,
  0xbf, 0x6f, 0x6c, 0x63, 0x6c, 0x9c, 0xd1, 0x9e, 0x69, 0xda, 0x76, 0x3a, 0xda, 0xee, 0x72, 0x4b, 0x6c, 0x87, 0x77, 0x68,
  0x3b, 0x7c, 0x7f, 0xef, 0x5e, 0xdb, 0x2e, 0x2a, 0x66, 0xcb, 0x0d, 0xa8, 0xb9, 0x17, 0x24, 0xcc, 0x6f, 0xf8, 0x2c, 0xf3,
  0x82, 0x30, 0x95, 0xdb, 0xf2, 0x87, 0x3c, 0xa8, 0xa3, 0x2a, 0x73, 0xff, 0xce, 0x8b, 0x6e, 0xae, 0x80, 0xa7, 0x58, 0xb1,
  0x95, 0x05, 0x4a, 0xe5, 0x09, 0xfc, 0x7f, 0x59, 0xdc, 0x7d, 0xf0, 0xfc, 0x4b, 0x2f, 0x1a, 0x43, 0x45, 0x92, 0x05, 0xa6,
  0x58, 0x2a, 0xb9, 0x09, 0xfd, 0xed, 0x83, 0x16, 0xc5, 0xed, 0x52, 0xb4, 0xbb, 0x71, 0x53, 0x5d, 0xd9, 0x42, 0x93, 0xe0,
  0x9a, 0xf9, 0xdc, 0xe2, 0xe1, 0x76, 0x0c, 0x39, 0xf9, 0xa8, 0x63, 0x10, 0x8f, 0x2e, 0x8b, 0x2e, 0xeb, 0xa9, 0x37, 0x61,
  0x0d, 0xf0, 0x39, 0xbc, 0x06, 0x19, 0x6a, 0x22, 0x10, 0xe0, 0x08, 0x7b, 0xc6, 0x54, 0xe8, 0xf5, 0x06, 0xf4, 0xe2, 0xf2,
  0x10, 0x73, 0x6e, 0x40, 0x56, 0xec, 0xc6, 0x29, 0x43, 0x93, 0x22, 0x0f, 0x32, 0x37, 0x00, 0xd6, 0xb1, 0xfe, 0xb4, 0x73,
  0x79, 0xe5, 0xee, 0xe1, 0x86, 0x99, 0x63, 0x68, 0x21, 0x4b, 0x33, 0xf2, 0xd7, 0x61, 0x43, 0xe6, 0xe7, 0x18, 0xfb, 0xa9,
  0xba, 0x65, 0x86, 0xfd, 0xec, 0xee, 0x15, 0x6d, 0xb3, 0xae, 0x69, 0x9b, 0x75, 0xd2, 0x15, 0x47, 0x53, 0x33, 0x9d, 0x82,
  0x7b, 0x22, 0xe7, 0xd1, 0x5e, 0x3f, 0xe1, 0x96, 0x23, 0x5b, 0x14, 0xb7, 0x8b, 0x0f, 0xbc, 0xce, 0x41, 0xdb, 0x93, 0x6f,
  0x4b, 0x3b, 0xcd, 0xad, 0xbd, 0xee, 0x64, 0xb7, 0x0d, 0xcb, 0xc2, 0x2d, 0xfa, 0xea, 0xb8, 0x41, 0x95, 0x8c, 0x3d, 0xa7,
  0x8d, 0xcc, 0x90, 0x6f, 0xc3, 0xa1, 0x1c, 0x03, 0xc8, 0xab, 0x8c, 0x8c, 0x3b, 0xf7, 0xf9, 0xb8, 0x2a, 0x11, 0x1d, 0x8c,
  0xa7, 0xc1, 0x7c, 0xf9, 0x65, 0x7e, 0xd4, 0x9d, 0xb1, 0x89, 0x4e, 0x71, 0x20, 0x70, 0xa4, 0x8a, 0x9b, 0xf9, 0xc6, 0xeb,
  0x7b, 0xbb, 0x52, 0xed, 0xb2, 0x2b, 0x25, 0x93, 0x96, 0x72, 0x12, 0x3b, 0x24, 0x8b, 0x73, 0xc9, 0xb7, 0xef, 0xd7, 0xa2,
  0x47, 0xec, 0x3a, 0x2d, 0x37, 0xe2, 0x8f, 0xc7, 0x99, 0xdd, 0x75, 0xd1, 0x6a, 0x1e, 0xca, 0x56, 0xd6, 0xdd, 0x1d, 0x5d,
  0xe5, 0x96, 0x94, 0x40, 0x42, 0x55, 0x5f, 0xd2, 0x0e, 0xb9, 0xab, 0x2f, 0x51, 0xcf, 0xe2, 0xbf, 0xe6, 0x49, 0x9c, 0xc5,
  0x50, 0xa9, 0xaa, 0x47, 0x9d, 0x72, 0x36, 0x60, 0x43, 0x6c, 0xbe, 0x98, 0xda, 0x50, 0x16, 0x9b, 0x2b, 0x45, 0x11, 0xb8,
  0x0a, 0xc4, 0xef, 0x09, 0xc4, 0xf3, 0x7c, 0x20, 0x93, 0xb3, 0x89, 0x2d, 0xf3, 0x94, 0x1f, 0x21, 0x5b, 0xd1, 0x23, 0xef,
  0x28, 0xd2, 0xc9, 0xd3, 0x47, 0xac, 0x26, 0xda, 0x84, 0x85, 0x5d, 0x15, 0x5a, 0x4f, 0xd5, 0x6b, 0x39, 0x01, 0x44, 0xaa,
  0x32, 0xb9, 0x1d, 0x7e, 0x07, 0x0a, 0xc5, 0xf2, 0x4b, 0x06, 0xd1, 0x76, 0x57, 0x75, 0xc8, 0x09, 0x03, 0x4b, 0xb9, 0xfb,
  0xa8, 0x74, 0xc0, 0x35, 0x4f, 0x6b, 0xd2, 0x99, 0x39, 0x47, 0x21, 0xf1, 0x2c, 0xe8, 0x8c, 0x7d, 0xea, 0x98, 0x76, 0x7c,
  0xe4, 0x7e, 0xbb, 0xd8, 0xb5, 0x51, 0x23, 0xec, 0xeb, 0xee, 0x81, 0xbb, 0xc5, 0xf2, 0x57, 0x90, 0x91, 0xbb, 0x15, 0x35,
  0x88, 0x18, 0x81, 0x61, 0xcd, 0x6b, 0x51, 0x30, 0x69, 0xd3, 0x6c, 0x18, 0x58, 0x33, 0x24, 0xab, 0x6b, 0x51, 0x62, 0x9c,
  0x56, 0x55, 0xb3, 0x0f, 0xf7, 0x34, 0x0f, 0x7c, 0xaf, 0x53, 0xb9, 0xe7, 0x6f, 0x66, 0x56, 0x70, 0x1a, 0xc4, 0xbc, 0x44,
  0xa0, 0xb4, 0xa3, 0x1d, 0x91, 0xd9, 0x79, 0xb4, 0x23, 0x32, 0x54, 0x31, 0x6f, 0x0e, 0xfe, 0x80, 0xa2, 0xc5, 0xf4, 0x8b,
  0x34, 0x1d, 0xd4, 0x88, 0xe8, 0x64, 0x0e, 0x2b, 0x4b, 0x64, 0x31, 0x52, 0x1b, 0x94, 0x5a, 0x7a, 0x55, 0xf2, 0x69, 0x6b,
  0x43, 0xbd, 0x08, 0xd3, 0xbe, 0x6a, 0xc3, 0x0f, 0x2f, 0x8e, 0x76, 0xa0, 0x90, 0xde, 0x0c, 0x8f, 0xa6, 0x6d, 0x23, 0xc9,
  0x14, 0x1e, 0x8f, 0x28, 0x2f, 0x6a, 0xf8, 0x3e, 0x61, 0x63, 0x70, 0xe2, 0x81, 0x4c, 0x11, 0x5c, 0x1e, 0xbf, 0x9d, 0x05,
  0xe8, 0xba, 0x5a, 0xff, 0xf9, 0xff, 0xac, 0x23, 0xda, 0xc3, 0x0a, 0xfc, 0x41, 0x6d, 0x72, 0x55, 0x1b, 0xfe, 0x79, 0xb7,
  0xb9, 0x0b, 0xe0, 0x43, 0xd1, 0x10, 0xfe, 0x50, 0x73, 0x31, 0x06, 0xfd, 0x6b, 0x02, 0x96, 0x33, 0x00, 0x40, 0xa7, 0xba,
  0x09, 0x83, 0x4b, 0xf6, 0x32, 0xc6, 0x54, 0x5b, 0x5e, 0xcb, 0x87, 0xdf, 0x43, 0xd9, 0x27, 0xd5, 0x12, 0x2f, 0x90, 0x67,
  0x6a, 0xaa, 0xc9, 0x29, 0x3e, 0x0d, 0x5f, 0x88, 0x2e, 0xa3, 0xf3, 0xdf, 0x7f, 0xfb, 0x3f, 0x0a, 0x10, 0x1a, 0x9a, 0x23,
  0x93, 0x25, 0xc3, 0x07, 0x0f, 0x8e, 0xc4, 0x86, 0x95, 0xec, 0x09, 0x17, 0xb9, 0x66, 0x79, 0x49, 0xe0, 0xf1, 0x14, 0x80,
  0x41, 0xed, 0x05, 0xdf, 0x8f, 0x10, 0xd9, 0x86, 0x16, 0xb7, 0xab, 0x25, 0x62, 0x0d, 0x54, 0x8a, 0x1c, 0x9c, 0xda, 0xf0,
  0x4d, 0x3c, 0xf6, 0x42, 0x28, 0x25, 0xec, 0x15, 0x6b, 0x89, 0xb5, 0xe6, 0xe0, 0xd2, 0xaf, 0x61, 0xa3, 0xd1, 0xa3, 0xff,
  0x95, 0x2b, 0xa3, 0x9d, 0xce, 0x6b, 0xd2, 0x2f, 0xe8, 0xd9, 0x43, 0xf6, 0x21, 0xf4, 0xd3, 0xb4, 0xd6, 0x21, 0x54, 0x09,
  0x03, 0x84, 0xb4, 0xe2, 0x0d, 0xcc, 0x42, 0x60, 0x7a, 0x28, 0x56, 0x99, 0x23, 0x68, 0x44, 0x83, 0x61, 0x1d, 0x2a, 0x06,
  0x34, 0x62, 0xc0, 0x43, 0x22, 0x71, 0xa4, 0xc6, 0xda, 0xdc, 0x25, 0x51, 0x4f, 0xb9, 0xcb, 0x0f, 0x93, 0xda, 0xf0, 0x47,
  0x3f, 0x64, 0xdb, 0x77, 0xf4, 0x0b, 0xb2, 0x53, 0xb9, 0x23, 0x2a, 0xae, 0x0d, 0xdf, 0xc6, 0xb8, 0x2c, 0x68, 0x5b, 0xde,
  0xa3, 0xc7, 0xe0, 0xf7, 0xdf, 0xfe, 0xed, 0x75, 0x50, 0xd1, 0x67, 0x30, 0x09, 0xd6, 0x4e, 0x57, 0x11, 0x8e, 0x20, 0x97,
  0x0a, 0xca, 0x21, 0x31, 0x5a, 0x64, 0xb8, 0x5c, 0xb6, 0x9a, 0x5c, 0xa7, 0x25, 0x99, 0xd4, 0x86, 0xa7, 0x82, 0x8d, 0x68,
  0x55, 0xcb, 0x34, 0xa0, 0xe5, 0x88, 0x58, 0x68, 0xc1, 0x0b, 0xca, 0xb9, 0x3e, 0x15, 0xb4, 0x53, 0x6e, 0x81, 0xf9, 0x1e,
  0xb2, 0x16, 0xe0, 0x88, 0x21, 0xa6, 0x2c, 0x1e, 0xee, 0x96, 0x3c, 0x9b, 0x22, 0x17, 0xaf, 0x27, 0x9f, 0xad, 0xe0, 0xa6,
  0x55, 0xb6, 0xb2, 0x9b, 0xf9, 0x66, 0xa0, 0x25, 0x24, 0x27, 0x5c, 0x68, 0x43, 0xbb, 0xd7, 0x56, 0x3c, 0x99, 0x6c, 0x06,
  0xfb, 0x55, 0x34, 0x8e, 0x91, 0xd2, 0x6b, 0xc3, 0xdf, 0x7f, 0xfb, 0x8f, 0xaf, 0x84, 0xf3, 0x2d, 0xc8, 0x03, 0x4b, 0xca,
  0xff, 0x2d, 0x40, 0x8d, 0xa0, 0xfe, 0x89, 0xa8, 0xae, 0x8f, 0x5f, 0x09, 0xaa, 0x5e, 0xf9, 0x64, 0x31, 0x22, 0x54, 0x2b,
  0x5d, 0x13, 0x02, 0xbb, 0x22, 0x71, 0x7e, 0x15, 0xf8, 0x44, 0xec, 0x44, 0xe7, 0x5b, 0xc0, 0x4e, 0x05, 0x92, 0x3d, 0xc0,
  0x27, 0x49, 0x36, 0xc1, 0x9e, 0x57, 0x56, 0x90, 0x2f, 0xc6, 0x63, 0x96, 0xa6, 0x93, 0x45, 0x68, 0x5d, 0x25, 0x60, 0x95,
  0x5b, 0x37, 0x2c, 0x33, 0xc0, 0x37, 0x38, 0x00, 0xd4, 0xbe, 0xec, 0x14, 0x7e, 0x9a, 0x62, 0x33, 0x57, 0x20, 0x96, 0x68,
  0xc1, 0x85, 0x26, 0xcf, 0xa8, 0xd6, 0x9a, 0xa1, 0x91, 0xc5, 0x69, 0xb3, 0x66, 0x81, 0x90, 0xf3, 0x1a, 0x98, 0x43, 0x02,
  0x2c, 0x19, 0x23, 0x75, 0xbf, 0x83, 0x19, 0xe0, 0x33, 0xf0, 0x22, 0xb5, 0x5b, 0xd7, 0x83, 0xd1, 0x14, 0x55, 0x6b, 0x5c,
  0x93, 0x42, 0xe8, 0x1e, 0x0d, 0xaf, 0x38, 0xde, 0x84, 0xd0, 0xb9, 0x47, 0x43, 0x70, 0x1d, 0x51, 0xd1, 0xc0, 0x0c, 0x4f,
  0xc4, 0xaf, 0x7b, 0x35, 0x97, 0x1e, 0x72, 0x6d, 0xf8, 0x4c, 0xfc, 0xca, 0x9b, 0x1f, 0xed, 0x40, 0x03, 0x44, 0xf6, 0xcc,
  0x0b, 0xf0, 0xb1, 0x20, 0x75, 0xb0, 0x03, 0x85, 0x3e, 0x5c, 0x53, 0x2c, 0x68, 0x70, 0xe4, 0x99, 0xb4, 0x86, 0x96, 0x0b,
  0x57, 0x06, 0x5e, 0x92, 0x05, 0x63, 0xa0, 0x4e, 0xa9, 0x7b, 0x31, 0xe3, 0x84, 0x4c, 0x7c, 0x93, 0x08, 0x65, 0xa2, 0x49,
  0x4d, 0x5a, 0x05, 0x1d, 0xae, 0x2a, 0xf0, 0x74, 0xca, 0x04, 0x34, 0x0b, 0xc0, 0x09, 0x8a, 0xb4, 0x33, 0x3c, 0x9a, 0x2b,
  0x4b, 0x04, 0xa9, 0x08, 0xb8, 0x1b, 0x13, 0xa9, 0xaf, 0x14, 0xd5, 0x02, 0x59, 0x30, 0x8b, 0x7c, 0x1c, 0x06, 0xc5, 0x53,
  0x26, 0x8c, 0x87, 0x20, 0xb5, 0x22, 0x90, 0xdf, 0x59, 0xb2, 0x00, 0xbd, 0xea, 0x37, 0x8f, 0x76, 0xe6, 0xd2, 0x3e, 0xd0,
  0x35, 0xbb, 0x96, 0x5d, 0xc0, 0xe7, 0x47, 0x5a, 0xf5, 0x3d, 0x3e, 0x2a, 0x59, 0x6d, 0x28, 0x77, 0x7d, 0x06, 0x17, 0xf3,
  0x40, 0x6b, 0xf4, 0x2a, 0x49, 0xe2, 0x64, 0x1d, 0x3f, 0xa3, 0xc3, 0x55, 0x1b, 0xb2, 0x14, 0x40, 0x03, 0x45, 0xeb, 0x5b,
  0x0c, 0x2b, 0x97, 0xeb, 0x79, 0x92, 0x92, 0x0b, 0x8b, 0x8a, 0x44, 0x2c, 0x36, 0x8b, 0x85, 0x58, 0xc8, 0xe6, 0xff, 0x85,
  0x14, 0x82, 0x15, 0x47, 0xe3, 0x10, 0x7e, 0xc0, 0x54, 0x00, 0x17, 0x6f, 0xb3, 0x79, 0x1d, 0x50, 0x9d, 0x3a, 0x40, 0x29,
  0x88, 0x1a, 0xc2, 0x44, 0x14, 0xe7, 0xf4, 0x2d, 0x99, 0x4d, 0xac, 0xd1, 0x57, 0xae, 0x17, 0x97, 0xd0, 0xf1, 0x22, 0x9b,
  0x2f, 0xb2, 0xaa, 0xb5, 0x92, 0xb6, 0x8d, 0x6e, 0xc8, 0x21, 0xc6, 0x19, 0x2d, 0x21, 0x4f, 0x2f, 0x00, 0x64, 0x60, 0x58,
  0xc0, 0xcb, 0xb6, 0x5e, 0xa2, 0x64, 0xc2, 0xd7, 0x87, 0xab, 0xfa, 0xbb, 0xd6, 0x26, 0x99, 0xbc, 0x4e, 0xd8, 0xdf, 0x37,
  0xae, 0x8b, 0xa8, 0xf7, 0x53, 0x8c, 0xe6, 0xcc, 0x0b, 0x2f, 0x49, 0x02, 0x80, 0xb3, 0x52, 0x85, 0x6c, 0x5a, 0x1f, 0xdc,
  0x9b, 0xd7, 0x97, 0x63, 0x1a, 0x5f, 0xfd, 0x19, 0x98, 0xa5, 0x6e, 0x93, 0xbc, 0xb0, 0x61, 0x49, 0x7e, 0xf2, 0x22, 0xef,
  0x9c, 0x59, 0x89, 0x21, 0x38, 0xbe, 0xed, 0x8a, 0xbc, 0xf0, 0x40, 0xf1, 0x72, 0xe6, 0xa8, 0x5a, 0x90, 0x37, 0x60, 0xb4,
  0x5b, 0x3e, 0x43, 0x2b, 0x36, 0x01, 0xc4, 0x3f, 0x7f, 0xf3, 0x8a, 0x93, 0x08, 0x09, 0xe3, 0xad, 0x17, 0x80, 0xba, 0xe7,
  0x6b, 0x20, 0x74, 0xc0, 0x9a, 0x45, 0xd0, 0x43, 0xc5, 0xbc, 0x29, 0x4a, 0x0e, 0xae, 0x11, 0x84, 0x9d, 0x4b, 0x1d, 0x28,
  0x2b, 0xab, 0xba, 0x83, 0xdc, 0x4a, 0x55, 0xcd, 0x5f, 0x92, 0xb9, 0x5a, 0xb9, 0xa6, 0x5f, 0xb4, 0x46, 0x34, 0x90, 0xb6,
  0x46, 0x57, 0x9a, 0x52, 0xbc, 0xe7, 0x42, 0x5d, 0x91, 0x55, 0x7c, 0xc7, 0x3a, 0x3d, 0xcb, 0x2c, 0xcf, 0x3a, 0x0f, 0xbd,
  0x35, 0x42, 0xee, 0x74, 0xca, 0xac, 0x09, 0x48, 0x5e, 0x58, 0x74, 0x10, 0xf6, 0xd6, 0x55, 0x9c, 0x64, 0x53, 0x6b, 0x2c,
  0xe4, 0x91, 0xc5, 0x37, 0x05, 0xad, 0x9b, 0x78, 0x61, 0x85, 0xcc, 0x43, 0xa3, 0x6b, 0x6a, 0x3a, 0x4b, 0x5e, 0x08, 0x7e,
  0xbd, 0xbe, 0x9c, 0x25, 0x24, 0x71, 0xa7, 0xb4, 0x56, 0x51, 0x36, 0x8a, 0xaf, 0xd1, 0x96, 0xa5, 0xe0, 0xb6, 0x30, 0x5e,
  0xbd, 0xf9, 0x0f, 0x24, 0xf0, 0x09, 0xdd, 0xfc, 0x8d, 0xb0, 0x76, 0x27, 0x09, 0x63, 0x16, 0x56, 0x58, 0x4b, 0x02, 0x6b,
  0x3a, 0x45, 0x0b, 0x91, 0xf9, 0xeb, 0xba, 0xe5, 0x6f, 0x51, 0xb1, 0xc7, 0x20, 0x27, 0x46, 0x18, 0x32, 0x03, 0xdd, 0xcf,
  0xd2, 0xfb, 0x8e, 0x32, 0x49, 0xd7, 0x8d, 0xf0, 0x06, 0x10, 0x15, 0xb2, 0xd7, 0x27, 0xd6, 0x22, 0x45, 0xba, 0xd3, 0xbb,
  0x2d, 0x2d, 0xf3, 0x16, 0xc6, 0x39, 0x6a, 0x45, 0x4d, 0x3f, 0x0a, 0x0b, 0xe1, 0x1e, 0x0a, 0xf2, 0x4e, 0x82, 0x39, 0xd5,
  0xec, 0x6a, 0x30, 0xd3, 0xfd, 0x4a, 0xb2, 0x91, 0x86, 0xa2, 0x4f, 0x35, 0x50, 0x05, 0x62, 0x00, 0x1a, 0x58, 0x1e, 0xe8,
  0x05, 0xd4, 0x00, 0x48, 0xd9, 0x50, 0xa3, 0xec, 0x29, 0xe0, 0x23, 0xf8, 0x95, 0x8c, 0xe6, 0xad, 0x59, 0x1f, 0xfb, 0xe5,
  0x9c, 0xaf, 0xc6, 0x32, 0x7d, 0x5f, 0xcb, 0xf4, 0x8f, 0xf2, 0x54, 0x15, 0x73, 0x86, 0x5a, 0xde, 0x07, 0x32, 0xea, 0xf0,
  0xf5, 0x22, 0x0c, 0x1b, 0x24, 0x8a, 0x3e, 0xbc, 0x26, 0xc7, 0x88, 0xfa, 0x54, 0x9e, 0x0b, 0xa6, 0x22, 0x04, 0xd1, 0x22,
  0x5e, 0xa4, 0xe1, 0x0d, 0xe9, 0x8e, 0xe0, 0x3c, 0x42, 0x16, 0x90, 0x96, 0x70, 0xda, 0x34, 0xd7, 0x90, 0x8c, 0x44, 0x35,
  0x09, 0xda, 0x60, 0x87, 0x61, 0x68, 0x8b, 0x9d, 0x13, 0x06, 0x0c, 0x47, 0xfe, 0x0d, 0xb9, 0x17, 0x80, 0x70, 0x64, 0x2d,
  0x24, 0x1f, 0x94, 0x0d, 0x53, 0xcc, 0x94, 0x22, 0xa3, 0xeb, 0xb5, 0xa8, 0x46, 0x3a, 0xb5, 0x29, 0xf6, 0xe6, 0x9d, 0x5a,
  0x01, 0x47, 0xb4, 0x53, 0x9f, 0x07, 0x0e, 0x76, 0x68, 0xf0, 0x35, 0xf8, 0xa0, 0xb4, 0xc0, 0x9a, 0x45, 0x51, 0x17, 0x8c,
  0xa1, 0x18, 0x69, 0x6c, 0x35, 0x09, 0x38, 0x2c, 0x57, 0x0e, 0xa3, 0xf2, 0x6b, 0x72, 0x1c, 0x25, 0x9a, 0x67, 0x24, 0x46,
  0xe3, 0xca, 0xd4, 0x98, 0x9c, 0x6c, 0x58, 0x3d, 0x27, 0xf1, 0xb6, 0xee, 0x10, 0xe4, 0xd4, 0x7a, 0x23, 0xcc, 0x06, 0x6c,
  0xd9, 0xf5, 0x3b, 0xca, 0x9a, 0xa8, 0x99, 0x74, 0x49, 0xc0, 0xf1, 0x7c, 0x8a, 0x2a, 0xc0, 0x54, 0x2b, 0x13, 0x22, 0xbd,
  0x0b, 0x5e, 0xc1, 0x04, 0x4a, 0x8f, 0x45, 0x21, 0x95, 0x3f, 0x9b, 0xcf, 0xc3, 0x00, 0x68, 0x3c, 0x8b, 0x49, 0xf0, 0x29,
  0x33, 0x22, 0xa4, 0xb0, 0x08, 0xc1, 0x20, 0x24, 0x24, 0x13, 0x5e, 0x5e, 0xd3, 0x74, 0x91, 0xac, 0x3b, 0x85, 0xf8, 0x9d,
  0xec, 0xf8, 0x06, 0x7d, 0x5d, 0x24, 0xd5, 0x32, 0x13, 0xfe, 0x32, 0xf5, 0xb2, 0x92, 0x44, 0x06, 0x56, 0x64, 0xf8, 0x13,
  0xc5, 0x37, 0x65, 0x02, 0xa2, 0x55, 0xb6, 0x35, 0xd7, 0xa1, 0xbe, 0xfe, 0x20, 0xac, 0x9e, 0x77, 0x68, 0x97, 0xac, 0x11,
  0x83, 0x7a, 0x76, 0x74, 0x6d, 0x48, 0x3e, 0x2e, 0xe2, 0xa9, 0xd2, 0xed, 0x07, 0x03, 0x49, 0xb8, 0xfb, 0x82, 0x18, 0xb5,
  0xad, 0xcb, 0x0e, 0xd0, 0x22, 0x37, 0x8a, 0x90, 0xd0, 0x4e, 0x45, 0x28, 0xa0, 0x52, 0xdb, 0x8a, 0x54, 0x8f, 0xda, 0x9d,
  0xd0, 0xbc, 0x26, 0x23, 0x4f, 0xab, 0xa5, 0xfa, 0x17, 0x94, 0xf8, 0x92, 0xf6, 0xf9, 0x4c, 0x70, 0xf2, 0x9d, 0xe5, 0x35,
  0xc3, 0x23, 0xc6, 0xf3, 0x9e, 0xa4, 0x57, 0x2f, 0xba, 0xd2, 0x7d, 0xfb, 0x6f, 0xa2, 0xb5, 0x0d, 0x62, 0x57, 0xb2, 0xa7,
  0x8a, 0x08, 0x9e, 0xf9, 0x3e, 0x10, 0x39, 0x08, 0x2b, 0x24, 0x84, 0xab, 0x20, 0xf2, 0xe3, 0xab, 0x94, 0x14, 0x36, 0x58,
  0x27, 0x0b, 0x2f, 0x84, 0x17, 0xa0, 0x78, 0x9a, 0x16, 0xfa, 0x9f, 0xa1, 0x37, 0x4f, 0xad, 0x24, 0x26, 0xb3, 0x58, 0x24,
  0xe6, 0x69, 0x0c, 0x9e, 0xea, 0x44, 0x72, 0xa7, 0x41, 0xe3, 0xf9, 0xbe, 0x94, 0xcb, 0xc8, 0x43, 0x4f, 0x2c, 0x84, 0x83,
  0x0f, 0x5f, 0xb4, 0x63, 0xe4, 0x12, 0xa8, 0x69, 0xbc, 0x09, 0xd2, 0x3c, 0x14, 0x6a, 0xe4, 0xe1, 0x54, 0x2d, 0xee, 0x56,
  0x9e, 0x4a, 0x6e, 0x68, 0x81, 0x95, 0x22, 0x01, 0x4b, 0x11, 0xb2, 0x13, 0x34, 0x5b, 0x34, 0x0c, 0xfe, 0x43, 0x6c, 0xac,
  0x4b, 0x20, 0x02, 0xcc, 0xe0, 0x12, 0xe8, 0xe4, 0x6e, 0x46, 0xba, 0xc6, 0xde, 0x4a, 0x19, 0x39, 0x92, 0x5c, 0x88, 0x24,
  0x6c, 0xcc, 0xd0, 0x46, 0x6e, 0xb0, 0xd9, 0x22, 0x24, 0xea, 0x94, 0xad, 0x9b, 0xd6, 0x89, 0x94, 0x37, 0xc8, 0x57, 0xbf,
  0x82, 0xa1, 0xb5, 0xc3, 0x25, 0x1e, 0x29, 0xa9, 0x24, 0x0e, 0x53, 0x5a, 0xf3, 0x31, 0xf0, 0x85, 0x10, 0x46, 0x8c, 0xf3,
  0xe1, 0x46, 0x7b, 0x4c, 0xdf, 0x4c, 0xe2, 0x04, 0x2d, 0x4a, 0xf8, 0xb2, 0x7c, 0x13, 0xab, 0x44, 0x84, 0x1f, 0xbe, 0xd8,
  0x2a, 0xc9, 0xb7, 0x29, 0xaa, 0xca, 0x71, 0x4f, 0x02, 0x38, 0xee, 0x7f, 0xfc, 0x6f, 0x3d, 0xd2, 0xbf, 0x4e, 0x1a, 0x98,
  0x0a, 0x90, 0x52, 0x2d, 0x49, 0xff, 0x91, 0x7f, 0xc2, 0x2d, 0x94, 0x84, 0xa5, 0x8b, 0x30, 0xdb, 0xc6, 0xa7, 0xb8, 0xfa,
  0x72, 0x7f, 0xe2, 0xaa, 0xca, 0x97, 0x58, 0xb3, 0x44, 0x1b, 0x24, 0x5d, 0x85, 0x86, 0x1c, 0x65, 0xe0, 0x49, 0x00, 0xa7,
  0xbf, 0x4f, 0xe2, 0x49, 0x80, 0xf2, 0x8f, 0x87, 0xd9, 0xe6, 0xfc, 0xb1, 0x4a, 0x3b, 0x16, 0x5b, 0x98, 0x4a, 0xf2, 0xb9,
  0xf9, 0x96, 0x5b, 0x24, 0x14, 0x98, 0x2b, 0x2b, 0xf0, 0xed, 0x41, 0xa3, 0xdd, 0xac, 0xda, 0x50, 0xfe, 0xba, 0x03, 0x2c,
  0x5e, 0x7b, 0x2d, 0x5c, 0xf4, 0x7a, 0x6b, 0xc0, 0xb6, 0x0e, 0x75, 0x8c, 0x32, 0x0c, 0x65, 0x3c, 0xa7, 0x4a, 0x85, 0x70,
  0xc7, 0x73, 0x18, 0x25, 0xc3, 0xeb, 0x1e, 0x8c, 0xa0, 0x87, 0x32, 0x08, 0x38, 0x21, 0x19, 0xe1, 0x8f, 0xf2, 0x58, 0x3c,
  0x2f, 0x5e, 0xeb, 0x38, 0x61, 0x23, 0x10, 0x98, 0x79, 0xd7, 0xd0, 0xed, 0x0b, 0x70, 0xb3, 0x12, 0xf4, 0xb5, 0x92, 0x08,
  0xba, 0x05, 0x21, 0x0b, 0xc4, 0x79, 0x6f, 0xa1, 0x75, 0xa7, 0xbc, 0xfa, 0x21, 0xbe, 0xb2, 0x30, 0xe7, 0x05, 0x6d, 0x84,
  0xc7, 0xd6, 0x8c, 0xd0, 0xca, 0x23, 0x5c, 0xe0, 0xff, 0x5d, 0x54, 0x89, 0x2d, 0x05, 0x24, 0xd9, 0xc4, 0x6f, 0x5e, 0x4b,
  0xbd, 0xa1, 0xab, 0x29, 0x14, 0x6a, 0xb3, 0x85, 0x50, 0x3a, 0xec, 0x7a, 0x1c, 0x2e, 0x52, 0x90, 0x6c, 0x4d, 0x4b, 0x0b,
  0xa0, 0x66, 0x8b, 0x24, 0x4a, 0xad, 0xbc, 0x33, 0x90, 0x67, 0x14, 0x48, 0x23, 0x56, 0xf2, 0xaa, 0x3a, 0x05, 0x73, 0x86,
  0x07, 0x05, 0xb7, 0x36, 0x61, 0x46, 0x19, 0x0a, 0x32, 0x16, 0x09, 0x23, 0x06, 0xf0, 0xce, 0x60, 0x86, 0x9b, 0x9d, 0x07,
  0x23, 0xb3, 0xa7, 0x26, 0x37, 0x53, 0x78, 0x5a, 0x10, 0xb1, 0xd3, 0x8f, 0xd1, 0x04, 0x64, 0xd1, 0x7b, 0x2a, 0xe0, 0xab,
  0xdd, 0x53, 0x5e, 0xb2, 0xdc, 0x61, 0xd1, 0xfa, 0x33, 0x13, 0x8a, 0x6a, 0xc5, 0xde, 0x5e, 0xca, 0x72, 0x7e, 0xfa, 0x94,
  0x2f, 0xcc, 0x5b, 0x6f, 0xc6, 0x7a, 0xda, 0xd6, 0xa3, 0x56, 0x1f, 0x5f, 0x69, 0x66, 0xbf, 0x92, 0x7a, 0xcf, 0x38, 0x7d,
  0xac, 0x69, 0x25, 0xde, 0x2a, 0x4d, 0x4b, 0x36, 0x58, 0x45, 0x2f, 0x82, 0xd1, 0xd7, 0xf4, 0xa2, 0xc4, 0x4a, 0x65, 0x43,
  0xe2, 0xc4, 0xf5, 0x2d, 0x05, 0xd7, 0x57, 0xb9, 0xbd, 0x55, 0xa6, 0xd5, 0x28, 0x7b, 0x16, 0x5e, 0x79, 0x37, 0xe9, 0x2f,
  0x5e, 0x90, 0x49, 0x31, 0xcb, 0x4b, 0x2c, 0x2c, 0x2a, 0xa0, 0xdc, 0x30, 0xb4, 0xad, 0x2f, 0x32, 0x15, 0x71, 0x41, 0x81,
  0xee, 0xfe, 0xf8, 0xcb, 0xef, 0xbf, 0xfd, 0xdb, 0xf3, 0xbf, 0xec, 0xed, 0xb7, 0x5a, 0xd5, 0xd0, 0x0d, 0xdb, 0x4d, 0xeb,
  0xc7, 0xc8, 0x02, 0xf7, 0x51, 0x86, 0xc5, 0xad, 0xdf, 0xff, 0xdb, 0xbf, 0x5b, 0x42, 0xd2, 0x72, 0x4e, 0x42, 0x1a, 0x70,
  0x85, 0x48, 0xe0, 0xc6, 0x98, 0x2e, 0x87, 0x89, 0x6f, 0xa0, 0xbd, 0x36, 0x94, 0xf5, 0xd3, 0x8f, 0xef, 0xb1, 0x91, 0x25,
  0x37, 0xfb, 0x9b, 0x47, 0xa3, 0x64, 0xd8, 0x69, 0x5a, 0xef, 0x71, 0xd9, 0xb0, 0x32, 0x81, 0x27, 0x83, 0xf8, 0x43, 0x7a,
  0xbd, 0x0b, 0xa6, 0x5c, 0x94, 0x77, 0xef, 0x5a, 0xd3, 0x38, 0xa4, 0x8e, 0x5f, 0x50, 0x57, 0xe8, 0x7e, 0x83, 0x53, 0x00,
  0x02, 0x04, 0xd4, 0xdb, 0xae, 0x88, 0x6b, 0xa4, 0xd6, 0x02, 0x26, 0x1c, 0x52, 0xab, 0x9c, 0xf7, 0xd2, 0x9b, 0xd9, 0x28,
  0x0e, 0x25, 0x5c, 0x2f, 0xde, 0xbd, 0x7d, 0xfb, 0xea, 0xc5, 0xa9, 0xf5, 0xcb, 0x8f, 0xa7, 0x3f, 0x58, 0xcf, 0xac, 0xf7,
  0x3f, 0xbc, 0x7b, 0xfb, 0x8a, 0xf7, 0x08, 0x58, 0x98, 0xd2, 0xd0, 0x5d, 0x83, 0x95, 0x49, 0x46, 0xa5, 0xda, 0x44, 0xc9,
  0x08, 0xa5, 0xfb, 0x27, 0xb8, 0x59, 0x92, 0x67, 0x82, 0xe0, 0x10, 0x79, 0xf0, 0x8f, 0x6f, 0xcd, 0x90, 0x95, 0xf2, 0x55,
  0x6b, 0x77, 0x0a, 0xde, 0xcd, 0x39, 0x89, 0x0d, 0x4d, 0x80, 0xad, 0x5f, 0xbe, 0x93, 0x7c, 0x61, 0xe6, 0x1a, 0x1b, 0x1b,
  0xeb, 0x33, 0x97, 0x88, 0x5f, 0x2f, 0xdd, 0x87, 0x72, 0x91, 0x60, 0x15, 0x0c, 0xc2, 0xb1, 0x82, 0xc8, 0x42, 0xef, 0xe5,
  0x82, 0xb1, 0x39, 0xca, 0x55, 0x8c, 0xf1, 0xba, 0x79, 0x8f, 0x2f, 0x09, 0x99, 0x20, 0xfa, 0x19, 0x7a, 0x93, 0x09, 0xfb,
  0xfb, 0x02, 0xa3, 0x25, 0xc1, 0x8c, 0x12, 0x70, 0x00, 0x29, 0x84, 0x9f, 0x71, 0x9c, 0x24, 0x7c, 0x8e, 0x72, 0xad, 0x7f,
  0x9c, 0xc0, 0x28, 0xd0, 0xc9, 0x65, 0x10, 0x2f, 0x52, 0x00, 0x24, 0xc8, 0x84, 0x91, 0x4d, 0x82, 0x31, 0x4c, 0x40, 0xae,
  0xdf, 0x58, 0xf1, 0x9c, 0x45, 0x62, 0x28, 0x1c, 0x99, 0x80, 0xf7, 0xce, 0x3d, 0x00, 0x08, 0x8a, 0xf0, 0xf4, 0x6c, 0x6a,
  0x41, 0xbb, 0x20, 0x02, 0xa9, 0xe8, 0x81, 0xb3, 0x30, 0x81, 0x62, 0x14, 0xc4, 0x58, 0xd7, 0xb3, 0xce, 0x41, 0xfa, 0x26,
  0xc1, 0xd8, 0x1a, 0x2d, 0xd2, 0x1b, 0xbe, 0x59, 0xf0, 0xb5, 0x0b, 0x23, 0x38, 0x96, 0x80, 0xe5, 0xf1, 0xa3, 0xb5, 0x0c,
  0xef, 0x29, 0x76, 0xff, 0x81, 0x85, 0x73, 0xf4, 0x68, 0xc1, 0x25, 0x61, 0x11, 0x1d, 0x86, 0x70, 0x75, 0x6a, 0x33, 0xc2,
  0x32, 0x21, 0xc9, 0xf7, 0x94, 0x88, 0x9d, 0xb4, 0x96, 0x40, 0x27, 0x94, 0x24, 0xf1, 0xcc, 0x74, 0xd5, 0x8f, 0x82, 0x21,
  0x5f, 0xee, 0xa3, 0x9d, 0x60, 0xc8, 0x57, 0x12, 0xb0, 0x0a, 0x4e, 0x31, 0xa9, 0x1c, 0x41, 0xc4, 0x20, 0xbb, 0x16, 0xd1,
  0x05, 0x2c, 0x70, 0x24, 0xd6, 0x3a, 0x40, 0x16, 0x03, 0x0c, 0x23, 0xf8, 0x4d, 0x8b, 0xeb, 0x13, 0x42, 0xd7, 0x22, 0x8b,
  0xc1, 0x36, 0x0f, 0xc6, 0xa4, 0xe3, 0xd2, 0x2c, 0x9e, 0x73, 0x28, 0xb0, 0xb3, 0x1b, 0xe5, 0x65, 0xf8, 0xe0, 0xaf, 0x17,
  0x54, 0x23, 0x10, 0x17, 0x9a, 0x99, 0x33, 0x96, 0x72, 0x16, 0xb9, 0x02, 0xa5, 0x9c, 0xde, 0x1f, 0xd1, 0xf7, 0x08, 0xa6,
  0x82, 0x2c, 0xcd, 0x30, 0xbd, 0x27, 0x4b, 0xab, 0xa2, 0x92, 0x39, 0x77, 0xde, 0x3b, 0xd4, 0x09, 0x1d, 0x8f, 0x2f, 0x2a,
  0x3b, 0xf5, 0xf8, 0xc6, 0x18, 0x32, 0xf9, 0x17, 0xf4, 0x4a, 0xbb, 0x5a, 0x95, 0xfd, 0x02, 0xe6, 0xe6, 0x00, 0x28, 0xe3,
  0xe4, 0x99, 0xde, 0x11, 0x44, 0xfd, 0x1a, 0x3f, 0x4e, 0x2e, 0xaf, 0x11, 0x95, 0x2f, 0x5b, 0x43, 0xaf, 0xe3, 0x45, 0x02,
  0xe6, 0x5c, 0x10, 0x22, 0xef, 0xe1, 0x6d, 0x78, 0x0b, 0xbc, 0x25, 0x0f, 0x3c, 0x68, 0xeb, 0x45, 0x1c, 0x4d, 0xc0, 0xb8,
  0xe3, 0xd1, 0x17, 0xb2, 0x66, 0xa4, 0x4b, 0xae, 0xef, 0x1d, 0x1a, 0x64, 0xb4, 0xd9, 0x5b, 0x93, 0xc7, 0x52, 0x94, 0x85,
  0x2a, 0x68, 0xec, 0xdb, 0x39, 0x6b, 0xf9, 0x96, 0xef, 0x37, 0x8d, 0x22, 0xa3, 0x3c, 0x7b, 0xac, 0x3c, 0xd7, 0xca, 0x3d,
  0x3b, 0xdd, 0x8b, 0x15, 0x69, 0x78, 0xc2, 0xd9, 0xe5, 0x2a, 0xa3, 0xd2, 0xb9, 0xb5, 0x46, 0xa0, 0x65, 0xc2, 0x50, 0xb8,
  0xcf, 0xdc, 0x6b, 0x06, 0x0e, 0x94, 0x4e, 0xf3, 0x26, 0x64, 0x56, 0x85, 0x17, 0x05, 0x7c, 0x35, 0x0e, 0x30, 0x07, 0xb5,
  0x22, 0x9e, 0x28, 0xab, 0x15, 0xe2, 0x89, 0xa2, 0x98, 0xa2, 0x88, 0xf1, 0x9c, 0xf0, 0xfd, 0x6c, 0x01, 0xb4, 0xeb, 0x85,
  0x81, 0xb7, 0xf3, 0x3c, 0x09, 0xd2, 0x91, 0x87, 0x1d, 0x8a, 0x57, 0xe5, 0x2a, 0x27, 0x37, 0x7e, 0xc4, 0x6e, 0xca, 0x15,
  0x52, 0x78, 0x77, 0x1a, 0x5f, 0xdc, 0xc4, 0xd5, 0xaf, 0x4e, 0x10, 0x84, 0xa9, 0x17, 0x94, 0xde, 0xbe, 0x5a, 0x24, 0xa0,
  0x0c, 0x76, 0xde, 0x80, 0xd2, 0xc7, 0xa4, 0x98, 0x62, 0xdb, 0x19, 0x4a, 0x7a, 0x6f, 0xe7, 0x2d, 0xbb, 0x3a, 0xfb, 0x2b,
  0x19, 0xfa, 0x6b, 0x2a, 0xbc, 0x89, 0xd3, 0xb3, 0x67, 0x30, 0x49, 0x0a, 0x98, 0x14, 0xea, 0xfc, 0x7c, 0xfa, 0x22, 0x2f,
  0xfb, 0x4a, 0xa7, 0x6b, 0xd3, 0x8e, 0x32, 0xed, 0x19, 0x00, 0xcf, 0xbf, 0x3d, 0x7d, 0xff, 0xcd, 0xfd, 0x1f, 0x91, 0x3a,
  0x65, 0xcd, 0xe3, 0x2b, 0xdc, 0x53, 0xac, 0x20, 0xcd, 0x69, 0x1c, 0xa7, 0x79, 0x90, 0x0c, 0x8f, 0xdd, 0x25, 0x28, 0xd4,
  0xa3, 0x18, 0x40, 0xda, 0xa1, 0x66, 0x18, 0x56, 0xe3, 0x46, 0x03, 0x03, 0xca, 0x8b, 0x70, 0xf3, 0xf9, 0x8a, 0x8d, 0xc0,
  0x59, 0xc1, 0xb4, 0x93, 0xcd, 0x3c, 0x2d, 0xcf, 0x2a, 0xe7, 0xc8, 0xa1, 0x08, 0x41, 0x30, 0x09, 0xb8, 0xea, 0xac, 0x69,
  0x35, 0x75, 0x24, 0xb1, 0x0c, 0x53, 0xbb, 0xde, 0xe3, 0xe8, 0x68, 0x5d, 0xd4, 0x5b, 0x8e, 0x52, 0xb6, 0xb8, 0xda, 0x05,
  0xcf, 0x53, 0xf6, 0xa9, 0xb6, 0x4f, 0xb6, 0xed, 0xb6, 0x0d, 0xdd, 0xd2, 0x53, 0x03, 0x83, 0x69, 0x55, 0xd1, 0xbc, 0xf2,
  0x96, 0xcb, 0xfa, 0x7d, 0x86, 0xf5, 0x5b, 0x31, 0x0a, 0xc2, 0x7f, 0x05, 0x93, 0xe9, 0x5d, 0xf4, 0x86, 0x27, 0x09, 0xbd,
  0xe4, 0x67, 0x21, 0xad, 0x5f, 0x82, 0x06, 0xac, 0x50, 0x1a, 0xc2, 0x3b, 0x6b, 0xc7, 0xc2, 0x2a, 0xa2, 0xc8, 0xd3, 0x66,
  0x2c, 0x14, 0x04, 0xbd, 0xcd, 0xb1, 0x6f, 0x79, 0x2a, 0x1e, 0x27, 0x57, 0x90, 0x3b, 0xb7, 0x5c, 0x14, 0xdf, 0x73, 0x07,
  0x27, 0x87, 0xb0, 0xbc, 0x87, 0x93, 0xe7, 0x09, 0x01, 0x7e, 0x47, 0x37, 0xe5, 0xe9, 0x98, 0xf2, 0xc2, 0xc4, 0xb3, 0xbe,
  0xd5, 0x73, 0xdc, 0xea, 0xb5, 0xef, 0xb5, 0xdd, 0x63, 0x12, 0xac, 0x02, 0x94, 0x7a, 0x27, 0x5b, 0xaa, 0x6a, 0x45, 0xda,
  0xb8, 0x22, 0xf9, 0xd2, 0x5a, 0x68, 0xab, 0x82, 0x6b, 0x25, 0xb8, 0x21, 0xc7, 0x1a, 0xb7, 0xe2, 0x47, 0xe0, 0x26, 0x10,
  0x85, 0x8b, 0x3d, 0x0e, 0x1d, 0x87, 0x2e, 0x4a, 0xe7, 0x48, 0xf8, 0xf3, 0x28, 0xa8, 0x65, 0x27, 0x5c, 0xe9, 0xa1, 0x5f,
  0x2f, 0x51, 0x0f, 0x6f, 0x67, 0x82, 0x1f, 0xbe, 0x9a, 0x6f, 0x4d, 0x7f, 0xab, 0xcc, 0xb7, 0x3f, 0xa7, 0x8c, 0x1b, 0x6e,
  0x68, 0x94, 0x21, 0x58, 0x93, 0x20, 0x49, 0x33, 0xdd, 0x17, 0x01, 0xe3, 0xdb, 0xb3, 0xb4, 0xdc, 0x05, 0xd7, 0xc2, 0x9a,
  0x31, 0xa5, 0x2a, 0x8c, 0x58, 0x02, 0xa6, 0x38, 0x28, 0xf5, 0x39, 0x77, 0x0b, 0x13, 0x46, 0x87, 0xd4, 0x66, 0xc4, 0xdb,
  0x64, 0x39, 0x6e, 0x1b, 0x83, 0xc0, 0x0e, 0x78, 0xfc, 0xe1, 0x03, 0x5a, 0xe8, 0x6b, 0x8d, 0xa1, 0xca, 0x98, 0x19, 0x7a,
  0x7d, 0xf7, 0x09, 0xe6, 0xe9, 0xf5, 0xff, 0x81, 0xa1, 0x3c, 0x31, 0xcc, 0x56, 0x81, 0x3c, 0xa3, 0xee, 0x3f, 0x3f, 0x8c,
  0x87, 0xc3, 0x4b, 0x47, 0xbd, 0x14, 0xce, 0xc3, 0xb5, 0x21, 0xac, 0x4a, 0x6d, 0x63, 0x78, 0xd9, 0x6b, 0xa3, 0x77, 0x77,
  0x27, 0x74, 0x88, 0xf4, 0xc6, 0x62, 0xde, 0xdc, 0x3f, 0x46, 0x6e, 0xea, 0xa1, 0x12, 0x21, 0x39, 0x75, 0xbf, 0x0b, 0xe9,
  0x5f, 0x4b, 0x5c, 0x93, 0x0e, 0x52, 0x2e, 0x32, 0x5f, 0x94, 0x1d, 0x2a, 0xee, 0xc1, 0x4b, 0xce, 0x51, 0x1e, 0x94, 0xee,
  0x2d, 0xeb, 0xbe, 0xd0, 0xdc, 0x03, 0x0d, 0x98, 0x5a, 0xfe, 0x82, 0xc2, 0x88, 0xdc, 0x03, 0xda, 0xda, 0xef, 0xb9, 0x8f,
  0x00, 0xd6, 0xa7, 0xba, 0x85, 0x08, 0x2e, 0x63, 0xa6, 0x48, 0x80, 0xf9, 0xfb, 0x2f, 0xdf, 0x6e, 0x5f, 0x1f, 0x3d, 0xac,
  0x5a, 0xd2, 0x0e, 0x09, 0xde, 0xea, 0xb0, 0x8e, 0x10, 0xa5, 0x38, 0x5b, 0x03, 0xef, 0x9a, 0x90, 0x02, 0x7f, 0x14, 0x44,
  0x54, 0x90, 0xe9, 0xa1, 0x53, 0x21, 0x05, 0x77, 0x34, 0xd9, 0xc6, 0xbd, 0x55, 0x3d, 0xdc, 0x0a, 0xfe, 0x5f, 0x9c, 0x00,
  0x86, 0x30, 0x32, 0x03, 0xaa, 0x11, 0x9c, 0x41, 0x10, 0x6b, 0x11, 0xbb, 0xb2, 0x66, 0xd8, 0x2d, 0xae, 0x1b, 0xf5, 0xdf,
  0xe7, 0x23, 0x83, 0x4a, 0x00, 0x61, 0x0f, 0x02, 0x1e, 0x5f, 0x80, 0x10, 0x15, 0xf2, 0xcf, 0xd7, 0x43, 0x3b, 0x9e, 0x9e,
  0x59, 0xbb, 0x36, 0xc6, 0xd3, 0xfc, 0xa6, 0x6e, 0x99, 0xd4, 0x2d, 0x11, 0x68, 0x94, 0x35, 0xf1, 0xe9, 0x37, 0x94, 0x9d,
  0x34, 0x87, 0x02, 0x3c, 0xab, 0x0a, 0x8e, 0x96, 0x17, 0x5d, 0xa0, 0x58, 0x47, 0x0d, 0xc7, 0x1d, 0x09, 0x9e, 0x20, 0x88,
  0x5e, 0x3b, 0x4d, 0x53, 0x56, 0xdd, 0x3e, 0x15, 0x0d, 0xb5, 0xeb, 0x76, 0xd9, 0x9a, 0xe4, 0x39, 0x49, 0x32, 0x90, 0x57,
  0x93, 0xd5, 0x86, 0xeb, 0xb3, 0x33, 0xca, 0x37, 0xc0, 0x99, 0xc2, 0x17, 0x98, 0xc9, 0xc7, 0x0d, 0x27, 0x9a, 0x3e, 0xdd,
  0xb3, 0x6e, 0xd5, 0x4f, 0x4e, 0x7e, 0x7c, 0xe9, 0x28, 0x9a, 0xcc, 0xb9, 0x85, 0xea, 0xc2, 0xfa, 0x5e, 0x87, 0x2c, 0x3a,
  0xcf, 0xa6, 0x83, 0xda, 0xfe, 0x6e, 0x8d, 0x1c, 0xce, 0x71, 0x3c, 0x9b, 0x87, 0x2c, 0x83, 0xe1, 0x40, 0x3b, 0xaf, 0xdf,
  0x5b, 0xda, 0x16, 0x26, 0x89, 0x40, 0x94, 0x9f, 0xfc, 0x57, 0x05, 0x30, 0xaa, 0x92, 0x60, 0xdb, 0xfc, 0x79, 0x23, 0x80,
  0x40, 0xa1, 0x8d, 0xbc, 0x2a, 0x11, 0x21, 0x86, 0x39, 0x19, 0x0c, 0x4b, 0xa6, 0x9e, 0x58, 0x4c, 0x2b, 0x87, 0x61, 0x9d,
  0xad, 0x7d, 0xff, 0xcd, 0x63, 0xb4, 0xd1, 0xd4, 0xbe, 0xb1, 0xa4, 0xbc, 0x75, 0x42, 0xfd, 0xab, 0x1c, 0xef, 0x3c, 0x59,
  0xfa, 0x3e, 0xf9, 0xcd, 0x5b, 0x71, 0xcc, 0xcb, 0xc0, 0x3b, 0x8f, 0x62, 0xbc, 0xae, 0x32, 0x5d, 0x63, 0x26, 0x51, 0x62,
  0x3c, 0xca, 0x1d, 0x95, 0xeb, 0x87, 0xb2, 0x3b, 0x98, 0xc1, 0x2f, 0xd7, 0x9a, 0xb1, 0x59, 0x9c, 0xdc, 0xb8, 0x94, 0xb5,
  0x29, 0x72, 0x38, 0x79, 0x32, 0x14, 0xa5, 0x1f, 0x53, 0xc8, 0x19, 0x8f, 0xd1, 0xdd, 0x2b, 0x61, 0x60, 0x31, 0xc7, 0xcd,
  0x50, 0x0d, 0xb0, 0x7a, 0x96, 0x90, 0xca, 0xff, 0xc0, 0x26, 0x20, 0x18, 0xa7, 0x45, 0xd4, 0xce, 0xc1, 0xe2, 0xa4, 0xe3,
  0x42, 0x79, 0x8b, 0xda, 0xf0, 0xdd, 0x9c, 0x89, 0xc8, 0xdc, 0x1c, 0x73, 0x27, 0x81, 0xc1, 0xf1, 0x60, 0x82, 0xa5, 0xd5,
  0xa1, 0x9d, 0x01, 0x68, 0xfb, 0x45, 0xb2, 0xa7, 0xb8, 0x0d, 0x24, 0xa2, 0x50, 0x27, 0x99, 0x87, 0x69, 0x81, 0x3e, 0xe5,
  0x79, 0xb3, 0x9e, 0x8a, 0x4e, 0x09, 0x53, 0x33, 0x48, 0x66, 0x57, 0x18, 0xdd, 0xc1, 0x88, 0x2c, 0xba, 0x8a, 0x0d, 0xee,
  0x1f, 0x9a, 0xfb, 0xfb, 0x2a, 0x52, 0xd1, 0xb4, 0xfe, 0xbc, 0xdb, 0x6c, 0xef, 0xe0, 0xa1, 0x32, 0x14, 0xeb, 0x40, 0xc6,
  0x40, 0x21, 0x14, 0x4e, 0xe4, 0xd6, 0xf8, 0x9f, 0xfe, 0xf4, 0xd7, 0xdf, 0x7f, 0xfb, 0x5f, 0xe0, 0xdf, 0x24, 0x20, 0x86,
  0x81, 0x3c, 0xad, 0x49, 0x82, 0xfc, 0x9e, 0x81, 0x3f, 0xbc, 0x38, 0x9f, 0x92, 0x2c, 0x4b, 0x18, 0x66, 0x1f, 0xe5, 0x39,
  0x3f, 0x54, 0xbf, 0xd7, 0xde, 0xdb, 0xe9, 0x75, 0xf7, 0x2c, 0x8c, 0x29, 0x35, 0xd2, 0xe0, 0x3c, 0x42, 0xc7, 0x34, 0x88,
  0x7d, 0x30, 0xd5, 0x71, 0xe5, 0xb8, 0x7e, 0xd6, 0xd3, 0x13, 0xe0, 0x0f, 0xde, 0x27, 0x43, 0xc9, 0x22, 0x49, 0xae, 0x76,
  0x8a, 0xb9, 0x06, 0x60, 0x12, 0x4f, 0x29, 0xa4, 0x0f, 0x22, 0x00, 0x84, 0x3d, 0x48, 0x7e, 0xae, 0x4f, 0x40, 0x39, 0x24,
  0xb1, 0xbf, 0x18, 0xd3, 0x21, 0xae, 0x29, 0x25, 0x63, 0x05, 0xe3, 0x00, 0xc3, 0xf0, 0x3c, 0x9d, 0x80, 0x80, 0xa2, 0xf0,
  0x4d, 0x43, 0xc4, 0xb6, 0xa3, 0x4b, 0x3e, 0xd9, 0xe6, 0x36, 0xcc, 0x73, 0xb4, 0x23, 0x8e, 0x09, 0x88, 0x37, 0x86, 0x39,
  0xe4, 0xc9, 0x78, 0x98, 0xf8, 0x99, 0xc4, 0x28, 0xb1, 0xe4, 0x4e, 0x3d, 0xb7, 0x08, 0x00, 0xe9, 0x20, 0x6f, 0xe2, 0x30,
  0xa0, 0x95, 0x14, 0xbd, 0xa4, 0xe3, 0x24, 0x98, 0x67, 0xc3, 0x07, 0x00, 0x0c, 0xb8, 0x05, 0x8f, 0x06, 0xd0, 0xc7, 0xd0,
  0x8f, 0xc7, 0x0b, 0x34, 0xf1, 0x9b, 0xe7, 0x2c, 0x7b, 0x15, 0x92, 0xb5, 0xff, 0xfc, 0xe6, 0x47, 0xbf, 0x1e, 0xf8, 0x4e,
  0xff, 0x01, 0xc8, 0x23, 0x39, 0xa1, 0x74, 0xf0, 0xf1, 0x93, 0xab, 0x12, 0x49, 0xf0, 0x81, 0x8e, 0x0f, 0x9c, 0x0f, 0x96,
  0x2b, 0x17, 0x00, 0x13, 0x1b, 0x56, 0xf2, 0x09, 0x09, 0x1d, 0x7f, 0x13, 0x84, 0x18, 0x27, 0x4a, 0x06, 0x2d, 0x17, 0xe9,
  0xf4, 0x59, 0x06, 0x3f, 0xf0, 0x1e, 0xd9, 0x67, 0xb4, 0xad, 0x39, 0x98, 0x78, 0x61, 0xca, 0x5c, 0x29, 0x60, 0x3e, 0xb0,
  0x4b, 0x3a, 0x96, 0x38, 0x68, 0xf5, 0x05, 0x90, 0x40, 0xf1, 0xa8, 0x8f, 0xa5, 0x39, 0x3b, 0x40, 0xed, 0x7d, 0x82, 0x89,
  0x6b, 0xaa, 0x82, 0x30, 0xa4, 0x01, 0xa4, 0x65, 0xe0, 0xf7, 0x5a, 0x2e, 0xea, 0x87, 0x9e, 0xfd, 0xc7, 0x5f, 0x1a, 0xf9,
  0x36, 0x93, 0xbd, 0x72, 0xf1, 0x5d, 0x5b, 0xbc, 0x93, 0x04, 0x0d, 0x8c, 0x73, 0x1e, 0x64, 0xb0, 0x66, 0x3b, 0xd6, 0xf4,
  0x66, 0x04, 0x62, 0x47, 0xd4, 0xeb, 0x88, 0x7a, 0xcf, 0x80, 0x30, 0xe3, 0xf3, 0x05, 0x83, 0xf7, 0x3f, 0x9d, 0x36, 0xfe,
  0x68, 0xd5, 0xd9, 0x35, 0x12, 0x15, 0xa2, 0xc8, 0x0b, 0x1d, 0x7b, 0xf5, 0x49, 0xc2, 0x70, 0xa5, 0xed, 0x14, 0x02, 0x1c,
  0x2d, 0xb7, 0xed, 0x76, 0xdc, 0xdd, 0x4f, 0xcd, 0x99, 0x37, 0xaf, 0x23, 0x8e, 0xeb, 0xd0, 0x2b, 0xef, 0xf3, 0x33, 0xb7,
  0x92, 0x1f, 0x41, 0xc1, 0x93, 0xf6, 0xea, 0xf3, 0xca, 0x81, 0x79, 0x4c, 0x16, 0x11, 0x97, 0x93, 0x2c, 0x1d, 0xd7, 0x2f,
  0x9d, 0x25, 0xdf, 0x9e, 0xb0, 0x4e, 0x32, 0x34, 0x70, 0xea, 0x97, 0xc7, 0xc7, 0xb6, 0xed, 0x34, 0x85, 0x31, 0x52, 0xdf,
  0xf9, 0xf8, 0xf8, 0x68, 0x58, 0xb3, 0x3f, 0xed, 0x9c, 0xbb, 0x63, 0xec, 0xd8, 0x7e, 0x6c, 0xf7, 0xec, 0xc7, 0xde, 0x6c,
  0xde, 0xb7, 0x5d, 0xfb, 0x08, 0x7f, 0x87, 0x19, 0xfe, 0x1c, 0xe2, 0xcf, 0x73, 0xfa, 0x59, 0xc3, 0x9f, 0x7f, 0x5f, 0xc4,
  0xf8, 0x50, 0xb3, 0x6b, 0xf0, 0xf0, 0xdd, 0xee, 0xd3, 0xbe, 0xbd, 0xfa, 0x38, 0xfe, 0xe4, 0x38, 0xab, 0x7c, 0x78, 0x5a,
  0xab, 0xfa, 0x2c, 0x3d, 0x77, 0xe3, 0x8b, 0x01, 0x49, 0xa7, 0x25, 0x9f, 0x5e, 0x36, 0x78, 0x54, 0xb7, 0xe9, 0xad, 0xed,
  0xf4, 0x33, 0x3a, 0xd8, 0xfc, 0x42, 0x7c, 0xd8, 0x04, 0x6a, 0x43, 0x09, 0x51, 0x26, 0xee, 0xd2, 0x0e, 0x78, 0x35, 0x0b,
  0xdd, 0x03, 0xcb, 0x7e, 0x52, 0x8f, 0x2f, 0x8e, 0x6d, 0xbc, 0x4f, 0x00, 0x20, 0x18, 0x79, 0x3e, 0xb4, 0x1e, 0xe3, 0x96,
  0x03, 0xd2, 0x43, 0xbc, 0x00, 0xeb, 0x53, 0x11, 0x07, 0x74, 0x9b, 0x13, 0x8a, 0x88, 0x2c, 0x62, 0x8d, 0xba, 0x33, 0x18,
  0x96, 0xbb, 0xb7, 0xdd, 0xdd, 0x6e, 0xab, 0x05, 0xa0, 0x7b, 0x64, 0xdd, 0xab, 0x09, 0x78, 0xf3, 0xa0, 0xbe, 0x48, 0x42,
  0x37, 0x9e, 0x67, 0x12, 0xf4, 0x64, 0xe0, 0x71, 0x87, 0x80, 0xa1, 0xc7, 0x23, 0x5f, 0xe2, 0x35, 0x4d, 0x96, 0x0f, 0xf4,
  0xd9, 0xcf, 0x92, 0x9b, 0xa5, 0x2f, 0xea, 0x24, 0xcd, 0xbf, 0xa5, 0x98, 0xa7, 0xb9, 0x1a, 0x93, 0x7b, 0x04, 0xd3, 0x5f,
  0x05, 0x93, 0xfa, 0xc3, 0xa4, 0x19, 0x5f, 0xdc, 0xde, 0xfa, 0xe2, 0x7e, 0xcc, 0xc1, 0x60, 0x60, 0x53, 0x1c, 0xde, 0x76,
  0x50, 0x26, 0x5d, 0x59, 0x14, 0xb7, 0xaf, 0xfb, 0xcd, 0x19, 0x8a, 0x86, 0x73, 0x76, 0x7b, 0xfb, 0xf9, 0x83, 0xd8, 0xdd,
  0x9a, 0x78, 0x01, 0x6e, 0x86, 0xd4, 0x1f, 0x2d, 0x13, 0xd1, 0x78, 0xe5, 0x7c, 0x76, 0xfa, 0x62, 0x79, 0x7d, 0x0d, 0xf3,
  0xca, 0x9b, 0x42, 0x1a, 0x71, 0x96, 0x8a, 0x25, 0xa1, 0x9f, 0xe4, 0x86, 0xef, 0xdc, 0xc5, 0xc9, 0xb3, 0x30, 0xac, 0xdb,
  0x74, 0x31, 0x2f, 0x90, 0x03, 0xc8, 0xb0, 0x57, 0x1e, 0x00, 0x79, 0x3d, 0x18, 0x5e, 0x73, 0xfc, 0xa0, 0x67, 0x22, 0x6e,
  0x79, 0xab, 0xdb, 0x3c, 0x67, 0xc0, 0x76, 0xaf, 0x9b, 0x40, 0x7e, 0x00, 0x31, 0x29, 0x5c, 0xfb, 0x09, 0x75, 0xef, 0xf4,
  0x37, 0xf5, 0xcf, 0x4f, 0x35, 0xdd, 0x67, 0x04, 0x3c, 0xff, 0x04, 0x4b, 0x46, 0x90, 0xc1, 0x58, 0x62, 0x0c, 0x12, 0xff,
  0x27, 0xd0, 0x2d, 0xe0, 0xa4, 0x09, 0xaf, 0x7f, 0x04, 0x63, 0xbc, 0x6e, 0x27, 0x63, 0x3a, 0x29, 0x65, 0x13, 0x33, 0x38,
  0x7d, 0x40, 0x2f, 0x7d, 0x35, 0x07, 0x20, 0x94, 0xba, 0xc7, 0x76, 0xd6, 0x68, 0xca, 0x3e, 0xff, 0xfe, 0xc8, 0x69, 0x5c,
  0xa7, 0xb3, 0xec, 0xd5, 0x73, 0xe0, 0x13, 0x00, 0xe8, 0xb9, 0xf4, 0x3e, 0x8d, 0xe7, 0x8d, 0x43, 0x57, 0x7d, 0xb1, 0xc4,
  0xe6, 0x9f, 0x2c, 0xb1, 0x57, 0x40, 0x3a, 0xf7, 0xc2, 0xc1, 0x68, 0x30, 0x1c, 0x35, 0xa5, 0x2a, 0x47, 0x9a, 0x54, 0x0b,
  0x36, 0x32, 0xa6, 0x6f, 0x70, 0x33, 0xda, 0xce, 0xc0, 0xfd, 0x2e, 0xb2, 0x8b, 0x4b, 0x87, 0x71, 0x06, 0xc0, 0xc8, 0x82,
  0x2e, 0x19, 0xb0, 0x14, 0x4a, 0x58, 0x24, 0x30, 0xe6, 0x70, 0x92, 0xe8, 0x33, 0x83, 0xb5, 0xf0, 0x37, 0x14, 0x69, 0xc4,
  0xaf, 0x19, 0xe5, 0xc0, 0x5c, 0xd4, 0xa5, 0x46, 0x44, 0x93, 0x19, 0x08, 0xee, 0x8c, 0xa5, 0xf5, 0xc8, 0x59, 0x22, 0x62,
  0x61, 0x29, 0x16, 0x61, 0x28, 0xfa, 0xb6, 0x7f, 0xff, 0xed, 0x3f, 0x6c, 0xc2, 0xf7, 0x51, 0xbb, 0xd5, 0xe9, 0x8a, 0x52,
  0x2b, 0x7a, 0x62, 0x5b, 0xcf, 0x55, 0x79, 0xf7, 0x70, 0xef, 0x60, 0x5f, 0xbc, 0xaa, 0x47, 0x3b, 0x54, 0x11, 0x16, 0xfc,
  0x35, 0x5e, 0xbf, 0x52, 0x6f, 0x39, 0x50, 0xf7, 0x5f, 0xa1, 0xb2, 0xf6, 0x9e, 0x37, 0x50, 0x55, 0xda, 0x58, 0xe5, 0xa7,
  0xe7, 0xb6, 0x06, 0xd4, 0x0c, 0x04, 0x8a, 0xb3, 0x9c, 0x0d, 0xde, 0x2e, 0x66, 0x23, 0x96, 0xc0, 0xef, 0xdb, 0xdb, 0x56,
  0xdf, 0x14, 0x70, 0x3f, 0x81, 0x86, 0x6d, 0x4e, 0xc2, 0x18, 0x56, 0x6f, 0xb6, 0xb3, 0xdf, 0x72, 0x9c, 0xe6, 0xdc, 0xf3,
  0x4f, 0x70, 0xdf, 0xb6, 0xde, 0x71, 0xed, 0x96, 0x0d, 0x9d, 0xf6, 0xec, 0x27, 0xa2, 0xf2, 0xec, 0x7b, 0xa8, 0x51, 0xac,
  0xa0, 0xcb, 0xb0, 0x19, 0x4a, 0x50, 0x8e, 0x63, 0x6f, 0x70, 0xd9, 0x4c, 0xe7, 0xa0, 0x01, 0xeb, 0xd0, 0x81, 0x43, 0xc2,
  0x98, 0x83, 0xa1, 0x78, 0xd0, 0xfb, 0xd8, 0xfa, 0xf4, 0x87, 0xfd, 0xd6, 0x13, 0xef, 0x63, 0xfb, 0x93, 0xce, 0x8e, 0x22,
  0x63, 0x1b, 0x16, 0x48, 0x0a, 0x63, 0xa9, 0x04, 0x9b, 0x13, 0xf0, 0x0f, 0x91, 0x27, 0xc4, 0x84, 0x90, 0xc1, 0x1c, 0xa0,
  0x5f, 0xf1, 0x08, 0x0f, 0x4e, 0xb9, 0xa3, 0x77, 0xe0, 0x78, 0x6a, 0x82, 0x5d, 0xf5, 0x85, 0x10, 0xa5, 0x83, 0xe1, 0x67,
  0xb1, 0xb7, 0x60, 0x51, 0x14, 0x68, 0x50, 0x7b, 0xb4, 0x4c, 0xa1, 0xd7, 0x55, 0x0d, 0xb4, 0x84, 0xe8, 0x36, 0x35, 0x47,
  0xb9, 0x74, 0x8e, 0x6d, 0x69, 0xb0, 0xc0, 0xd4, 0xec, 0xd5, 0xf0, 0xd1, 0x12, 0x75, 0x47, 0xda, 0x24, 0xb6, 0x5a, 0xa9,
  0x0d, 0x8a, 0xcf, 0x4e, 0xf3, 0x6f, 0x71, 0x10, 0xd5, 0x6d, 0x03, 0x47, 0x52, 0x63, 0x16, 0xc0, 0x52, 0x8a, 0x94, 0xe0,
  0x9a, 0x57, 0xc1, 0x35, 0x2f, 0xc0, 0x35, 0xdf, 0x0e, 0xae, 0xf9, 0xd6, 0x70, 0xa1, 0x02, 0x2d, 0x80, 0x65, 0xe8, 0xd6,
  0x6f, 0x0a, 0x1a, 0x07, 0xeb, 0x2e, 0xa8, 0x46, 0x8b, 0x20, 0xf4, 0x79, 0xb6, 0x7c, 0x5a, 0x77, 0x96, 0xa8, 0x39, 0xa6,
  0xc0, 0xc8, 0x7d, 0x90, 0x0d, 0x75, 0x7c, 0x98, 0x0d, 0x1a, 0x07, 0x9d, 0x56, 0x7f, 0x76, 0x34, 0x38, 0xec, 0xc2, 0x9f,
  0x27, 0x83, 0xdd, 0x96, 0x24, 0x40, 0x34, 0x43, 0x07, 0x33, 0x00, 0xa2, 0x05, 0x2a, 0xbc, 0x37, 0x1b, 0xc2, 0x1f, 0xa4,
  0xe5, 0xdf, 0xff, 0xfb, 0xbf, 0xdb, 0xae, 0x37, 0x20, 0xba, 0xf7, 0x46, 0x29, 0x30, 0x85, 0xcb, 0xcf, 0xda, 0x8a, 0xaa,
  0x6f, 0x63, 0x91, 0xda, 0x6f, 0xf7, 0x3e, 0x03, 0x2d, 0x40, 0x27, 0xab, 0x47, 0x4b, 0x8d, 0x4b, 0x3c, 0xe4, 0x92, 0x55,
  0xef, 0xd1, 0x52, 0xf0, 0x84, 0x57, 0xc9, 0x13, 0x9f, 0xfb, 0xd3, 0x27, 0x83, 0x32, 0x9e, 0x66, 0xab, 0x1a, 0x4c, 0x9d,
  0xc6, 0xd3, 0x66, 0xbe, 0x42, 0x05, 0x2f, 0x0e, 0x0d, 0x00, 0xab, 0x04, 0x51, 0xc4, 0x92, 0x1f, 0x4e, 0x7f, 0x7a, 0x33,
  0x98, 0x6a, 0x98, 0x00, 0x9f, 0x13, 0x7c, 0x50, 0x91, 0x44, 0x8e, 0xb8, 0xb0, 0xa0, 0x95, 0x9e, 0x3d, 0x6b, 0xb4, 0x2c,
  0x53, 0x79, 0x45, 0x16, 0x2e, 0xde, 0xa1, 0x43, 0xc7, 0x52, 0x8a, 0x04, 0x2c, 0xe3, 0x74, 0xb2, 0x5c, 0x1e, 0x30, 0xb8,
  0xbd, 0xc5, 0xd5, 0x31, 0x42, 0x0f, 0xda, 0xb2, 0x59, 0x65, 0x68, 0xf3, 0x5c, 0x68, 0x69, 0x11, 0x80, 0x0a, 0x40, 0x7b,
  0xc6, 0x48, 0xc7, 0x06, 0xcb, 0x04, 0xcb, 0x75, 0xf0, 0xd5, 0x81, 0x17, 0xee, 0xa7, 0x1f, 0xe7, 0x05, 0x38, 0xa1, 0xfa,
  0xb5, 0x1b, 0x38, 0xc5, 0x49, 0xc9, 0xa3, 0xe6, 0xfc, 0x48, 0x70, 0x80, 0xe8, 0x0e, 0x56, 0xb5, 0xaa, 0xf4, 0x63, 0x19,
  0x4b, 0x30, 0x8e, 0xde, 0x9b, 0x91, 0xe4, 0xbc, 0x53, 0x75, 0x2e, 0x05, 0x88, 0x41, 0x13, 0x28, 0xd7, 0xf2, 0xf6, 0x14,
  0xc2, 0x47, 0x31, 0x6c, 0x2c, 0x07, 0x20, 0x9a, 0x28, 0x84, 0x26, 0x8c, 0xae, 0x13, 0x15, 0x54, 0xe4, 0xb7, 0x59, 0xe4,
  0x94, 0x92, 0xf1, 0x21, 0x92, 0xcc, 0x59, 0xd5, 0x2a, 0x7a, 0x7e, 0x15, 0xf9, 0x6b, 0xfb, 0x85, 0x77, 0x9b, 0x7a, 0x85,
  0xa5, 0xd1, 0xfa, 0xdc, 0x2a, 0x55, 0x74, 0x16, 0xe7, 0x89, 0xed, 0x75, 0xc4, 0x2b, 0xb9, 0xce, 0x58, 0x5a, 0xf0, 0x9c,
  0x35, 0x72, 0xe8, 0xd9, 0xc6, 0xad, 0x1e, 0x78, 0xcf, 0x23, 0x1d, 0x8e, 0x37, 0x62, 0xb4, 0x72, 0x57, 0x0c, 0x03, 0x00,
  0x30, 0x5b, 0xdc, 0x5a, 0x9b, 0x4c, 0xac, 0x45, 0x14, 0x62, 0xb4, 0x72, 0xa2, 0x4e, 0x02, 0xc9, 0x73, 0x5e, 0x22, 0x3b,
  0x47, 0x38, 0x71, 0xba, 0xaa, 0x33, 0x8e, 0x04, 0x2c, 0x73, 0x72, 0x99, 0x2f, 0xd2, 0x69, 0x5d, 0xae, 0x1c, 0xf8, 0x27,
  0x84, 0x52, 0xf8, 0x0b, 0x48, 0xe8, 0xed, 0xb7, 0x56, 0xa8, 0x8c, 0x0a, 0x94, 0x6a, 0x50, 0xb1, 0x31, 0xf1, 0x40, 0xef,
  0x18, 0x75, 0x1b, 0x78, 0x04, 0x81, 0xdb, 0xae, 0xec, 0xa3, 0x60, 0x1c, 0x17, 0x4e, 0x06, 0x28, 0x6e, 0xb8, 0x02, 0x87,
  0xa5, 0xd9, 0x6c, 0x6e, 0xb2, 0x87, 0xe4, 0x90, 0xb6, 0xf3, 0xa9, 0xcf, 0x9b, 0xe1, 0x65, 0x09, 0x03, 0x6c, 0x4b, 0xcc,
  0x90, 0xa0, 0x23, 0x22, 0x27, 0xf8, 0x24, 0x29, 0x99, 0x64, 0x39, 0x0d, 0x83, 0x84, 0x20, 0x4a, 0x10, 0x48, 0x00, 0x6d,
  0xbd, 0xae, 0x76, 0x92, 0xc9, 0xba, 0x0e, 0x61, 0x6a, 0x5d, 0x55, 0xa0, 0x34, 0x55, 0x11, 0xbd, 0x29, 0x34, 0x65, 0x00,
  0xb8, 0x66, 0x1a, 0xcf, 0x18, 0xb7, 0x5d, 0xa1, 0xf5, 0xd1, 0x40, 0x52, 0xb2, 0xb3, 0xe4, 0x5e, 0x8e, 0x8d, 0x76, 0x5d,
  0x7e, 0xbf, 0xc3, 0x6c, 0x81, 0x36, 0x59, 0xe4, 0x5b, 0x78, 0x1b, 0x90, 0xf0, 0xf6, 0x03, 0x72, 0x7d, 0x93, 0x2c, 0x6d,
  0xda, 0x2e, 0x79, 0xa9, 0xd2, 0x6c, 0x58, 0xa1, 0xd3, 0xc0, 0x5d, 0x06, 0xf4, 0x38, 0xec, 0x1d, 0xf8, 0x77, 0x47, 0x2d,
  0x8b, 0xed, 0x2e, 0x67, 0x2c, 0x9b, 0xc6, 0x7e, 0xcf, 0x7e, 0xff, 0xee, 0xe4, 0xd4, 0x76, 0xf9, 0x15, 0x35, 0x69, 0x6f,
  0x69, 0x0b, 0xcb, 0xae, 0x71, 0x0a, 0x8c, 0x01, 0x7a, 0x00, 0xbf, 0x75, 0x17, 0x8c, 0x09, 0x2f, 0x3b, 0xe8, 0x79, 0x80,
  0xf7, 0x89, 0x97, 0x02, 0xf5, 0xfe, 0x74, 0xf2, 0xee, 0x2d, 0x80, 0x8b, 0xb2, 0x3d, 0x98, 0xdc, 0xd0, 0x74, 0x60, 0x6a,
  0xfd, 0xdc, 0xf7, 0xc6, 0x92, 0xbe, 0x98, 0x47, 0xf5, 0x49, 0x19, 0x1e, 0xe5, 0x6d, 0x82, 0x58, 0xe3, 0x70, 0xa2, 0xc3,
  0xad, 0x3b, 0x36, 0xbc, 0x31, 0x93, 0x4e, 0x8b, 0x98, 0xdf, 0xaa, 0x24, 0x3e, 0x9f, 0x67, 0x27, 0x61, 0x0c, 0xb2, 0x26,
  0x4d, 0xc6, 0x4a, 0xa9, 0x61, 0xc1, 0x00, 0x0a, 0x9a, 0xa3, 0xec, 0x0c, 0x59, 0x23, 0xbd, 0xbd, 0xfd, 0xf8, 0xc9, 0x7d,
  0x94, 0xa0, 0x50, 0x35, 0x13, 0x74, 0x6c, 0x5a, 0x8e, 0x47, 0x09, 0x5e, 0x8b, 0x95, 0x79, 0x01, 0xe8, 0x0c, 0x45, 0x65,
  0xdc, 0x7d, 0x10, 0xf1, 0x06, 0x47, 0x1a, 0xc0, 0x50, 0x55, 0x93, 0xc0, 0x38, 0xd0, 0x7a, 0x61, 0x1b, 0xd2, 0x1d, 0x42,
  0xc5, 0x5c, 0x21, 0x70, 0x0d, 0x6a, 0x86, 0x94, 0x3a, 0xa5, 0x33, 0x59, 0x86, 0x98, 0x5a, 0x2f, 0x97, 0x66, 0x41, 0xb4,
  0xc8, 0x00, 0x0f, 0xa5, 0x2d, 0x13, 0xc4, 0x02, 0x9d, 0x4f, 0x44, 0xc9, 0xe3, 0x16, 0xb6, 0xec, 0x2a, 0x93, 0x96, 0xe6,
  0x79, 0xf6, 0x2f, 0x1f, 0x7c, 0xdd, 0x8e, 0x61, 0xc5, 0x48, 0x6a, 0x6f, 0xb0, 0x3c, 0x1a, 0x58, 0x2d, 0xba, 0xf5, 0x76,
  0xdd, 0x94, 0x8f, 0x1b, 0x95, 0x80, 0xd8, 0x6c, 0xd9, 0x66, 0x60, 0xda, 0x28, 0x5d, 0x33, 0xae, 0xb2, 0xce, 0x68, 0x58,
  0x7c, 0x2a, 0x8f, 0x5a, 0xb5, 0x9b, 0x65, 0x51, 0x66, 0xe4, 0xa0, 0xf6, 0x2a, 0xe2, 0xdb, 0xeb, 0x2a, 0xcd, 0x0c, 0x25,
  0x54, 0xcd, 0x5c, 0x96, 0x7c, 0x67, 0xeb, 0xd1, 0x12, 0x79, 0x97, 0xa4, 0xed, 0xb1, 0x2d, 0xf6, 0xa8, 0xc8, 0x74, 0xab,
  0x82, 0x5c, 0x03, 0xf9, 0x6b, 0xb7, 0xb3, 0xd4, 0x85, 0xb4, 0x04, 0xc1, 0x58, 0x64, 0xb4, 0x1d, 0xdb, 0x78, 0x57, 0xa5,
  0xdd, 0x03, 0x7f, 0x37, 0x8e, 0xd8, 0x59, 0x16, 0xfb, 0xde, 0x8d, 0x8a, 0x6d, 0xd8, 0x64, 0x54, 0xe9, 0x95, 0x65, 0x22,
  0x5c, 0x0a, 0xfa, 0x05, 0x4f, 0x0d, 0xf0, 0xd0, 0xe3, 0x4e, 0xbe, 0x7f, 0xc5, 0xd5, 0x4e, 0xb1, 0x3f, 0x1e, 0x1d, 0x92,
  0x89, 0xad, 0xfc, 0x70, 0x8d, 0x45, 0xef, 0xb0, 0xa6, 0xc2, 0x06, 0x47, 0x24, 0x8e, 0x2c, 0xd2, 0x45, 0x7c, 0x7b, 0xa5,
  0x6f, 0x01, 0x68, 0x3a, 0x10, 0x0c, 0xa6, 0x6a, 0x25, 0xa8, 0x67, 0x42, 0x63, 0xb6, 0x2f, 0x72, 0x1b, 0xbf, 0x11, 0xe4,
  0x7c, 0x91, 0xe8, 0x0a, 0xae, 0xa0, 0x47, 0xe6, 0xe0, 0x94, 0xbf, 0xc6, 0xad, 0x92, 0x94, 0xc2, 0x29, 0xb4, 0x6b, 0x92,
  0xba, 0xb3, 0xf4, 0x5c, 0x0a, 0x88, 0x0b, 0x76, 0x93, 0x0e, 0xde, 0x8d, 0xfe, 0x06, 0x44, 0xd1, 0xc4, 0xdf, 0x75, 0x5e,
  0xc5, 0x71, 0x27, 0x14, 0xbc, 0xc3, 0xe3, 0x89, 0x2f, 0xc1, 0x3e, 0xaa, 0x3b, 0x7d, 0x7c, 0xab, 0x7c, 0xec, 0x8b, 0xc1,
  0x70, 0x59, 0x08, 0xf6, 0x35, 0x41, 0xa5, 0xd6, 0xf1, 0x73, 0xa1, 0x4d, 0x10, 0x95, 0xf0, 0xaa, 0x7e, 0xe1, 0x0a, 0x93,
  0x97, 0x77, 0xf9, 0xf1, 0x02, 0x03, 0x58, 0x20, 0x19, 0x0b, 0xd1, 0xc3, 0x27, 0x4f, 0x28, 0xae, 0xc3, 0xc1, 0x91, 0xd1,
  0x1d, 0x19, 0x1c, 0x2a, 0x48, 0x67, 0x92, 0xb8, 0x93, 0x15, 0x49, 0x2a, 0x9c, 0x04, 0x97, 0x8c, 0x5a, 0x38, 0x07, 0x0b,
  0xf3, 0x98, 0x0d, 0xb8, 0x7f, 0x18, 0xac, 0x5e, 0x16, 0x41, 0x2f, 0x42, 0xee, 0x33, 0xdc, 0xca, 0x01, 0xe0, 0xab, 0xa0,
  0x5b, 0x55, 0x22, 0xf5, 0x05, 0xe1, 0xbe, 0xae, 0x23, 0x54, 0x7a, 0x66, 0x39, 0xca, 0xb9, 0xb2, 0xe1, 0xcb, 0x64, 0xeb,
  0xc8, 0x2f, 0xeb, 0x7b, 0x3d, 0x3f, 0x6f, 0x99, 0xab, 0x2c, 0x6d, 0xa8, 0xa5, 0x0c, 0x76, 0xf7, 0xd0, 0xfc, 0x17, 0xbf,
  0xa5, 0x36, 0x5d, 0xb9, 0xb6, 0xca, 0x09, 0xb4, 0x78, 0x4c, 0xe6, 0xde, 0x0a, 0x45, 0x54, 0xc6, 0x4d, 0x0a, 0x31, 0xa4,
  0xb3, 0xaa, 0x04, 0xb4, 0xe2, 0x60, 0xf2, 0x3a, 0x90, 0xb5, 0xaa, 0x67, 0xdc, 0x4d, 0x3a, 0xe3, 0x62, 0x3b, 0xed, 0x99,
  0x4e, 0x8c, 0x9a, 0x85, 0xae, 0x21, 0x45, 0x54, 0xff, 0x0b, 0xe7, 0x53, 0x09, 0xbc, 0x3a, 0xbb, 0x1e, 0xf3, 0xc0, 0x4b,
  0x1c, 0x3d, 0x7e, 0x5c, 0xe7, 0x2b, 0x64, 0xa8, 0x48, 0xc7, 0xb0, 0x49, 0x88, 0x6b, 0x73, 0x73, 0x44, 0xa6, 0x7d, 0x79,
  0x15, 0x29, 0xb8, 0x82, 0x37, 0xd5, 0x51, 0x6b, 0xa8, 0x88, 0xbb, 0x0e, 0x13, 0xed, 0x24, 0x7f, 0x6e, 0xa2, 0x00, 0x0e,
  0xe4, 0xb1, 0x74, 0xc0, 0x81, 0x90, 0x85, 0x3c, 0xca, 0x5e, 0x36, 0x5f, 0x74, 0xc4, 0x62, 0x2b, 0x02, 0xf6, 0x2c, 0xbb,
  0xee, 0xc5, 0xd1, 0x71, 0x1b, 0x4c, 0xd4, 0xbc, 0x4c, 0x5a, 0x77, 0x5a, 0xf7, 0x05, 0x93, 0x6e, 0xe5, 0x42, 0x23, 0x3b,
  0x3f, 0x3a, 0x6f, 0x18, 0xd9, 0xd2, 0x6a, 0x06, 0x89, 0x95, 0x5f, 0x9d, 0x50, 0x5d, 0x03, 0x2f, 0x08, 0x26, 0xdc, 0xe9,
  0x00, 0x0d, 0xe2, 0x68, 0xcd, 0x42, 0x55, 0xcf, 0xf7, 0x21, 0xd4, 0xff, 0x82, 0x25, 0x54, 0x47, 0xf5, 0x97, 0x5b, 0xe0,
  0xa8, 0x72, 0xe4, 0x2f, 0x40, 0x9b, 0x9d, 0xfb, 0x80, 0xdf, 0x94, 0x2e, 0xcd, 0x6c, 0x3b, 0xf4, 0x5f, 0x88, 0x3c, 0x8b,
  0x92, 0x6a, 0xea, 0x81, 0x54, 0xc1, 0xdd, 0xfd, 0x33, 0xda, 0xb7, 0x3b, 0xc3, 0x8a, 0xb6, 0xb2, 0xc8, 0xf0, 0x49, 0x05,
  0xef, 0xb0, 0x0b, 0xe1, 0x00, 0x70, 0x62, 0x1c, 0x88, 0xa5, 0x2a, 0x34, 0x3f, 0x3e, 0x6e, 0xf5, 0x3f, 0xda, 0x79, 0x2e,
  0xa0, 0xed, 0xda, 0x79, 0x76, 0xa7, 0x78, 0x50, 0x64, 0x60, 0x7f, 0x52, 0x72, 0x14, 0xb7, 0x4a, 0x28, 0x2c, 0xda, 0x94,
  0x5f, 0x84, 0xe0, 0xdb, 0x10, 0xfd, 0xea, 0xd5, 0x28, 0x8c, 0xda, 0xc3, 0x7f, 0x56, 0x2e, 0x01, 0x60, 0x1b, 0x29, 0xae,
  0x3c, 0xdb, 0x4f, 0xa3, 0x41, 0xb5, 0xaf, 0x4d, 0xe9, 0x95, 0x72, 0xb0, 0xbe, 0xda, 0xef, 0xbe, 0xc2, 0x08, 0x2b, 0xb8,
  0x5e, 0x78, 0x86, 0x24, 0x8e, 0x34, 0x9a, 0x2c, 0x0c, 0x39, 0xc0, 0x7f, 0x84, 0xef, 0x55, 0x40, 0x75, 0xbe, 0x64, 0xc5,
  0xd7, 0x1c, 0x75, 0xce, 0x3a, 0xfa, 0x94, 0x1a, 0xe6, 0x1b, 0x21, 0x70, 0x9d, 0x65, 0xaf, 0x81, 0xab, 0xe2, 0xa5, 0xd4,
  0x7d, 0x1e, 0xaa, 0xc5, 0x68, 0x14, 0xca, 0x13, 0x6d, 0x68, 0x67, 0xc3, 0x4e, 0x00, 0x6f, 0xee, 0xc8, 0x16, 0x39, 0x7c,
  0x9b, 0x1a, 0x3d, 0x2c, 0xb4, 0x12, 0xf3, 0xcd, 0x39, 0x99, 0xbf, 0x97, 0xaf, 0x55, 0x7e, 0x27, 0xd4, 0xd0, 0x63, 0xe5,
  0xbc, 0x16, 0x2e, 0x3b, 0xa6, 0xc8, 0x06, 0xba, 0x21, 0xc3, 0xcf, 0x1b, 0x89, 0xa5, 0x34, 0x0e, 0xd2, 0x20, 0x25, 0x54,
  0x64, 0x83, 0x36, 0xbe, 0x3e, 0x17, 0xb4, 0xb1, 0x3e, 0x13, 0xd4, 0xae, 0x74, 0xcc, 0x79, 0xd6, 0x85, 0x58, 0x07, 0xae,
  0xd5, 0x07, 0x4b, 0x4c, 0x65, 0x41, 0xc9, 0x81, 0x7f, 0x95, 0xb8, 0x20, 0x8f, 0xaa, 0x6e, 0xcb, 0xd4, 0x0f, 0xe5, 0xfb,
  0xf2, 0x46, 0x4d, 0x59, 0x3e, 0xa8, 0xa8, 0x53, 0xcd, 0x46, 0xc2, 0x86, 0xb0, 0x8b, 0xb9, 0x1e, 0xc2, 0x85, 0xb4, 0x3e,
  0xb0, 0xb1, 0xba, 0x80, 0xd5, 0x0a, 0x26, 0x94, 0x4b, 0x87, 0x19, 0x71, 0xc8, 0x15, 0x15, 0x83, 0x0c, 0xc0, 0x18, 0xbe,
  0xbf, 0xb4, 0x12, 0xf9, 0xef, 0xa3, 0x2c, 0x92, 0x48, 0x88, 0x43, 0x7f, 0x00, 0x8f, 0xc7, 0xfa, 0x32, 0xe3, 0xdc, 0xa9,
  0x0a, 0x7d, 0x66, 0x4b, 0x17, 0x11, 0x7d, 0x2c, 0xd1, 0x09, 0xc2, 0xce, 0x73, 0xe8, 0xf9, 0x79, 0x49, 0xbb, 0xd2, 0x6b,
  0x8f, 0xb2, 0x79, 0x03, 0x07, 0x2f, 0x3a, 0xed, 0x2b, 0xc9, 0x9e, 0xf6, 0xdb, 0xd3, 0xf7, 0xf9, 0x1d, 0x3e, 0xa9, 0x27,
  0x38, 0x88, 0x36, 0xf8, 0x38, 0x0e, 0xb4, 0xdd, 0x4a, 0x14, 0xd6, 0x6e, 0xbb, 0x83, 0x5b, 0x93, 0x77, 0xa1, 0x40, 0x72,
  0x79, 0xe5, 0x8c, 0xb8, 0xb6, 0x2e, 0x4e, 0x09, 0x30, 0xb2, 0xaa, 0x46, 0x5d, 0x7e, 0x84, 0xba, 0x8c, 0x40, 0x03, 0x7f,
  0x5b, 0xe0, 0x0d, 0xd3, 0x02, 0xc9, 0xba, 0x90, 0x19, 0x94, 0x88, 0xbb, 0x35, 0xe6, 0x34, 0xc7, 0xe1, 0x48, 0x8e, 0xbe,
  0x19, 0x93, 0x9a, 0x49, 0x6d, 0x6b, 0x6e, 0x10, 0x59, 0x39, 0xe2, 0x4c, 0x1f, 0x1e, 0xe4, 0xab, 0x46, 0xea, 0xd3, 0xfb,
  0xe0, 0x74, 0x5b, 0x5c, 0x96, 0x8c, 0x70, 0x95, 0x19, 0xfb, 0xf5, 0x68, 0x7c, 0xcf, 0xd3, 0x13, 0xef, 0x81, 0x3c, 0x1c,
  0x7d, 0x2b, 0xe4, 0x89, 0xae, 0x0b, 0x48, 0xb3, 0xde, 0x2f, 0xf4, 0x63, 0xb7, 0x94, 0x31, 0x59, 0xca, 0x96, 0xe4, 0x29,
  0x92, 0x15, 0xf8, 0xdd, 0xfb, 0x67, 0xe0, 0xb7, 0x98, 0x7c, 0x2a, 0xec, 0xe5, 0x4a, 0x83, 0x04, 0x0c, 0x67, 0x2e, 0xd3,
  0xcf, 0x10, 0x61, 0xb9, 0x3d, 0xc2, 0xf1, 0x28, 0x2e, 0x00, 0xe1, 0x11, 0xa7, 0xbc, 0x47, 0xdb, 0x71, 0x85, 0x5d, 0xf2,
  0xf0, 0x61, 0x6e, 0x80, 0x6b, 0xfd, 0xf4, 0x45, 0xc3, 0xc2, 0xea, 0x99, 0x72, 0xd1, 0xf0, 0xb4, 0xa4, 0x34, 0x84, 0x95,
  0x31, 0xbb, 0x12, 0x26, 0x32, 0xb7, 0x7a, 0xb5, 0x43, 0xda, 0xba, 0x99, 0xa1, 0x17, 0xcb, 0x11, 0x35, 0x63, 0xc2, 0xec,
  0x10, 0x60, 0x06, 0xa3, 0x55, 0x28, 0x67, 0xf3, 0xe4, 0xbd, 0x6e, 0x00, 0x56, 0xb7, 0xe5, 0xd3, 0x5e, 0xd7, 0xfa, 0x2e,
  0x53, 0xa3, 0x84, 0x15, 0x7a, 0xaf, 0xdb, 0x0c, 0x72, 0xeb, 0x4c, 0x06, 0x88, 0x24, 0x77, 0xcc, 0xa5, 0xb5, 0x90, 0x83,
  0xc5, 0x4f, 0x23, 0x9f, 0x89, 0x00, 0x11, 0x98, 0x82, 0xca, 0x7b, 0xe6, 0x55, 0x9b, 0x41, 0xfa, 0x23, 0x10, 0xc9, 0x39,
  0xee, 0xd1, 0x39, 0x8f, 0x1f, 0xcf, 0x87, 0x83, 0x16, 0xfc, 0x7b, 0x64, 0x6e, 0xf3, 0x89, 0x3d, 0x97, 0x39, 0x20, 0xb8,
  0x64, 0xb8, 0x14, 0x67, 0xa7, 0x40, 0x29, 0x01, 0xe9, 0xce, 0x65, 0x4a, 0x4e, 0x0e, 0x9e, 0x2c, 0xa2, 0xa0, 0xe5, 0xf5,
  0x40, 0x3e, 0xf2, 0x7d, 0xdd, 0x4b, 0xb5, 0xaf, 0x7b, 0x29, 0xb6, 0x0f, 0xe7, 0xce, 0xed, 0xad, 0xac, 0xf3, 0x71, 0xfe,
  0xe9, 0xf6, 0x76, 0x89, 0x41, 0xda, 0x45, 0x04, 0x2e, 0xc6, 0xc3, 0xeb, 0x26, 0xfd, 0xba, 0xbd, 0xc5, 0x9f, 0xe2, 0xae,
  0x8a, 0xbe, 0x99, 0x94, 0x34, 0xc8, 0x37, 0x55, 0xe5, 0x00, 0x97, 0x3c, 0x25, 0x43, 0xed, 0x1f, 0xcb, 0x1a, 0xc7, 0xc7,
  0x55, 0x38, 0x14, 0xaf, 0x5a, 0x8e, 0x50, 0xb6, 0xc6, 0xdc, 0x0b, 0x46, 0x10, 0x41, 0x73, 0x6c, 0xbf, 0x50, 0xa9, 0xbe,
  0x7a, 0xea, 0x3a, 0x90, 0xe3, 0x5b, 0x99, 0x4e, 0xcd, 0xcb, 0xed, 0x42, 0x8f, 0xe2, 0x56, 0x06, 0xe8, 0x94, 0x5f, 0xcb,
  0x30, 0x78, 0x48, 0x1d, 0x16, 0x6a, 0x61, 0x26, 0x42, 0x61, 0xdc, 0x6b, 0xda, 0xb5, 0xbb, 0xbd, 0x55, 0xb9, 0x4d, 0x73,
  0x4c, 0x6d, 0x2a, 0xb4, 0x13, 0xb7, 0x31, 0x94, 0x9a, 0x0a, 0xbc, 0x81, 0x5c, 0x13, 0x35, 0xac, 0x45, 0xa4, 0xac, 0xaf,
  0x22, 0x88, 0x62, 0x65, 0x0b, 0x9d, 0x6c, 0x1c, 0x57, 0xc6, 0x52, 0x0b, 0x6d, 0x14, 0x6a, 0x05, 0xec, 0xf6, 0xcf, 0xe2,
  0x00, 0xb4, 0x7c, 0x61, 0xf7, 0x0d, 0xa3, 0x78, 0x9d, 0x40, 0x29, 0x49, 0xa0, 0x0a, 0xc3, 0xb5, 0x7c, 0xad, 0x43, 0x71,
  0x02, 0xc6, 0x25, 0x0f, 0x8f, 0x96, 0xd2, 0x9a, 0x65, 0x2a, 0xcc, 0xe7, 0xab, 0x30, 0x1f, 0xcd, 0xcf, 0x3c, 0x37, 0xbe,
  0xc6, 0x16, 0xd6, 0xa5, 0x8f, 0xb6, 0x71, 0x65, 0x5c, 0x62, 0xa0, 0x9d, 0x24, 0x5f, 0x7b, 0xe8, 0x21, 0xbf, 0xa1, 0xe4,
  0xc3, 0x6b, 0xec, 0x28, 0xf0, 0x43, 0xd6, 0xd4, 0x74, 0x0b, 0x5e, 0x7e, 0x38, 0x05, 0xe5, 0xe3, 0x23, 0xfa, 0x84, 0x7d,
  0xcc, 0xf7, 0xd2, 0xf0, 0xe4, 0x3d, 0x1a, 0xcd, 0xa1, 0x3a, 0x34, 0x61, 0x1c, 0x86, 0xc0, 0x5a, 0xc0, 0xd2, 0x29, 0x79,
  0x64, 0xdf, 0xfc, 0xe4, 0xbb, 0x79, 0x6e, 0xe3, 0x8b, 0x8f, 0xbc, 0x8b, 0x63, 0x1e, 0x38, 0xe2, 0x96, 0xa7, 0x3b, 0x24,
  0xe5, 0xf0, 0x94, 0x9f, 0x9c, 0x6c, 0xe8, 0xf9, 0xf6, 0xb6, 0xae, 0x7c, 0x95, 0xa2, 0xa5, 0x05, 0x68, 0x50, 0xd7, 0xb5,
  0x80, 0x96, 0xa0, 0xfc, 0x21, 0x5b, 0xbf, 0xce, 0xc5, 0xe6, 0x69, 0x44, 0xee, 0xce, 0x87, 0xd7, 0xb7, 0xc2, 0x0a, 0xb8,
  0xcd, 0xbf, 0x8e, 0x70, 0x8b, 0x26, 0x2f, 0xfe, 0x55, 0xc7, 0xb8, 0x77, 0x80, 0x2e, 0x40, 0xee, 0x53, 0x23, 0x47, 0x06,
  0xb0, 0xe5, 0xf0, 0x32, 0x74, 0xed, 0x94, 0xf7, 0xcc, 0x05, 0x0a, 0xb9, 0x2d, 0xa4, 0xe4, 0x2b, 0xe7, 0xbd, 0x81, 0x88,
  0xc0, 0x96, 0x84, 0x2d, 0x49, 0x5b, 0x2e, 0xf4, 0x44, 0x95, 0x8d, 0xf2, 0xac, 0xff, 0xd1, 0x2e, 0xdc, 0xc3, 0x04, 0x7e,
  0xad, 0x71, 0x98, 0xab, 0xe0, 0xd7, 0xa2, 0xa9, 0x50, 0xbd, 0x7d, 0xf4, 0x70, 0xc0, 0x93, 0xa9, 0x1c, 0xee, 0xfa, 0x72,
  0x37, 0x44, 0x80, 0xbb, 0x2a, 0x0c, 0xc4, 0x79, 0x5b, 0x1b, 0x89, 0x17, 0x7c, 0xed, 0x50, 0xd4, 0x8b, 0xda, 0xca, 0x2d,
  0xab, 0x6f, 0x2d, 0x21, 0x8a, 0x25, 0xe7, 0x8c, 0x4b, 0x01, 0x69, 0xf1, 0xd4, 0x7d, 0x67, 0x89, 0xd9, 0x24, 0x2a, 0x7c,
  0x8e, 0x77, 0x54, 0x7c, 0x2c, 0x9a, 0x40, 0x08, 0x33, 0xa7, 0x21, 0xfe, 0x53, 0x98, 0x74, 0xe2, 0x41, 0xa8, 0x28, 0xfe,
  0x64, 0xaa, 0xe0, 0x62, 0x99, 0x44, 0x41, 0x31, 0xe6, 0xf3, 0xc9, 0x81, 0x69, 0x8b, 0xb8, 0x3d, 0xd8, 0x60, 0xef, 0xae,
  0xa2, 0xba, 0xef, 0x02, 0x34, 0xa0, 0xa4, 0x1f, 0x56, 0xd9, 0x68, 0xf8, 0xca, 0xe1, 0x6b, 0xfc, 0x11, 0x7e, 0x7f, 0x1a,
  0xf8, 0xf4, 0xa7, 0x5f, 0x49, 0x44, 0x94, 0xe8, 0xb6, 0x75, 0xe8, 0xc9, 0x0c, 0x50, 0xac, 0x0d, 0x32, 0x39, 0xd5, 0x26,
  0x66, 0xe1, 0x80, 0xdd, 0xe5, 0xba, 0x68, 0x5e, 0x19, 0x2b, 0xbd, 0xcb, 0x95, 0x6b, 0xf3, 0xe6, 0x4a, 0x11, 0xe4, 0x5b,
  0xa9, 0x6b, 0xe9, 0x79, 0xf0, 0xe4, 0x52, 0xb8, 0xe5, 0x25, 0x4a, 0x43, 0x4a, 0xa9, 0x28, 0x15, 0x94, 0x23, 0x53, 0x7c,
  0x1d, 0xd5, 0xdc, 0xa0, 0x4a, 0xd1, 0xd8, 0x2c, 0x2b, 0x35, 0xbd, 0xd3, 0x64, 0xfc, 0xba, 0xd8, 0x7c, 0xf1, 0x10, 0xe5,
  0x76, 0xe8, 0xa4, 0x1b, 0x95, 0x10, 0x9b, 0xc6, 0x01, 0x4e, 0x25, 0x9d, 0xd7, 0xa1, 0x93, 0x24, 0x4c, 0x15, 0x36, 0xb9,
  0xda, 0x77, 0xaa, 0x0a, 0xef, 0xc2, 0xa5, 0xde, 0xd4, 0x28, 0xfa, 0xa7, 0x62, 0xb2, 0x74, 0xd9, 0x99, 0x32, 0x5b, 0x9f,
  0xac, 0x9f, 0x14, 0x71, 0x0d, 0xe1, 0x29, 0x99, 0xd5, 0x3f, 0xf3, 0xcb, 0xd1, 0x50, 0xc9, 0xc9, 0x0b, 0xd2, 0xb4, 0x6b,
  0xc9, 0x84, 0xf1, 0x84, 0xca, 0x44, 0x37, 0x87, 0x8e, 0x3f, 0x2b, 0xd7, 0xa9, 0xec, 0x86, 0x16, 0x56, 0x8f, 0x83, 0xa8,
  0x56, 0x6f, 0x5e, 0xe9, 0x7d, 0xf2, 0xce, 0xe5, 0x29, 0x39, 0x4a, 0xe7, 0xd6, 0xe3, 0xd5, 0x06, 0x12, 0xbe, 0x24, 0x6a,
  0xad, 0xed, 0xb5, 0x07, 0xee, 0x26, 0x5a, 0xc3, 0x4d, 0x91, 0x5e, 0xe0, 0x8a, 0x5f, 0x14, 0x6e, 0xef, 0x51, 0xf6, 0x27,
  0x10, 0xdd, 0x33, 0x63, 0x87, 0x99, 0x1f, 0xcc, 0x2b, 0x64, 0x44, 0xdc, 0x07, 0xd0, 0xed, 0x59, 0xc5, 0xd8, 0xbd, 0xbf,
  0x17, 0xfc, 0x86, 0x08, 0x7a, 0x5f, 0x92, 0x3d, 0xff, 0x04, 0xa0, 0x89, 0xbb, 0xef, 0x0b, 0xf3, 0x3a, 0x3e, 0xff, 0x67,
  0xc1, 0x0d, 0x00, 0x63, 0x18, 0x61, 0x5b, 0x88, 0x85, 0x85, 0x69, 0x3a, 0xef, 0x55, 0x5b, 0x6e, 0xba, 0x17, 0x5f, 0xf5,
  0x5e, 0x77, 0xe7, 0xbf, 0xd5, 0x1c, 0xd5, 0xec, 0xf4, 0x4f, 0xd1, 0xe0, 0x37, 0xb0, 0xea, 0x14, 0x27, 0x79, 0x58, 0x4c,
  0x49, 0x04, 0xc7, 0x33, 0x3f, 0x78, 0xd3, 0x44, 0x22, 0x17, 0xac, 0xfe, 0x91, 0xb2, 0xbd, 0x5d, 0x5b, 0xff, 0x82, 0x4d,
  0x1e, 0x10, 0xb7, 0x65, 0x12, 0xd7, 0xc7, 0xe9, 0xd4, 0x9d, 0xcd, 0x3e, 0x0d, 0x0a, 0x9d, 0xac, 0xc9, 0x99, 0x76, 0xc1,
  0x58, 0x1e, 0x4c, 0xa7, 0x98, 0x2e, 0x3d, 0x9b, 0x49, 0xe7, 0x88, 0x9f, 0xe1, 0xc9, 0x01, 0x93, 0xd9, 0xd1, 0x50, 0x77,
  0x28, 0x73, 0xae, 0x1e, 0x3f, 0xc6, 0xab, 0xc3, 0x78, 0xd2, 0x1f, 0x0a, 0x34, 0xde, 0x48, 0xa5, 0x13, 0x65, 0x32, 0x53,
  0x54, 0xbc, 0x50, 0x59, 0x8d, 0x7d, 0x39, 0x19, 0x7e, 0x52, 0x08, 0x2f, 0x20, 0xb3, 0xdd, 0xcf, 0x94, 0xa9, 0x93, 0xd7,
  0xc4, 0xe4, 0xc4, 0xdf, 0x7f, 0xfb, 0x9f, 0x7a, 0x29, 0x25, 0x17, 0xe2, 0x67, 0xcc, 0x30, 0x5d, 0xf2, 0x38, 0xcd, 0x9a,
  0xfc, 0x30, 0x8f, 0xb6, 0xcb, 0x66, 0x3f, 0x31, 0x87, 0x5a, 0x7d, 0xfe, 0xb4, 0xc2, 0x54, 0xde, 0x11, 0x58, 0xcb, 0x94,
  0x34, 0xdf, 0xcf, 0xcd, 0xb1, 0x6b, 0x34, 0xc6, 0xd4, 0x04, 0x1d, 0x25, 0x41, 0x65, 0x6e, 0x64, 0x03, 0xc0, 0x7a, 0xd2,
  0xee, 0x76, 0x5b, 0xce, 0xf7, 0xf8, 0x2f, 0x85, 0xa8, 0xb1, 0x1b, 0x9e, 0x7d, 0x7f, 0x7b, 0xeb, 0x1f, 0xe1, 0x63, 0xd3,
  0x77, 0xa8, 0x74, 0xe9, 0xbb, 0xd7, 0x2b, 0x3a, 0x55, 0x82, 0x8f, 0x85, 0xe5, 0x52, 0x0b, 0xa3, 0xe1, 0x84, 0x1a, 0x5f,
  0x97, 0x70, 0xc2, 0xfb, 0x3c, 0xda, 0x6f, 0x1d, 0x7f, 0x06, 0xc7, 0xe5, 0xd1, 0x92, 0x3f, 0xaf, 0xac, 0x59, 0x10, 0x7d,
  0xee, 0xf1, 0x22, 0x2d, 0x61, 0x98, 0xbf, 0xa5, 0xac, 0xe1, 0xa9, 0xaa, 0xfc, 0xfd, 0x7e, 0x6b, 0x35, 0xfb, 0x2c, 0x10,
  0x9a, 0x0f, 0x83, 0x08, 0xbd, 0x03, 0x79, 0x26, 0x4c, 0x88, 0xbc, 0xaa, 0x1c, 0xe1, 0x05, 0xd8, 0xb5, 0x2e, 0xb7, 0x76,
  0xe5, 0xd9, 0x2d, 0x7e, 0xa6, 0x48, 0x3b, 0x2c, 0xe6, 0x53, 0xd6, 0x85, 0x5e, 0xc7, 0xa9, 0xb6, 0x8f, 0xd1, 0xe9, 0x15,
  0x1f, 0x91, 0x93, 0xdb, 0x49, 0xfc, 0x48, 0x84, 0x1f, 0x67, 0x16, 0x79, 0x32, 0xb2, 0x06, 0xf2, 0x4b, 0xc1, 0x21, 0xb6,
  0xf1, 0x0e, 0x71, 0xaa, 0x90, 0x05, 0xa5, 0xe0, 0x85, 0x4f, 0x34, 0x0f, 0x8a, 0x4d, 0x7e, 0xbb, 0x8d, 0x2a, 0xe2, 0x2e,
  0x6c, 0xd1, 0x49, 0x7f, 0xb4, 0xf4, 0xf1, 0xb0, 0x07, 0xd6, 0xb5, 0x05, 0x8a, 0x78, 0x63, 0xcc, 0x90, 0xa0, 0x42, 0xf2,
  0xcd, 0x27, 0x57, 0xa5, 0x11, 0xe4, 0x11, 0xc9, 0x33, 0x70, 0x40, 0xd1, 0x73, 0x84, 0xca, 0x78, 0xfe, 0xd1, 0xee, 0x3f,
  0xb0, 0x44, 0x3c, 0x13, 0x5d, 0x5d, 0xa8, 0x48, 0x7f, 0xa5, 0xa3, 0x28, 0x43, 0x12, 0xb6, 0x2b, 0x3e, 0xad, 0x33, 0xa0,
  0xd7, 0xe0, 0x82, 0xc0, 0x9b, 0xfc, 0xb3, 0x02, 0x22, 0x90, 0xa3, 0xbe, 0xf4, 0x56, 0x18, 0x9d, 0xda, 0x08, 0x5f, 0x52,
  0x7d, 0x72, 0xc7, 0x76, 0xe9, 0xa7, 0xec, 0xb8, 0x70, 0x46, 0x4b, 0x42, 0x95, 0x4c, 0x06, 0x0f, 0x1f, 0xfa, 0x4d, 0xda,
  0xc8, 0x3a, 0xe3, 0x1c, 0x23, 0x47, 0xfa, 0x30, 0x29, 0x0c, 0x93, 0x4c, 0x8e, 0xc1, 0x4c, 0xd0, 0xf7, 0xc7, 0xf5, 0x5c,
  0x80, 0xd3, 0xbf, 0x18, 0x1b, 0xff, 0xf0, 0xe8, 0x00, 0x4d, 0xd1, 0xd7, 0xc3, 0x6c, 0x01, 0x1b, 0xff, 0xd6, 0x8c, 0xed,
  0x42, 0x47, 0x32, 0x63, 0x83, 0x56, 0x1f, 0x1a, 0xe2, 0xe7, 0x67, 0xf8, 0x0b, 0xe5, 0xb3, 0x8a, 0x46, 0xf9, 0x75, 0xed,
  0xfc, 0xfd, 0xbb, 0x08, 0x7d, 0x68, 0xe8, 0xb4, 0xb2, 0xb6, 0xfc, 0xa4, 0x82, 0xed, 0xae, 0x85, 0x54, 0x07, 0xb3, 0x5c,
  0x8d, 0x3b, 0xd0, 0xbc, 0x5f, 0x44, 0xd3, 0x9a, 0xdc, 0x83, 0x87, 0x66, 0x3b, 0x22, 0xbc, 0xeb, 0xd3, 0x4a, 0xd2, 0xbb,
  0x3e, 0x13, 0xd4, 0x47, 0xc7, 0x73, 0x1e, 0x89, 0x4f, 0xd8, 0x9c, 0x6e, 0x55, 0xbb, 0x24, 0x2b, 0xfc, 0x5c, 0x4c, 0xd0,
  0x90, 0x79, 0xa6, 0x81, 0xde, 0x8f, 0xc1, 0xd6, 0x62, 0x09, 0xe4, 0xc0, 0xc6, 0x05, 0xf1, 0x1b, 0xda, 0xe5, 0x5f, 0xa1,
  0xe3, 0x9f, 0xdc, 0xe1, 0xe3, 0xc9, 0x4b, 0xe1, 0xcb, 0x0d, 0xeb, 0xd0, 0xd2, 0xc8, 0xe0, 0xef, 0x99, 0x13, 0x36, 0xaf,
  0x93, 0xbf, 0x57, 0x7b, 0xfe, 0x61, 0xbc, 0x4a, 0x6a, 0x04, 0x7c, 0xa1, 0x26, 0x15, 0xe9, 0x41, 0xb0, 0xca, 0x74, 0x92,
  0x18, 0x0f, 0xee, 0xb2, 0x14, 0xbf, 0xf1, 0x2c, 0x4e, 0x00, 0xd3, 0xbe, 0x08, 0x55, 0x81, 0xb5, 0x2d, 0xd5, 0x91, 0x67,
  0xed, 0x45, 0x0d, 0x47, 0x9f, 0xbc, 0x1e, 0x0d, 0xe2, 0x93, 0xa1, 0x4f, 0x1b, 0x6d, 0xc1, 0x1a, 0x79, 0x54, 0x4b, 0x7c,
  0xd4, 0x09, 0x86, 0x56, 0xaa, 0x5a, 0xec, 0x8d, 0xc3, 0x50, 0xe3, 0xfc, 0x13, 0x49, 0xb9, 0xb0, 0xb8, 0x02, 0xb9, 0x89,
  0x5e, 0x13, 0xca, 0xd1, 0x33, 0x94, 0x03, 0x67, 0xfc, 0x5c, 0x18, 0x20, 0x46, 0xbf, 0x93, 0x5b, 0x49, 0x85, 0x5f, 0xf8,
  0x05, 0x2d, 0x05, 0x62, 0xca, 0x23, 0x08, 0xc7, 0xfa, 0x66, 0x55, 0xef, 0x2a, 0x6d, 0x06, 0xd1, 0x38, 0x5c, 0xf8, 0x0c,
  0x7c, 0x73, 0xf5, 0x01, 0x23, 0xdb, 0x39, 0xb6, 0x5f, 0xaa, 0x87, 0x1e, 0x35, 0xf7, 0xbd, 0x9b, 0x33, 0x79, 0x67, 0xc1,
  0x31, 0xed, 0xb7, 0xe6, 0xb9, 0x8a, 0x76, 0xfe, 0x45, 0x47, 0xc9, 0xe0, 0xf2, 0x4e, 0x20, 0x64, 0xab, 0x8a, 0xc1, 0xa1,
  0x0d, 0xdd, 0x15, 0x54, 0x7c, 0x6d, 0xf2, 0xdc, 0x23, 0x71, 0xdf, 0x4c, 0x65, 0x18, 0xf5, 0x8a, 0xa2, 0xad, 0x85, 0x0f,
  0x1f, 0x55, 0xd6, 0x51, 0xb7, 0x91, 0x57, 0xa1, 0x25, 0xc7, 0xab, 0x90, 0xf6, 0x55, 0x9f, 0xd3, 0x4b, 0xd8, 0x18, 0x3f,
  0xff, 0x2a, 0xb0, 0xac, 0x7f, 0x2c, 0xe9, 0x1b, 0x74, 0xc9, 0x11, 0xa6, 0x3e, 0xfe, 0x24, 0x50, 0x62, 0xe2, 0xfb, 0x25,
  0xa6, 0xe5, 0xe9, 0xd8, 0xc6, 0x34, 0xd2, 0xca, 0x9a, 0xb9, 0x18, 0x04, 0x50, 0xf3, 0x2f, 0x05, 0x56, 0xc1, 0xb9, 0x61,
  0x88, 0xf7, 0x3c, 0x76, 0x63, 0x9b, 0x9d, 0x9c, 0x2c, 0x46, 0x5f, 0x3c, 0xdf, 0x1b, 0x96, 0x21, 0x51, 0x63, 0xac, 0x4d,
  0xe5, 0xc9, 0xe9, 0xfb, 0x5b, 0x54, 0x30, 0xc8, 0xdf, 0xf5, 0xcd, 0x94, 0xf4, 0x65, 0xb3, 0xd9, 0xe4, 0x95, 0x5d, 0x59,
  0xa3, 0x97, 0x57, 0x5e, 0x81, 0x0d, 0xce, 0x4f, 0x2a, 0xf0, 0x74, 0x79, 0xd3, 0x9c, 0xc6, 0x49, 0xe8, 0xa5, 0x85, 0x39,
  0x44, 0xe9, 0xc7, 0xd6, 0xa7, 0x62, 0x9d, 0xf2, 0x54, 0xa1, 0x1a, 0x1e, 0x1b, 0x2c, 0xb8, 0x26, 0xba, 0x51, 0x2f, 0x66,
  0x53, 0xda, 0xf5, 0x15, 0xf9, 0x9b, 0x04, 0x87, 0x9e, 0xdd, 0x21, 0xa3, 0xc5, 0x58, 0x86, 0x72, 0xae, 0x5f, 0x91, 0x97,
  0x29, 0xeb, 0x68, 0x96, 0x47, 0xf9, 0xce, 0x45, 0xbb, 0xbf, 0x4e, 0x37, 0x55, 0x24, 0xd7, 0xf5, 0x2b, 0xb3, 0xd3, 0x4a,
  0x27, 0xb9, 0x28, 0xf3, 0xbb, 0xd4, 0x5c, 0xbc, 0xbc, 0xbd, 0x6d, 0x09, 0xad, 0x63, 0xe6, 0x5f, 0x16, 0x62, 0xc1, 0x1b,
  0xb2, 0x37, 0x29, 0x28, 0x5c, 0x1d, 0x2c, 0xcb, 0x41, 0x31, 0x92, 0xdf, 0xd7, 0xc6, 0xe3, 0x24, 0x30, 0xe5, 0xa8, 0x8a,
  0xd1, 0x93, 0x4a, 0x67, 0x5f, 0x17, 0x89, 0xca, 0xfb, 0x29, 0x84, 0xe0, 0xbe, 0x1c, 0x20, 0x33, 0x00, 0xf5, 0xa5, 0xe0,
  0x54, 0x6e, 0x38, 0xad, 0xdb, 0xa2, 0xda, 0x2e, 0x9c, 0x4a, 0x9d, 0x9b, 0x1c, 0xc6, 0x2b, 0x6e, 0x08, 0x6a, 0x57, 0x50,
  0xfe, 0x73, 0x2f, 0x95, 0xe9, 0xc6, 0xc2, 0xc9, 0x4c, 0x33, 0x37, 0x1d, 0x7f, 0x12, 0x2c, 0x00, 0x53, 0x87, 0xd5, 0x67,
  0x4d, 0x0f, 0x04, 0xdc, 0x47, 0xed, 0xf0, 0x8c, 0x38, 0x2b, 0x68, 0x3b, 0x6e, 0xd5, 0x89, 0x1a, 0xe7, 0x93, 0xd3, 0x57,
  0xb7, 0x50, 0xa4, 0x99, 0x76, 0x12, 0x26, 0x1d, 0xf7, 0xcd, 0xf3, 0x99, 0xfd, 0xe2, 0x21, 0xc5, 0xf2, 0xc9, 0xa8, 0x0a,
  0xe7, 0x7b, 0x7d, 0x6e, 0xe9, 0x5a, 0xa7, 0x03, 0x4d, 0xe5, 0x4d, 0x3e, 0x07, 0x40, 0x14, 0x06, 0xc8, 0x88, 0x22, 0x8f,
  0xe8, 0x67, 0x7e, 0xf6, 0x41, 0x5e, 0xd9, 0x92, 0xef, 0x70, 0xf5, 0xc0, 0xa3, 0xba, 0x3b, 0x32, 0x56, 0x3e, 0x14, 0x0f,
  0x2e, 0xea, 0x58, 0x5c, 0x9f, 0xc1, 0xc3, 0x03, 0x54, 0xf0, 0xf8, 0x31, 0xea, 0xa0, 0x26, 0xf8, 0x0d, 0x75, 0xa7, 0xc1,
  0xef, 0xdb, 0x38, 0xda, 0x6b, 0xb5, 0x5a, 0x32, 0x08, 0x28, 0xae, 0xe0, 0xc8, 0x2b, 0x6d, 0xcc, 0x4e, 0xd1, 0x2e, 0x96,
  0x01, 0x61, 0xa5, 0xee, 0xf5, 0xf0, 0x95, 0x69, 0x01, 0x34, 0x54, 0x92, 0xff, 0x44, 0x58, 0x22, 0x67, 0x0c, 0x7d, 0x8b,
  0x17, 0xf2, 0x27, 0xb7, 0xcd, 0x39, 0x56, 0x84, 0x8a, 0x13, 0x97, 0x4a, 0xa1, 0xde, 0xda, 0xa6, 0x59, 0x45, 0x35, 0xa1,
  0xdd, 0xc8, 0x4c, 0x70, 0xb8, 0x0d, 0x86, 0x9f, 0xa1, 0x2c, 0x41, 0xa5, 0x7b, 0x31, 0x68, 0x8f, 0x09, 0xd3, 0xea, 0x6c,
  0xfa, 0xeb, 0x4e, 0x1b, 0xf1, 0xa3, 0x0e, 0xb1, 0xeb, 0xaf, 0xbe, 0xc7, 0x57, 0x18, 0x29, 0xc2, 0x83, 0xed, 0x17, 0x3f,
  0xfc, 0x2a, 0xbc, 0x0b, 0x1c, 0x26, 0xff, 0x12, 0x69, 0x61, 0x28, 0xb1, 0xb1, 0x2f, 0x9d, 0x39, 0xba, 0x2b, 0xe2, 0x8c,
  0xa1, 0x3e, 0x64, 0x63, 0x67, 0x38, 0x78, 0x8a, 0x3d, 0x2a, 0xd7, 0xae, 0xb7, 0xb1, 0xb6, 0x82, 0x69, 0x17, 0xc7, 0x4f,
  0x85, 0x45, 0xc7, 0x3f, 0x12, 0x58, 0x18, 0x55, 0x1d, 0xfe, 0xf7, 0x9b, 0x58, 0xe3, 0x0c, 0xbf, 0x17, 0x48, 0xe8, 0xc8,
  0xbf, 0xff, 0x57, 0x42, 0x09, 0x7f, 0x75, 0xc6, 0x2f, 0xc9, 0x3f, 0xcb, 0x3f, 0xfe, 0x77, 0x7c, 0xac, 0xac, 0x6b, 0xfe,
  0x5d, 0xbf, 0x52, 0xcb, 0x90, 0x3e, 0xea, 0x37, 0x49, 0xcf, 0xb2, 0x38, 0xf3, 0xc2, 0x63, 0x0a, 0x35, 0x24, 0xd8, 0xbe,
  0x0e, 0x08, 0xfb, 0x83, 0xf6, 0x1e, 0x3f, 0xfa, 0xb7, 0x53, 0xac, 0x0f, 0x93, 0xf9, 0xde, 0xce, 0x2d, 0xf8, 0xfc, 0xa6,
  0xee, 0x2a, 0x53, 0x22, 0xbf, 0x63, 0xed, 0xcc, 0x13, 0xd5, 0x34, 0xf8, 0xf8, 0x65, 0xdc, 0x55, 0xed, 0xe8, 0x32, 0xee,
  0x33, 0x7e, 0x19, 0xb7, 0xd1, 0x80, 0xdf, 0xb3, 0x5d, 0xd5, 0x44, 0xde, 0xb3, 0xcd, 0x57, 0x41, 0xb5, 0x12, 0x71, 0x7a,
  0x10, 0x7f, 0xe9, 0xe0, 0xe3, 0x47, 0xfb, 0xb5, 0x70, 0xe6, 0xc9, 0x3d, 0x14, 0xbf, 0x3f, 0xb9, 0x1f, 0xed, 0x9f, 0xe7,
  0xe4, 0x40, 0x42, 0xe9, 0x62, 0xce, 0x35, 0x23, 0x1b, 0xd3, 0xaa, 0xe1, 0x4b, 0xee, 0x99, 0xbb, 0x86, 0x87, 0x8f, 0xe5,
  0x98, 0x5d, 0x08, 0x6c, 0x8f, 0x6f, 0xa2, 0x6c, 0x7e, 0x06, 0x3f, 0x8d, 0x66, 0xaf, 0xd4, 0x67, 0x6c, 0xf9, 0xcd, 0x53,
  0xfc, 0xe2, 0x11, 0x77, 0x3b, 0xa2, 0xe9, 0x3a, 0xaa, 0x1f, 0x1c, 0x86, 0x4b, 0x90, 0x54, 0x0e, 0x45, 0xd6, 0xd9, 0x18,
  0xd6, 0x2c, 0x93, 0x15, 0xf0, 0x36, 0xa6, 0x04, 0x94, 0xb6, 0xac, 0x21, 0x9f, 0x0d, 0x88, 0x44, 0x96, 0x68, 0x05, 0x27,
  0x7e, 0x56, 0x0c, 0x2b, 0x63, 0x23, 0x54, 0x21, 0x98, 0xaf, 0x3e, 0xe7, 0x0c, 0x8c, 0x5d, 0x7c, 0x78, 0x8d, 0xad, 0x0d,
  0x7e, 0x2c, 0x78, 0xfc, 0xf2, 0x2d, 0xed, 0xbd, 0xfb, 0xe8, 0x22, 0xe0, 0x5f, 0x11, 0x09, 0xb0, 0x76, 0xc8, 0x1d, 0x22,
  0xac, 0x72, 0x46, 0x25, 0xbc, 0x2a, 0x9e, 0x05, 0x50, 0x81, 0x51, 0xf1, 0xf5, 0x73, 0x4e, 0xd1, 0x37, 0x56, 0xe6, 0xa5,
  0x17, 0x78, 0x91, 0x97, 0xc7, 0xbd, 0x0b, 0x51, 0x7c, 0x46, 0x25, 0x40, 0xa2, 0xd0, 0xe2, 0x3f, 0xff, 0xaf, 0x98, 0x5e,
  0x8c, 0xf7, 0xa5, 0x6e, 0xac, 0x7f, 0x85, 0x55, 0xcc, 0x56, 0x3f, 0xad, 0xf9, 0x8a, 0x26, 0xb6, 0x5e, 0xc7, 0x64, 0xd8,
  0xee, 0xb5, 0xfc, 0xa8, 0x27, 0x56, 0x54, 0x6c, 0x0b, 0x1d, 0x8f, 0x90, 0x95, 0x45, 0xd7, 0x51, 0x30, 0x5b, 0xcc, 0xac,
  0x49, 0xa9, 0x2a, 0x98, 0x4d, 0xe5, 0xea, 0xf2, 0x6b, 0x9b, 0x14, 0x28, 0x2d, 0xf0, 0xe1, 0x0a, 0x30, 0x67, 0x14, 0x12,
  0x33, 0xae, 0x78, 0xeb, 0xcf, 0x84, 0xb0, 0x37, 0xaf, 0xe4, 0xb9, 0x34, 0xee, 0x48, 0xf0, 0x07, 0xb5, 0x4a, 0x3c, 0x10,
  0x2b, 0x23, 0x32, 0xb2, 0x85, 0x20, 0x81, 0x14, 0xba, 0xd7, 0x6f, 0xc0, 0x87, 0xc7, 0x28, 0xce, 0x80, 0x04, 0xf8, 0x09,
  0x57, 0x7c, 0xe6, 0x7c, 0x25, 0x60, 0xab, 0xe6, 0x6d, 0x09, 0x64, 0x81, 0x83, 0xb5, 0x62, 0xa3, 0x53, 0xad, 0xbc, 0xc0,
  0xbd, 0x2b, 0x35, 0x23, 0x75, 0x83, 0x1b, 0xbb, 0xe4, 0x1f, 0xbb, 0x96, 0x5e, 0x92, 0x28, 0x3f, 0x93, 0xe5, 0x9f, 0xc8,
  0xe6, 0x37, 0x74, 0x9e, 0x99, 0x33, 0x84, 0x22, 0x80, 0x1f, 0x48, 0xfd, 0x78, 0xe1, 0x5e, 0x7e, 0xc2, 0x23, 0xa9, 0x8f,
  0x96, 0x17, 0x2b, 0x4c, 0xd9, 0xb9, 0x5c, 0xa9, 0x33, 0x77, 0xff, 0x15, 0x14, 0x91, 0x61, 0x43, 0xac, 0xef, 0xd1, 0xd6,
  0x14, 0xba, 0x9e, 0xf7, 0x64, 0xd8, 0x04, 0x65, 0x63, 0x80, 0xdb, 0x29, 0xa8, 0xf4, 0xf3, 0x8b, 0xb4, 0xd4, 0x2e, 0x5f,
  0x7e, 0xb5, 0x16, 0xe5, 0x36, 0x8a, 0x50, 0x9e, 0xbc, 0x57, 0xab, 0x78, 0x18, 0x6d, 0xa3, 0xfa, 0x4f, 0x85, 0x07, 0xdd,
  0x2f, 0x44, 0x75, 0x55, 0x6f, 0x83, 0x52, 0x7f, 0x14, 0xe1, 0x2f, 0x99, 0x20, 0x43, 0x32, 0x41, 0xca, 0x36, 0xcc, 0x37,
  0xb6, 0xb5, 0x54, 0x42, 0x63, 0xf1, 0x7e, 0x31, 0x40, 0xa1, 0xc4, 0xc3, 0x8c, 0xa1, 0xec, 0x04, 0x73, 0xd9, 0xb8, 0xa0,
  0xe8, 0xbc, 0x78, 0x41, 0x11, 0xcd, 0xe3, 0x23, 0x79, 0xf6, 0xb6, 0x2b, 0x3e, 0x2c, 0xed, 0x8a, 0xbb, 0x6e, 0x5d, 0x3b,
  0xcf, 0x0a, 0xcd, 0xef, 0x2d, 0xfa, 0x94, 0xc7, 0x4d, 0xf2, 0x51, 0x1c, 0x47, 0xdd, 0x14, 0xa4, 0x15, 0xf6, 0x73, 0x4b,
  0x19, 0x33, 0x71, 0x7f, 0x14, 0x82, 0x56, 0xe4, 0x8f, 0x23, 0xaa, 0xfa, 0x78, 0x17, 0x9c, 0xb8, 0xac, 0xed, 0x68, 0x07,
  0xcf, 0x28, 0xe2, 0xdf, 0x69, 0x36, 0x0b, 0x87, 0x0f, 0xfe, 0x3f, 0x40, 0x2d, 0xc1, 0x7a, 0x04, 0xb1, 0x00, 0x00,
};

static void sendIndexPage(void)
{
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, PSTR("text/html"), (PGM_P)INDEX_HTML_GZ, sizeof(INDEX_HTML_GZ));
}
//...................................................................
// End of file
// Firmware Version: V3.2.1
