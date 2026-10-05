//
// RadioClock_JJY40_StandardsChecked.ino
//
// Firmware version: V2.6
//
// JJY validation baseline:
//   NICT "The Method of Emitting Standard Time and Frequency Signal Emission"
//   https://jjy.nict.go.jp/jjy/trans/index-e.html
//
// Important implementation choice:
//   - ESP32 LEDC = hardware 40 kHz carrier
//   - ESP32 hardware timer = 1 kHz deterministic 0.1 ms timebase
//   - JJY envelope = 0.2/0.5/0.8 s, generated from the hardware-timer tick
//   - No software GPIO bit-banging of the 40 kHz carrier

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
#include <ArduinoJson.h>    // JSON parsing and generation
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_timer.h"
#include <math.h>

// Global Objects
WebServer server(80);     // Web server on port 80

// Configuration Constants
#define DEVICENAME_PREFIX "RadioStation"     // Device name prefix for WiFi AP mode
#define FIRMWARE_VERSION "V2.6"

#define DEFAULT_TZ_NAME "Asia/Tokyo"
#define DEFAULT_TZ (9 * 60 * 60)                 // Legacy fallback
#define CONFIG_FILE "/config.json"           // WiFi and timezone configuration file
#define STATION_CONFIG_FILE "/stations.json" // Station configuration file

// WiFi Configuration Variables
char ssid[64] = "";                          // WiFi SSID (network name)
char passwd[64] = "";                        // WiFi password
long timezone_offset = DEFAULT_TZ;               // Legacy numeric offset
String timezone_name = DEFAULT_TZ_NAME;           // IANA timezone name
int transmission_offset_minutes = 0;           // Extra offset (minutes) applied on top of the selected time zone's local time; 0 = exactly that zone's time
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
  "BSF (77.5KHz)",
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

// Legacy station divider values retained only for compatibility with older
// configuration/status data. RF frequency is now generated by LEDC hardware.
int st_cycle2[] = {80, 120, 120, 155, 155, 120, 137};

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
int last_station = -1;                        // Last selected station (prevents redundant switches)

//...................................................................
// Timer and Interrupt Configuration

// Hardware Timer Settings
int tm0cycle;   // Timer 0 cycle count for current station frequency
#define TM0RES    (1) // Legacy value retained for compatibility; RF no longer uses it.

// Radio and Buzzer Modulation Dividers
int radiodiv;   // Divider for radio carrier frequency generation

// Time Base Constants
// Timer interrupt rate depends on frequency: 1 KHz base frequency
// AMPDIV: Amplitude update frequency (10 Hz)
// SSECDIV: Second tick frequency (1 Hz)
#define AMPDIV   (100)  // 1 KHz / 100 => 10 Hz (amplitude update every 0.1 seconds)
#define SSECDIV   (10)  // 10 Hz / 10 => 1 Hz (clock tick every 1 second)

// Bit Symbol Definitions for Time Code Encoding
#define SP_0  (0)  // Binary 0 symbol
#define SP_1  (1)  // Binary 1 symbol
#define SP_M  (2)  // Minute marker symbol
#define SP_P0 (SP_1) // MSF parity 0 (= 1)
#define SP_P1 (3)    // MSF parity 1
#define SP_2  (2)    // BSF/BPC: symbol 2
#define SP_3  (3)    // BSF/BPC: symbol 3
#define SP_M4 (4)    // BSF/BPC: minute marker 4
#define SP_MAX  (SP_M4) // Maximum symbol value

// NOTE: Symbol patterns are defined in station-specific arrays:
// bits_STATION[] => bits60[]: 60-second symbol buffer with station-specific patterns
// sp_STATION[] => secpattern[]: 0.1-second pattern for each symbol (10 patterns per symbol)
// JJY & WWVB Time Code Patterns
// NOTE: Comments describe JJY format (WWVB uses similar structure)
int8_t bits_jjy[] = {  // 60-bit transmission frame: {SP_0, SP_1, SP_M} symbols
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // (M), Minutes[3], 0, Minutes[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, 0, Hours[2], 0, Hours[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, 0, Day_of_Year[2], Day_of_Year[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // Day_of_Year[4], 0, 0, Parity1, Parity2, 0, (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, Year[8], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M  // Day_of_Week[3], LeapSec1, LeapSec2, 0, 0, 0, 0, (M)
};

// WWVB DST state is carried in the station-specific control symbols.

// JJY Amplitude Modulation Pattern (SP_x defines which amplitude for each 0.1-second subframe)
int8_t sp_jjy[] = {
  1, 1, 1, 1, 1, 1, 1, 1, 0, 0,   // SP_0: 80% modulation (8 ticks high, 2 low)
  1, 1, 1, 1, 1, 0, 0, 0, 0, 0,   // SP_1: 50% modulation (5 ticks high, 5 low)
  1, 1, 0, 0, 0, 0, 0, 0, 0, 0    // SP_M: Minute marker 20% (2 ticks high, 8 low)
};

// WWVB Amplitude Modulation Pattern (inverted compared to JJY)
int8_t sp_wwvb[] = {
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0: 80% modulation
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,   // SP_1: 50% modulation
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1    // SP_M: Minute marker 20%
};

// DCF77 Time Code Patterns (Germany)
// NOTE: Encoding is LSB->MSB. Bit [0] changes based on DCF/HBG time zone
int8_t bits_dcf[] = {
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // 0, Reserved[9]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_1, SP_0, // Reserved[5], 0, 0, Time_Zone, 0
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // 1, Minutes[4], Minutes[3], Parity1
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // Hours[4], Hours[2], Parity2, Day[4]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // Day[2], Day_of_Week[3], Month[4], Month[1]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M  // Year[4], Year[4], Parity3, (Minute Marker)
};

// DCF77 Amplitude Modulation Pattern
int8_t sp_dcf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0: 100ms on, 900ms off
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1: 200ms on, 800ms off
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1    // SP_M: Full amplitude (1000ms on)
};

// BSF Time Code Patterns (Taiwan) - Quad encoding
int8_t bits_bsf[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4,
  SP_1,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // 1, Minutes[3], Hours[2.5], Parity1[.5]
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4  // Day_of_Month[2.5], DOW[2.5], Month[2], Year[3.5], Parity2[.5]
};

// BSF Amplitude Modulation Patterns (5 patterns for quad encoding)
int8_t sp_bsf[] = {
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0: Low
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,   // SP_1: Lower-mid
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1,   // SP_2: Mid
  0, 0, 0, 0, 0, 0, 1, 1, 1, 1,   // SP_3: Upper-mid
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1    // SP_M4: Minute marker (full amplitude)
};

// MSF Time Code Patterns (UK) - 4 patterns
int8_t bits_msf[] = {
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_0
};

// MSF Amplitude Modulation Patterns (4 patterns: SP_0, SP_1/SP_P0, SP_M, SP_P1)
int8_t sp_msf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0: Low
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1/SP_P0: Mid (parity 0)
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,   // SP_M: Minute marker
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1    // SP_P1: High (parity 1)
};

// BPC Time Code Patterns (China) - Quad with 5 patterns
int8_t bits_bpc[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // (B), Parity1, Parity2, Hours[2], Minutes[3], DOW[2]
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // Parity3, Day[3], Month[2], Year[3], Parity4
  SP_M4, SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P1: 0, 1, 2 for 00-19, -39, -59 seconds
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P2: 0, P3: AM/PM(0/2)+parity<hmDOW>
  SP_M4, SP_2, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P4: parity<DMY> (0/1)
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0
};

// BPC Amplitude Modulation Patterns (5 patterns)
int8_t sp_bpc[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0: Low
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1: Lower-mid
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1,   // SP_2: Mid
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,   // SP_3: Upper-mid
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0    // SP_M4: Minute marker (no modulation)
};

// Legacy 60-second WWVB symbol buffer retained for diagnostics.
int8_t bits_wwvb[60] = {};

//...................................................................
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

// Dedicated high-priority FreeRTOS task that performs all time-critical
// radio signal generation, woken directly by the hardware timer ISR.
void radioTask(void *pvParameters);

// Applies the configured "Time zone" card selection to the C library as a
// POSIX TZ rule (see posixTzFor()/applyTimezone() below), used by both the
// displayed clock and the transmitted time code.
static void applyTimezone(void);

// WiFi power management (Scheduled/power-save mode) - see definitions near
// checkWiFiConnection() for full explanation.
bool shouldWifiBeOnForSchedule(void);
void updateWifiPowerManagement(void);

// Debounced config persistence (flash-wear mitigation) - see definition
// near saveConfig()/writeConfigNow().
void writeConfigNow(void);

// Legacy frame pointers retained for compatibility with the original scheduler.
int8_t *st_bits[] = {bits_jjy, bits_jjy, bits_wwvb, bits_dcf, bits_bsf, bits_msf, bits_bpc};
int8_t *bits60;  // Pointer to current station's 60-bit pattern

// Amplitude modulation pattern lookup table: maps station index to its modulation pattern
int8_t *st_sp[]   = {sp_jjy, sp_jjy, sp_wwvb, sp_dcf, sp_bsf, sp_msf, sp_bpc};
int8_t *secpattern;  // Legacy pointer retained for compatibility
uint8_t txEnvelope[60][10]; // 0=carrier off, 1=full carrier, 2=reduced carrier
uint8_t txSymbol[60];
uint32_t carrierFrequencyHz = 40000;
bool carrierReady = false;

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
// The ISR notifies a dedicated, high-priority FreeRTOS task using a counting
// task notification. This is used (rather than the previous binary semaphore
// + manual "buzzup" counter) because a binary semaphore can only ever signal
// "at least one tick is pending" - if the servicing task were ever delayed
// past 1 ms, additional ISR ticks would be collapsed into a single pending
// flag and the envelope timing would drift. A counting task notification
// preserves the exact number of 1 ms ticks that occurred, so the radio task
// can always catch up deterministically and losslessly.
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

int radioc = 0;  // Radio carrier phase counter: 0..(tm0cycle - 1)
int ampc = 0;    // Amplitude cycle counter: 0..(AMPDIV - 1), generates 10 Hz updates
int tssec = 0;   // Time base counter: 0..(SSECDIV - 1), generates 1 Hz clock ticks

//...................................................................
// Network Time and Synchronization

int ntpsync = 1;  // NTP synchronization flag: 1 = synchronized, 0 = not synchronized
bool full_time_tx = false; // Continuous transmission mode
int full_time_station = SN_JJY_E; // Radio type used in full-time mode
time_t now;       // Current UNIX timestamp (seconds since epoch)
struct tm nowtm;
volatile bool ntpSyncEvent = false;

// Set by the web-server task (loop()/HTTP handlers) whenever a change is
// made that affects what is encoded in the radio signal (transmission
// offset, full-time mode/station, timezone). The actual regeneration of
// txSymbol[]/txEnvelope[] is always performed by radioTask, never by the
// web handler itself, because those arrays are also read every 1 ms by
// ampchange() in radioTask - having two tasks write them concurrently could
// tear a read mid-update. radioTask checks this flag on every 1 kHz tick,
// so the encoding (and therefore the UI, which polls /api/status) updates
// within at most ~1 ms of the change being made.
volatile bool encodingRefreshRequested = false;
uint64_t ntpLastMonoUs = 0;
int64_t ntpLastEpochUs = 0;
double ntpDriftPpm = 0.0;
double ntpDriftAbsSecPerHour = 0.0;
uint32_t ntpIntervalSec = 3600;
#define NTP_INITIAL_INTERVAL_SEC 3600UL
#define NTP_MIN_INTERVAL_SEC 900UL
#define NTP_MAX_INTERVAL_SEC 86400UL
#define NTP_ERROR_BUDGET_SEC 0.90
#define NTP_DRIFT_EMA_ALPHA 0.35

//...................................................................
// Output Control Variables

int radioout = 0,  // Current radio output pin value (0 or 1)
    buzzout = 0;   // Current buzzer output pin value (0 or 1)

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
  // latency and timing jitter. vTaskNotifyGiveFromISR increments a counter
  // on the target task rather than just setting a flag, so no ticks are
  // ever lost even if the radio task is briefly delayed.
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
  Serial.printf("started... (firmware %s)\n", FIRMWARE_VERSION);
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
  digitalWrite(PIN_BUZZ, buzzout);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // Start the dedicated radio-timing task BEFORE the hardware timer/LEDC
  // carrier are enabled (applyCurrentSchedule() -> setstation() ->
  // starttimer() below arms the 1 kHz interrupt), so radioTaskHandle is
  // valid the instant the first tick can arrive.
  //
  // Priority is set above the Arduino loopTask (which runs the web server,
  // Serial, and WiFi housekeeping) and the task is pinned to the same core
  // the loopTask normally runs on (APP CPU / core 1) so it always preempts
  // web/Serial work instead of competing with it on the other core, which
  // would leave the housekeeping tasks (WiFi driver, SNTP) undisturbed.
  xTaskCreatePinnedToCore(radioTask, "radioTask", 4096, NULL,
                           RADIO_TASK_PRIORITY, &radioTaskHandle, 1);

  applyCurrentSchedule();
  ampchange();
  Serial.print("radio started.\n");
}

// Dedicated, high-priority task that performs ALL time-critical radio
// signal work. It is woken directly by the hardware timer ISR via a
// counting task notification, so its scheduling latency is bounded only by
// FreeRTOS's own interrupt-to-task wake latency (typically low single-digit
// microseconds on ESP32) and is completely unaffected by however long the
// web server, Serial output, or WiFi housekeeping in loop() takes to run.
//
// No delay() of any kind appears in this task; it blocks only on the
// hardware-timer-driven notification, so every tick it processes is exactly
// 1 ms of real elapsed time as measured by the hardware timer.
void radioTask(void *pvParameters)
{
  for (;;) {
    // Block until the ISR notifies us. ulTaskNotifyTake(pdTRUE, ...) clears
    // the notification value on return and yields the exact number of 1 ms
    // ticks that occurred since the last time we checked, so ticks can never
    // be silently dropped even if this task is briefly delayed.
    uint32_t pendingTicks = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Service any pending "encoding changed" request from the web UI before
    // processing ticks, so a change (e.g. a new transmission offset)
    // takes effect on the very next 1 ms tick rather than waiting for the
    // next minute boundary.
    if (encodingRefreshRequested) {
      encodingRefreshRequested = false;
      getlocaltime();
      applyCurrentSchedule();
      if (makebitpattern) {
        makebitpattern();
        printbits60();
        if (last_station != SN_BPC) {
          printPatternRepeatInfo(last_station);
        }
      }
      ampchange();
    }

    while (pendingTicks-- > 0) {
      // 1 kHz buzzer toggle.
      if (!buzzout && ampmod && buzzsw) {
        buzzout = 1;
        digitalWrite(PIN_BUZZ, HIGH);
      } else {
        buzzout = 0;
        digitalWrite(PIN_BUZZ, LOW);
      }

      ampc++;
      if (AMPDIV <= ampc) {
        ampc = 0;
        tssec++;
        if (SSECDIV <= tssec) { // 1 second action --v
          tssec = 0;
          int lastmin = nowtm.tm_min;
          getlocaltime();

          // Apply current schedule BEFORE making bit pattern
          // This ensures the correct station is selected before encoding
          applyCurrentSchedule();

          if (lastmin != nowtm.tm_min) {
            // Generate a new 60-second time-code frame at the start of each minute.
            last_rotation_time = millis();
            makebitpattern();
            printbits60();
            if (last_station != SN_BPC) {
              printPatternRepeatInfo(last_station);
            }
          }
          // BPC repeats a 20-second block three times per minute.
          if (last_station == SN_BPC && (nowtm.tm_sec % 20) == 0) {
            printPatternRepeatInfo(last_station);
          }
          else {
            // Every second, check if we need to apply a new schedule (for rotation)
            // Only regenerate pattern if station changes
            if (current_schedule_index != -1 && applicable_count > 1) {
              makebitpattern();  // Update pattern every second when rotating
            }
          }

          // Single combined line: date/time, schedule state, station,
          // exact field being transmitted, and second-in-frame - all in
          // one Serial.printf() call so each second produces one line.
          {
            int frameSeconds = (last_station == SN_BPC) ? 20 : 60;
            Serial.printf(
              "%d-%d-%d, %02d:%02d:%02d (sched=%d/%d) | TX: %s | %s | second=%02d/%02d\n",
              nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday,
              nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec,
              current_schedule_index + 1, applicable_count,
              station_names[last_station],
              txFieldDescription(last_station, nowtm.tm_sec),
              nowtm.tm_sec, frameSeconds);
          }
        }
        ampchange();
      }
    }
  }
}

// loop() now only handles non-time-critical housekeeping: WiFi supervision,
// SNTP bookkeeping, and the web server. It contains no delay() calls and
// never touches the radio envelope directly - all of that lives in
// radioTask(), driven purely by the hardware timer.
void loop() {
  // Check WiFi connection periodically
  if (ap_mode == false && (millis() - last_wifi_check) > WIFI_CONNECT_CHECK_INTERVAL) {
    last_wifi_check = millis();
    checkWiFiConnection();
  }

  if (ntpSyncEvent) {
    ntpSyncEvent = false;
    configureAdaptiveNtp();
  }

  // WiFi power management: in Scheduled/power-save mode, turns the radio
  // off between sync windows. Safe to call every loop() pass - it
  // internally rate-limits itself.
  updateWifiPowerManagement();

  // Flush any pending config change to flash once things have settled
  // (see saveConfig()/writeConfigNow() for why this is debounced).
  if (configDirty && (millis() - configDirtyBecause) >= CONFIG_SAVE_DEBOUNCE_MS) {
    configDirty = false;
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
  bool wifiUp = (!ap_mode) && (WiFi.status() == WL_CONNECTED);
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
  if (ap_mode || WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }

  yield();// feed watchdog
}


//...................................................................
void starttimer(void)
{
  if (istimerstarted) stoptimer();

  ampc = 0;
  radioc = 0;
  tssec = 0;

  // Clear any stale notification count left over from before the timer was
  // stopped, so the radio task doesn't "catch up" on ticks that never
  // actually happened in hardware.
  if (radioTaskHandle != NULL) {
    xTaskNotifyStateClear(radioTaskHandle);
    ulTaskNotifyValueClear(radioTaskHandle, 0xFFFFFFFF);
  }

  portDISABLE_INTERRUPTS();
  // Use a 1 MHz hardware-timer tick and generate the 1 kHz service
  // interrupt with a 1000-tick alarm.  A direct 1 kHz timer clock would
  // require an 80,000 divider on an 80 MHz APB clock, exceeding the
  // ESP32 hardware timer divider range.
  tm0 = timerBegin(1000000);
  if (tm0 == NULL) {
    portENABLE_INTERRUPTS();
    Serial.println("ERROR: Failed to create 1 MHz hardware timer");
    return;
  }
  timerAttachInterrupt(tm0, &onTimer);
  timerAlarm(tm0, 1000, true, 0);
  portENABLE_INTERRUPTS();

  istimerstarted = 1;
}

void stoptimer(void)
{
  if (tm0 != NULL) {
    portDISABLE_INTERRUPTS();
    timerDetachInterrupt(tm0);
    timerEnd(tm0);
    tm0 = NULL;
    portENABLE_INTERRUPTS();
  }

  if (carrierReady) ledcWrite(PIN_RADIO, 0);
  istimerstarted = 0;
}

// setup amplitude value ampmod depends on bit pattern & 0.1 second frame
void ampchange(void)
{
  if (!carrierReady) return;

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
  ampmod = duty != 0;
  digitalWrite(PIN_LED, ampmod ? HIGH : LOW);
}


const char* stationEncodingName(int station)
{
  switch (station) {
    case SN_JJY_E:
    case SN_JJY_W: return "JJY pulse-width AM";
    case SN_WWVB:  return "WWVB pulse-width AM";
    case SN_DCF77: return "DCF77 reduced-carrier AM";
    case SN_BSF:   return "BSF";
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

void setstation(int station)
{
  if (station < 0 || station >= NUM_STATIONS) return;

  stoptimer();

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
    carrierReady = false;
    Serial.printf("ERROR: LEDC could not configure %lu Hz\\n",
                  (unsigned long)carrierFrequencyHz);
    return;
  }

  ledcWrite(PIN_RADIO, 0);
  last_station = station;
  bits60 = st_bits[station];
  secpattern = st_sp[station];
  makebitpattern = st_makebits[station];
  makebitpattern();
  starttimer();
  ampchange();

  Serial.printf("TX START: %s | encoding=%s | frequency=%lu Hz (%.3f kHz)\\n",
                station_names[station], stationEncodingName(station),
                (unsigned long)carrierFrequencyHz, carrierFrequencyHz / 1000.0);
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
static void stationTime(time_t utc, int station, bool nextMinute, struct tm &out)
{
  (void)station;  // encoding format no longer changes which time is used
  time_t tx = utc + (nextMinute ? 60 : 0);
  tx += (time_t)transmission_offset_minutes * 60;
  localtime_r(&tx, &out);  // honors the POSIX TZ rule set by applyTimezone(), including DST
}

static void clearTxFrame(void)
{
  memset(txSymbol, 0, sizeof(txSymbol));
  memset(txEnvelope, 0, sizeof(txEnvelope));
}

static void buildJjyEnvelope(void)
{
  for (int s = 0; s < 60; ++s) {
    int marker = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59);
    int highTenths = marker ? 2 : (txSymbol[s] ? 5 : 8);
    for (int k = 0; k < 10; ++k)
      txEnvelope[s][k] = (k < highTenths) ? 1 : 2;
  }
}

static void buildWwvbEnvelope(void)
{
  for (int s = 0; s < 60; ++s) {
    int marker = (s == 0 || s == 9 || s == 19 || s == 29 || s == 39 || s == 49 || s == 59);
    int lowTenths = marker ? 8 : (txSymbol[s] ? 5 : 2);
    for (int k = 0; k < 10; ++k)
      txEnvelope[s][k] = (k < lowTenths) ? 2 : 1;
  }
}

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
  //   Minute: 1..7, PA2: 36
  //   Hour:   12..18, PA1: 19
  //   Day-of-year: 22..30
  //   Year: 41..48
  //   Weekday: 50..52
  //   LS1/LS2: 53/54
  //   SU1/SU2: 40/20
  //   ST1..ST6: 55..58
  //
  // This encoder intentionally transmits the ordinary time-code frame.
  // The special JJY call-sign interval at :15 and :45 is handled separately.
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

// write binary value into bit pattern (little endian)
void
binarize(int v, int pos, int len)
{
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    txSymbol[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// continuous (over 4 bit) BCD to write
void
bcdize(int v, int pos, int len)
{
  int l;

  pos = pos + len - 1;
  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    binarize(v % 10, pos - l + 1, l);
    v = v / 10;
    pos = pos - l;
    len = len - l;
  }
  return;
}

// LSB->MSB (big endian) binarize
void
rbinarize(int v, int pos, int len)
{
  for ( ; 0 < len; pos++, len--) {
    txSymbol[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// LSB->MSB BCDize
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


// 4-ary encoding (little endian)
void
quadize(int v, int pos, int len)
{
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    txSymbol[pos] = (uint8_t)(v & 3);
    v >>= 2;
  }
  return;
}

// calculate even parity
int
parity(int pos, int len)
{
  int s = 0;
  
  for (pos; 0 < len; pos++, len--) {
    s += txSymbol[pos];
  }
  return (s % 2);
}

// binary parity for 4-ary data (for BSF/BPC): is it OK?
int
qparity(int pos, int len)
{
  int s = 0;
  
  for (pos; 0 < len; pos++, len--) {
    s += (txSymbol[pos] & 1) + ((txSymbol[pos] & 2) >> 1);
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

      ntpDriftPpm = (ntpDriftPpm == 0.0)
        ? ppm
        : ntpDriftPpm * (1.0 - NTP_DRIFT_EMA_ALPHA) + ppm * NTP_DRIFT_EMA_ALPHA;
      ntpDriftAbsSecPerHour = fabs(ntpDriftPpm) * 3600.0 / 1000000.0;
    }
  }

  ntpLastMonoUs = mono;
  ntpLastEpochUs = epochUs;
  ntpSyncEvent = true;
}

void configureAdaptiveNtp(void)
{
  double drift = fabs(ntpDriftPpm) / 1000000.0;
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
  Serial.print("Attempting to connect to Network named: ");
  Serial.println(ssid);
  WiFi.begin(ssid, passwd);

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
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  // NOTE: Arduino-ESP32's configTime(gmtOffset_sec, daylightOffset_sec, ...)
  // has a side effect most people don't expect: it builds its own TZ string
  // from the two offset arguments and calls setenv("TZ", ...)/tzset()
  // internally. Since both offsets are 0 here (this firmware doesn't use
  // configTime()'s own offset handling at all - it manages the zone/DST via
  // applyTimezone()'s POSIX TZ strings instead), that internal call would
  // silently reset TZ to plain UTC0, undoing whatever zone the user
  // actually selected. A previous revision called applyTimezone() BEFORE
  // this line, so every boot (and every reconnect, since checkWiFiConnection
  // calls ntpstart() again) would quietly revert the selected time zone back
  // to UTC the moment WiFi came up - e.g. Tokyo (UTC+9) would then transmit
  // and display as if it were plain UTC. Calling applyTimezone() again here,
  // AFTER configTime(), ensures the user's actual selection is what's left
  // in effect once NTP setup completes.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (timezone_name.length() > 0) {
    applyTimezone();
  }
  sntp_set_sync_interval(NTP_INITIAL_INTERVAL_SEC * 1000UL);

  for (int i = 0; i < 20 && !getLocalTime(&nowtm, 1000); ++i) {
    Serial.print(".");
  }

  if (getLocalTime(&nowtm)) {
    ntpsync = 1;
    Serial.println("\nNTP synchronized");
  } else {
    ntpsync = 0;
    Serial.println("\nNTP synchronization failed");
  }
}

void
ntpstop(void)
{
  ntpsync = 0;
  esp_sntp_stop();
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
    timezone_name = DEFAULT_TZ_NAME; timezone_offset = DEFAULT_TZ;
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
  File configFile = LittleFS.open(CONFIG_FILE, "w");
  if (!configFile) { Serial.println("Failed to open config file for writing"); return; }
  configFile.println(ssid);
  configFile.println(passwd);
  configFile.println(timezone_name);
  configFile.println(full_time_tx ? "1" : "0");
  configFile.println(full_time_station);
  configFile.println(transmission_offset_minutes);
  configFile.println(wifiPowerMode);
  configFile.close();
  Serial.println("Config saved");
}

void saveConfig(void)
{
  configDirty = true;
  configDirtyBecause = millis();
}

void loadSchedules(void)
{
  schedule_count = 0;

  File schedFile = LittleFS.open(STATION_CONFIG_FILE, "r");
  if (!schedFile) {
    Serial.println("No schedule file found, using default");
    schedules[0].station   = SN_JJY_E;
    schedules[0].start_min = 0;
    schedules[0].end_min   = 1439;
    schedule_count = 1;
    return;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, schedFile);
  schedFile.close();

  if (err || !doc.is<JsonArray>()) {
    Serial.println("Schedule JSON invalid, using default");
    goto DEFAULT_SCHEDULE;
  }
{
  JsonArray arr = doc.as<JsonArray>();

  /* ---------- Iterate through the schedule array ---------- */
  for (JsonObject obj : arr) {
    /* Normal scheduled transmission entry */
    if (!obj.containsKey("station") ||
        !obj.containsKey("start") ||
        !obj.containsKey("end")) {
      continue;
    }

    if (schedule_count >= MAX_SCHEDULES)
      break;

    schedules[schedule_count].station   = obj["station"].as<int>();
    schedules[schedule_count].start_min = obj["start"].as<int>();
    schedules[schedule_count].end_min   = obj["end"].as<int>();

    schedule_count++;
  }

  if (schedule_count == 0) {
    goto DEFAULT_SCHEDULE;
  }

  Serial.printf(
    "Loaded %d schedules\n",
    schedule_count
  );
  return;
}
DEFAULT_SCHEDULE:
  schedules[0].station   = SN_JJY_E;
  schedules[0].start_min = 0;
  schedules[0].end_min   = 1439;
  schedule_count = 1;
}

void saveSchedules(void)
{
  File schedFile = LittleFS.open(STATION_CONFIG_FILE, "w");
  if (!schedFile) {
    Serial.println("Failed to open schedule file for writing");
    return;
  }
  
  // Write JSON format
  schedFile.print("[");
  for (int i = 0; i < schedule_count; i++) {
    if (i > 0) schedFile.print(",");
    schedFile.printf("{\"station\":%d,\"start\":%d,\"end\":%d}",
                     schedules[i].station,
                     schedules[i].start_min,
                     schedules[i].end_min);
  }
  schedFile.print("]");
  schedFile.close();
  Serial.println("Schedules saved");
  applyCurrentSchedule();
}

void applyCurrentSchedule(void)
{
  // Full-time mode ignores schedules, but continues transmitting the
  // selected station's normal clock/time-code signal 24/7.
  if (full_time_tx) {
    applicable_count = 0;
    current_schedule_index = -1;
    if (last_station != full_time_station) {
      last_station = full_time_station;
      setstation(full_time_station);
    }
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
  
  int new_station = SN_JJY_E;  // Default
  
  if (applicable_count == 0) {
    // No applicable schedule, use default
    current_schedule_index = -1;
  } else if (applicable_count == 1) {
    // Single schedule, use it
    current_schedule_index = 0;
    new_station = schedules[applicable_schedules[0]].station;
  } else {
    // Multiple schedules - rotate through them
    unsigned long now = millis();
    if (current_schedule_index == -1) {
      current_schedule_index = 0;
      last_rotation_time = now;
      Serial.printf("Start rotation: %d schedules available\n", applicable_count);
    }
      // Serial.printf("now: %lu\n", now);
      // Serial.printf("last_rotation_time: %lu\n", last_rotation_time);
    // Check if it's time to rotate to next schedule
    if ((now - last_rotation_time) >= (ROTATION_INTERVAL_MINUTES * 60000UL)) {
      current_schedule_index++;
      Serial.print("current_schedule_index:");
      Serial.println(current_schedule_index);

      Serial.print("applicable_count:");
      Serial.println(applicable_count);

      if (current_schedule_index >= applicable_count) {
        current_schedule_index = 0;
      }
      last_rotation_time = now;
      Serial.printf("Rotating to schedule %d of %d (station=%d)\n", 
                    current_schedule_index + 1, applicable_count,
                    schedules[applicable_schedules[current_schedule_index]].station);
    }
    
    new_station = schedules[applicable_schedules[current_schedule_index]].station;
  }
  
  // Change station if needed
  if (new_station != last_station) {
    last_station = new_station;
    setstation(new_station);
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
// once the earlier configTime()-clobber bug was fixed, since nowtm and the
// schedule matching in applyCurrentSchedule() both key off the same
// zone-aware local clock. The wake-window check below uses that same clock.
bool shouldWifiBeOnForSchedule(void)
{
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

  int current_min = nowtm.tm_hour * 60 + nowtm.tm_min;
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
  if (ap_mode) return;
  if (wifiPowerMode != WIFI_POWER_SCHEDULED) return;

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
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
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

  // Check if WiFi is still connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost, attempting to reconnect...");
    WiFi.reconnect();
  }
  
  // If connection takes too long, switch to AP mode
  if (WiFi.status() != WL_CONNECTED && 
      (millis() - wifi_connect_start) > WIFI_CONNECT_TIMEOUT) {
    Serial.println("WiFi connection timeout, starting AP mode...");
    startAPMode();
  }
}

void startAPMode(void)
{
  stoptimer();  // Stop radio while configuring
  ap_mode = true;
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
  WiFi.softAPdisconnect(true);
  // Keep web server running in STA mode
  WiFi.mode(WIFI_STA);
  starttimer();  // Resume radio
  Serial.println("AP mode stopped, web server continues on WiFi");
}

void initWebServer(void)
{
  // Serve index page
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getIndexHTML());
  });
  
  // API endpoints
  server.on("/api/config", HTTP_GET, []() {
    String json = "{\"ssid\":\"" + String(ssid) + "\",\"timezone\":\"" +
                  timezone_name + "\",\"full_time_tx\":" +
                  String(full_time_tx ? "true" : "false") + ",\"full_time_station\":" + String(full_time_station) +
                  ",\"transmission_offset_minutes\":" + String(transmission_offset_minutes) +
                  ",\"wifi_power_mode\":" + String(wifiPowerMode) + "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/config", HTTP_POST, []() {
    // Settings can be updated independently. Full-time TX therefore takes
    // effect immediately without requiring the Save settings button.
    bool wifiChanged = false;
    bool encodingChanged = false;

    if (server.hasArg("ssid")) {
      server.arg("ssid").toCharArray(ssid, sizeof(ssid));
      wifiChanged = true;
    }
    if (server.hasArg("password")) {
      server.arg("password").toCharArray(passwd, sizeof(passwd));
      wifiChanged = true;
    }
    if (server.hasArg("timezone")) {
      timezone_name = server.arg("timezone");
      applyTimezone();
      encodingChanged = true;
    }
    if (server.hasArg("full_time_tx")) {
      String v = server.arg("full_time_tx");
      full_time_tx = (v == "1" || v.equalsIgnoreCase("true"));
      encodingChanged = true;
    }
    if (server.hasArg("full_time_station")) {
      full_time_station = server.arg("full_time_station").toInt();
      if (full_time_station < SN_JJY_E || full_time_station > SN_BPC)
        full_time_station = SN_JJY_E;
      encodingChanged = true;
    }
    if (server.hasArg("transmission_offset_minutes")) {
      transmission_offset_minutes = server.arg("transmission_offset_minutes").toInt();
      if (transmission_offset_minutes < -720 || transmission_offset_minutes > 840)
        transmission_offset_minutes = 0;
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

    if (encodingChanged) {
      // Hand the actual regeneration off to radioTask (see
      // encodingRefreshRequested above) rather than touching
      // txSymbol[]/txEnvelope[] here, since this handler runs on the web
      // server task and those arrays are read by radioTask every 1 ms.
      // radioTask picks this up on its very next tick, so the transmitted
      // signal (and therefore the UI, which polls /api/status once a
      // second) reflects the change essentially immediately.
      encodingRefreshRequested = true;
    } else {
      // Nothing encoding-related changed, but a schedule/station switch may
      // still be needed the ordinary way (e.g. re-evaluate full_time_tx
      // toggling off with no other field changed is covered above; this
      // branch only runs when only WiFi credentials were posted).
      applyCurrentSchedule();
    }
    saveConfig();

    server.send(200, "application/json",
                String("{\"status\":\"ok\",\"full_time_tx\":") +
                (full_time_tx ? "true" : "false") +
                ",\"station\":" + String(last_station) + "}");

    // Only reconnect when Wi-Fi credentials were changed.
    if (wifiChanged) {
      Serial.printf("Connecting to WiFi: %s\\n", ssid);
      WiFi.mode(WIFI_STA);
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
  
  server.on("/api/schedule", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      // Parse JSON and add single schedule
      String json = server.arg("plain");
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, json);
      
      if (error) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid json\"}");
        return;
      }
      
      if (schedule_count < MAX_SCHEDULES) {
        schedules[schedule_count].station = doc["station"];
        schedules[schedule_count].start_min = doc["start"];
        schedules[schedule_count].end_min = doc["end"];
        schedule_count++;
        saveSchedules();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"max schedules reached\"}");
      }
    } else {
      server.send(400, "application/json", "{\"error\":\"missing data\"}");
    }
  });
  
  server.on("/api/schedules", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      // Parse JSON and update schedules
      String json = server.arg("plain");
      StaticJsonDocument<2000> doc;
      DeserializationError error = deserializeJson(doc, json);
      
      if (error) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid json\"}");
        return;
      }
      
      schedule_count = 0;
      for (JsonObject item : doc.as<JsonArray>()) {
        if (schedule_count < MAX_SCHEDULES) {
          schedules[schedule_count].station = item["station"];
          schedules[schedule_count].start_min = item["start"];
          schedules[schedule_count].end_min = item["end"];
          schedule_count++;
        }
      }
      saveSchedules();
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing data\"}");
    }
  });
  
  server.on("/api/schedule", HTTP_DELETE, []() {
    if (server.hasArg("index")) {
      int idx = server.arg("index").toInt();
      if (idx >= 0 && idx < schedule_count) {
        // Remove schedule at index
        for (int i = idx; i < schedule_count - 1; i++) {
          schedules[i] = schedules[i + 1];
        }
        schedule_count--;
        saveSchedules();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid index\"}");
      }
    } else {
      server.send(400, "application/json", "{\"error\":\"missing index\"}");
    }
  });
  
  server.on("/api/status", HTTP_GET, []() {
    // Build response with rotation information
    // Use the ESP32's configured IANA timezone for the displayed local time.
    getlocaltime();
    char localTime[16];
    char localDate[16];
    strftime(localTime, sizeof(localTime), "%H:%M:%S", &nowtm);
    strftime(localDate, sizeof(localDate), "%Y-%m-%d", &nowtm);

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

    String json = "{\"time\":\"" + String(localTime) + "\",\"date\":\"" + String(localDate) + "\"";
    json += ",\"timezone\":\"" + timezone_name + "\"";
    json += ",\"tx_time\":\"" + String(txTime) + "\"";
    
    // Add applicable schedules count for rotation display
    json += ",\"applicable_schedules\":[";
    for (int i = 0; i < applicable_count; i++) {
      if (i > 0) json += ",";
      json += String(applicable_schedules[i]);
    }
    json += "]";
    
    json += ",\"full_time_tx\":" + String(full_time_tx ? "true" : "false");
  json += ",\"ntp_interval\":" + String(ntpIntervalSec);
  json += ",\"ntp_drift_ppm\":" + String(ntpDriftPpm, 3);
    json += ",\"station\":" + String(last_station);
    json += ",\"tx_next_minute\":" + String(nextMinute ? "true" : "false");
    json += ",\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += "}";
    server.send(200, "application/json", json);
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

  server.begin();
}

String getIndexHTML(void)
{
  String html = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RadioClock Control</title>
<style>
:root{--bg:#f4f7fb;--card:#fff;--text:#172033;--muted:#667085;--line:#e5e9f0;--accent:#2563eb;--ok:#16a34a;--danger:#dc2626}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1050px;margin:auto;padding:24px}.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}
h1{font-size:25px;margin:0 0 3px}.sub,.hint{color:var(--muted);font-size:13px}.clock{font:600 28px ui-monospace,monospace}
.card{background:#fff;border:1px solid var(--line);border-radius:16px;padding:22px;margin-bottom:16px;box-shadow:0 4px 18px rgba(16,24,40,.05)}
h2{font-size:17px;margin:0 0 16px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
label{display:block;font-size:13px;font-weight:650;margin-bottom:7px}input,select{width:100%;padding:11px 12px;border:1px solid #d5dae3;border-radius:9px;background:#fff;font:inherit}
input:focus,select:focus{outline:2px solid #bfdbfe;border-color:var(--accent)}.btn{border:0;border-radius:9px;padding:10px 15px;font-weight:650;cursor:pointer}
.primary{background:var(--accent);color:#fff}.secondary{background:#eef2f7;color:var(--text)}.danger{background:#fee2e2;color:var(--danger);padding:7px 10px}
.actions{display:flex;gap:9px;margin-top:17px;flex-wrap:wrap}.switchrow{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;border:1px solid var(--line);border-radius:12px;background:#fafbfc}.fulltime-control{grid-column:1/-1}.fulltime-station{grid-column:1/-1;margin-top:0}.fulltime-station label{margin-bottom:7px}
.switch{position:relative;width:48px;height:27px;margin:0}.switch input{opacity:0;width:0;height:0}.slider{position:absolute;inset:0;background:#cbd5e1;border-radius:30px;cursor:pointer}
.slider:before{content:"";position:absolute;width:21px;height:21px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}.switch input:checked+.slider{background:var(--accent)}.switch input:checked+.slider:before{transform:translateX(21px)}
.schedule{display:grid;grid-template-columns:1.3fr 1fr 1fr auto;gap:10px;align-items:end;padding:14px 0;border-bottom:1px solid var(--line)}
.badge{display:inline-flex;padding:5px 9px;border-radius:99px;font-size:12px;font-weight:700;background:#e8f0ff;color:#1d4ed8}.badge.off{background:#f1f5f9;color:#64748b}
.status{display:none;padding:11px 13px;border-radius:9px;margin-bottom:14px}.success{display:block;background:#dcfce7;color:#166534}.error{display:block;background:#fee2e2;color:#991b1b}
@media(max-width:720px){.wrap{padding:14px}.top{align-items:flex-start;gap:10px}.clock{font-size:20px}.grid{grid-template-columns:1fr}.schedule{grid-template-columns:1fr 1fr}.schedule .station{grid-column:1/-1}}
</style></head><body><div class="wrap">
<div class="top"><div><h1>RadioClock Control</h1><div class="sub">Radio time signal configuration &middot; <span id="fwVersion">V2.6</span></div></div>
<div style="text-align:right"><div class="clock" id="timeDisplay">--:--:--</div><div class="hint" id="dateDisplay">----</div><div style="margin-top:5px"><span class="badge off" id="txBadge">Scheduled</span></div></div></div>
<div id="statusMsg" class="status"></div>

<div class="card"><h2>General</h2><div class="grid">
<div><label>Time zone</label><select id="timezone" onchange="setTimezone()">
<option value="Australia/Brisbane">Brisbane — UTC+10 (no DST)</option>
<option value="Australia/Sydney">Sydney — UTC+10/+11 (DST)</option>
<option value="Asia/Tokyo">Tokyo — UTC+9</option>
<option value="Europe/London">London — UTC/UTC+1 (DST)</option>
<option value="America/New_York">New York — UTC-5/-4 (DST)</option>
<option value="America/Los_Angeles">Los Angeles — UTC-8/-7 (DST)</option>
<option value="UTC">UTC</option></select>
<div class="hint">Sydney daylight saving is handled automatically.</div></div>
<div><label>Transmission offset</label><select id="txOffset" onchange="setTransmissionOffset()"></select><div class="hint">Adds this offset to the selected time zone's time. No offset keeps the encoded time exactly aligned with the Time zone selected above.</div></div>
<div class="switchrow fulltime-control"><div><strong>Full-time transmission</strong><div class="hint">Transmit the selected radio time signal continuously. Schedules are ignored while enabled, while normal time-code modulation remains active.</div></div>
<label class="switch"><input type="checkbox" id="fullTime" onchange="setFullTime()"><span class="slider"></span></label></div>
<div class="fulltime-station"><label>Full-time radio type</label><select id="fullTimeStation" onchange="setFullTimeStation()"></select><div class="hint">This radio type is transmitted 24/7 when Full-time transmission is enabled.</div></div>
</div></div>

<div class="card"><h2>Transmission</h2>
<div class="switchrow"><div><strong id="txStationName">--</strong><div class="hint" id="txEncoding">Encoding: --</div></div><div style="text-align:right"><div style="font:600 22px ui-monospace,monospace" id="txTimeDisplay">--:--:--</div><div class="hint" id="txTimeNote">Time encoded in the current transmission</div></div></div>
<div class="hint" style="margin-top:10px">This is the time currently being encoded by the active radio service: the Time zone selected above, plus the Transmission offset. It is always the same regardless of which radio type is selected - only the bit-level format changes between radio types, never the time itself.</div>
</div>

<div class="card"><h2>Time synchronisation</h2>
<div class="switchrow"><div><strong>NTP</strong><div class="hint">Automatic synchronisation with adaptive update timing based on measured clock drift.</div></div>
<button class="btn secondary" id="ntpButton" onclick="syncNtp()">Sync now</button></div>
</div>

<div class="card"><h2>Transmission schedules</h2><div class="hint">Choose the radio type and transmission period. Overlapping schedules rotate every 5 minutes.</div>
<div id="schedulesList"></div><div class="actions"><button class="btn secondary" onclick="addSchedule()">＋ Add schedule</button>
<button class="btn primary" onclick="saveSchedules()">Save schedules</button></div></div>

<div class="card"><h2>Radio types</h2><div id="stationsList" class="grid"></div></div>

<div class="card"><h2>Wi-Fi</h2>
<div class="switchrow" style="flex-direction:column;align-items:stretch;gap:10px;margin-bottom:16px">
<div><strong>WiFi power mode</strong><div class="hint">Always on keeps WiFi connected continuously. Power-save turns WiFi off except for 5 minutes after every reboot, plus a 10-minute window before each scheduled transmission start (enough time to reconnect and get a fresh NTP sync) - WiFi is deliberately off during the transmission itself. With Full-time transmission enabled, or no schedules configured, it instead wakes briefly every 6 hours to resync. While WiFi is off this page won't be reachable, so use Always on while you're actively configuring the device.</div></div>
<div class="actions"><button class="btn" id="wifiPowerAlwaysBtn" onclick="setWifiPowerMode(0)">Always on</button><button class="btn" id="wifiPowerScheduledBtn" onclick="setWifiPowerMode(1)">Power-save (scheduled)</button></div>
</div>
<div class="grid">
<div><label>Network name (SSID)</label><input id="ssid" type="text"></div>
<div><label>Password</label><input id="password" type="password"></div></div>
<div class="actions"><button class="btn primary" onclick="saveConfig()">Save settings</button></div></div>
</div>
<script>
let stations=[];const $=id=>document.getElementById(id);
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function encodingName(id){return ({0:'JJY 40 kHz pulse-width AM',1:'JJY 60 kHz pulse-width AM',2:'WWVB 60 kHz pulse-width AM',3:'DCF77 77.5 kHz reduced-carrier AM',4:'BSF 68 kHz time code',5:'MSF 60 kHz A/B time code',6:'BPC 68.5 kHz two-bit symbols'}[id])||'standard time-code';}
function status(m,ok){let d=$('statusMsg');d.textContent=m;d.className='status '+(ok?'success':'error');setTimeout(()=>d.className='status',3000);}
function opts(v){return stations.map(s=>`<option value="${s.id}" ${s.id==v?'selected':''}>${esc(s.name)}</option>`).join('');}
function mt(v){return String(Math.floor(v/60)).padStart(2,'0')+':'+String(v%60).padStart(2,'0');}
function buildTxOffsetOptions(){
  let a=['<option value="0">No offset — matches selected time zone</option>'];
  for(let m=-720;m<=840;m+=60){
    if(m===0) continue;
    let sign=m<0?'-':'+', x=Math.abs(m), h=Math.floor(x/60), mm=x%60;
    a.push(`<option value="${m}">${sign}${h}${mm?':'+String(mm).padStart(2,'0'):''}</option>`);
  }
  [-570,-210,210,330,345,390,630,765].forEach(m=>{
    let sign=m<0?'-':'+', x=Math.abs(m), h=Math.floor(x/60), mm=x%60;
    a.push(`<option value="${m}">${sign}${h}:${String(mm).padStart(2,'0')}</option>`);
  });
  $('txOffset').innerHTML=a.join('');
}
function tm(v){let a=v.split(':').map(Number);return a[0]*60+a[1];}
function render(sc){$('schedulesList').innerHTML=sc.map(x=>row(x.station,x.start,x.end)).join('')||'<div class="hint">No schedules configured.</div>';}
function row(st,sta,en){let d=document.createElement('div');d.className='schedule';
d.innerHTML=`<div class="station"><label>Radio type</label><select class="schStation">${opts(st)}</select></div><div><label>Start</label><input class="schStart" type="time" value="${mt(sta)}"></div><div><label>End</label><input class="schEnd" type="time" value="${mt(en)}"></div><button class="btn danger" onclick="this.closest('.schedule').remove()">Delete</button>`;return d.outerHTML;}
function addSchedule(){if(stations.length){let d=document.createElement('div');d.innerHTML=row(0,0,1439);$('schedulesList').appendChild(d.firstElementChild);}}
async function load(){try{let [c,s,sc]=await Promise.all([fetch('/api/config').then(r=>r.json()),fetch('/api/stations').then(r=>r.json()),fetch('/api/schedules').then(r=>r.json())]);
$('ssid').value=c.ssid||'';$('timezone').value=c.timezone||'Australia/Brisbane';buildTxOffsetOptions();$('txOffset').value=String(c.transmission_offset_minutes??0);$('fullTime').checked=!!c.full_time_tx;stations=s;$('fullTimeStation').innerHTML=opts(c.full_time_station??0);render(sc);
renderWifiPowerButtons(c.wifi_power_mode??0);
$('stationsList').innerHTML=stations.map(x=>`<div class="switchrow"><strong>${esc(x.name)}</strong><span class="badge">Available</span></div>`).join('');
}catch(e){status('Unable to load configuration',false);}}
async function saveConfig(){
 let f=new FormData();
 f.append('ssid',$('ssid').value);
 f.append('password',$('password').value);
 f.append('timezone',$('timezone').value);
 try{
  let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
  status(d.status==='ok'?'Settings saved.':'Save failed',d.status==='ok');
 }catch(e){status('Save failed',false);}
}

function renderWifiPowerButtons(mode){
  $('wifiPowerAlwaysBtn').className='btn '+(mode==0?'primary':'secondary');
  $('wifiPowerScheduledBtn').className='btn '+(mode==1?'primary':'secondary');
}

async function setWifiPowerMode(mode){
  renderWifiPowerButtons(mode); // reflect instantly
  let f=new FormData();
  f.append('wifi_power_mode',String(mode));
  try{
    let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
    if(d.status!=='ok') throw new Error('update failed');
    status(mode==1?'Power-save mode enabled. WiFi will turn off between sync windows.':'WiFi will stay on continuously.',true);
  }catch(e){status('Unable to update WiFi power mode.',false);}
}

async function setTimezone(){
  let f=new FormData();
  f.append('timezone',$('timezone').value);
  try{
    let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
    if(d.status!=='ok') throw new Error('update failed');
    status('Time zone updated.',true);
    await tick();
  }catch(e){status('Unable to update time zone.',false);}
}

async function setTransmissionOffset(){
  let f=new FormData();
  f.append('transmission_offset_minutes',$('txOffset').value);
  try{
    let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
    if(d.status!=='ok') throw new Error('update failed');
    status('Transmission offset updated.',true);
    await tick();
  }catch(e){status('Unable to update transmission offset.',false);}
}

async function setFullTime(){
 const enabled=$('fullTime').checked;
 const badge=$('txBadge');

 // Reflect the change instantly in the browser.
 badge.textContent=enabled?'Full-time TX':'Scheduled';
 badge.className='badge '+(enabled?'':'off');

 let f=new FormData();
 f.append('full_time_tx',enabled?'1':'0');
 f.append('full_time_station',$('fullTimeStation').value);

 try{
  let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
  if(d.status!=='ok') throw new Error('update failed');
  status(enabled?'Full-time transmission enabled.':'Returned to scheduled transmission.',true);
  await tick();
 }catch(e){
  $('fullTime').checked=!enabled;
  badge.textContent=!enabled?'Full-time TX':'Scheduled';
  badge.className='badge '+(!enabled?'':'off');
  status('Unable to change transmission mode.',false);
 }
}

async function setFullTimeStation(){
 let f=new FormData();
 f.append('full_time_tx',$('fullTime').checked?'1':'0');
 f.append('full_time_station',$('fullTimeStation').value);
 try{
  let d=await fetch('/api/config',{method:'POST',body:f}).then(r=>r.json());
  if(d.status!=='ok') throw new Error('update failed');
  status('Full-time radio type updated.',true);
  await tick();
 }catch(e){status('Unable to update radio type.',false);}
}

async function syncNtp(){
  const b=$('ntpButton');
  const old=b.textContent;
  b.disabled=true;
  b.textContent='Synchronising...';
  try{
    let d=await fetch('/api/ntp-sync',{method:'POST'}).then(r=>r.json());
    if(d.status!=='ok') throw new Error(d.message||'NTP request failed');
    status('NTP synchronisation requested.',true);
    setTimeout(tick,1500);
  }catch(e){
    status(e.message||'Unable to request NTP synchronisation.',false);
  }finally{
    b.disabled=false;
    b.textContent=old;
  }
}
async function saveSchedules(){let a=[...document.querySelectorAll('.schedule')].map(x=>({station:+x.querySelector('.schStation').value,start:tm(x.querySelector('.schStart').value),end:tm(x.querySelector('.schEnd').value)}));
for(let x of a)if(x.end<=x.start){status('End time must be later than start time.',false);return;}
try{let d=await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(a)}).then(r=>r.json());status(d.status==='ok'?'Schedules saved.':'Save failed',d.status==='ok');}catch(e){status('Save failed',false);}}
async function tick(){try{let d=await fetch('/api/status').then(r=>r.json());$('timeDisplay').textContent=d.time||'--:--:--';$('dateDisplay').textContent=(d.date||'')+' '+(d.timezone||'');if(d.firmware_version)$('fwVersion').textContent=d.firmware_version;let b=$('txBadge');b.textContent=d.full_time_tx?'Full-time TX':'Scheduled';b.className='badge '+(d.full_time_tx?'':'off');$('txTimeDisplay').textContent=d.tx_time||'--:--:--';let st=stations.find(x=>x.id==d.station);$('txStationName').textContent=st?st.name:'--';$('txEncoding').textContent=st?('Encoding: '+(st.encoding||encodingName(st.id))):'Encoding: --';$('txTimeNote').textContent=d.tx_next_minute?'Time code represents the following minute':'Time code represents the current transmission frame';}catch(e){}}
load();tick();setInterval(tick,1000);
</script></body></html>
)HTML";
  return html;
}

//...................................................................
// End of file
// Firmware Version: V2.6
