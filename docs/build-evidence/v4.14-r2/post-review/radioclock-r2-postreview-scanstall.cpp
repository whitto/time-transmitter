
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <strings.h>
#include <vector>
#include "RadioBleArbiter.h"
#include "CasioWatchBattery.h"
#include "BtSyncHistory.h"
#include "RadioWifiAccessWindow.h"
#include "RadioConfigWriter.h"
#include "RadioJsonWriter.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
size_t strlcpy(char* to, const char* from, size_t count) {
  if (count) { std::strncpy(to, from, count - 1); to[count - 1] = '\0'; }
  return std::strlen(from);
}
class String {
public:
  std::string s;
  String(const char* p = "") : s(p) {}
  String(std::string p) : s(std::move(p)) {}
  String(int n) : s(std::to_string(n)) {}
  String(unsigned long n) : s(std::to_string(n)) {}
  size_t length() const { return s.length(); }
  const char* c_str() const { return s.c_str(); }
  void reserve(size_t n) { s.reserve(n); }
  void trim() {
    auto first = s.find_first_not_of(" \t\r\n");
    s = first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
  }
  void replace(const char* from, const char* to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, std::strlen(from), to); pos += std::strlen(to);
    }
  }
  void toUpperCase() { for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c)); }
  int indexOf(const char* p) const { auto pos = s.find(p); return pos == std::string::npos ? -1 : int(pos); }
  bool equalsIgnoreCase(const char* p) const { return strcasecmp(s.c_str(), p) == 0; }
  long toInt() const { return std::strtol(s.c_str(), nullptr, 10); }
  void toCharArray(char* out, size_t count) const { strlcpy(out, c_str(), count); }
  String& operator+=(const String& v) { s += v.s; return *this; }
  String& operator+=(char v) { s += v; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend bool operator==(const String& a, const String& b) { return a.s == b.s; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
};
struct SerialMock {
  void println(const char*) {}
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
uint32_t tick = 100;
uint32_t millis() { return tick; }
bool hostContext = false, allowHostProgress = true;
void delay(uint32_t);
struct ble_npl_event {
  void (*fn)(ble_npl_event*) = nullptr;
  void* argument = nullptr;
  bool queued = false;
};
std::deque<ble_npl_event*> hostEvents;
std::vector<ble_npl_event*> submittedScanEvents;
int scanEventReleases = 0;
void ble_npl_event_init(ble_npl_event* e, void (*fn)(ble_npl_event*), void* argument) {
  assert(!e->queued); e->fn = fn; e->argument = argument;
}
void* ble_npl_event_get_arg(ble_npl_event* e) { return e->argument; }
void* nimble_port_get_dflt_eventq() { return nullptr; }
void ble_npl_eventq_put(void*, ble_npl_event* e) {
  assert(e->fn && !e->queued);
  e->queued = true; submittedScanEvents.push_back(e); hostEvents.push_back(e);
}
void ble_npl_event_deinit(ble_npl_event*);
void delay(uint32_t milliseconds) {
  tick += milliseconds;
  if (!allowHostProgress || hostEvents.empty()) return;
  assert(!hostContext);
  auto* event = hostEvents.front(); hostEvents.pop_front(); event->queued = false;
  assert(event->fn);
  hostContext = true; event->fn(event); hostContext = false;
}
time_t fakeEpoch = 1704110400; // 2024-01-01 12:00 UTC: deterministic slot/day checks.
time_t fakeTime(time_t* out) { if (out) *out = fakeEpoch; return fakeEpoch; }
#define time fakeTime
constexpr int WL_CONNECTED = 3;
struct WiFiMock { int status() const { return WL_CONNECTED; } } WiFi;
void beginWifiCredentialConnection() {}
int sntpRestarts = 0;
void sntp_restart() { ++sntpRestarts; }
struct NimBLEUUID { explicit NimBLEUUID(const char*) {} };
struct NimBLEAddress {
  std::string value;
  std::string toString() const { return value; }
};
struct NimBLEAdvertisedDevice {
  std::string name, address;
  bool service = true;
  bool haveServiceUUID() const { return service; }
  bool isAdvertisingService(NimBLEUUID) const { return service; }
  bool haveName() const { return !name.empty(); }
  std::string getName() const { return name; }
  NimBLEAddress getAddress() const { return {address}; }
  uint8_t getAddressType() const { return 1; }
};
class NimBLEScanCallbacks {
public:
  virtual void onResult(const NimBLEAdvertisedDevice*) {}
};
struct NimBLEScan {
  bool scanning = false, startOk = true, stopOk = true;
  int starts = 0, stops = 0;
  NimBLEScanCallbacks* callback = nullptr;
  void setScanCallbacks(NimBLEScanCallbacks* c, bool) { callback = c; }
  void setActiveScan(bool) {}
  void setInterval(int) {}
  void setWindow(int) {}
  void setMaxResults(int) {}
  void setScanResponseTimeout(int timeout) { assert(timeout == 0); }
  bool isScanning() const { return scanning; }
  bool start(unsigned long, bool, bool) {
    assert(hostContext); ++starts; return scanning = startOk;
  }
  bool stop() {
    assert(hostContext); ++stops; if (!stopOk) return false; scanning = false; return true;
  }
} mockScan;
#include "RadioBleScanControl.h"
struct NimBLEClient {};
struct NimBLERemoteService {};
struct NimBLERemoteCharacteristic {};
constexpr int ESP_BT_CONTROLLER_STATUS_IDLE = 0;
int controllerStatus = ESP_BT_CONTROLLER_STATUS_IDLE;
int esp_bt_controller_get_status() { return controllerStatus; }
struct NimBLEDevice {
  static bool initialized, initOk, deinitOk, controllerStops;
  static int inits, deinits;
  static bool init(const char*) {
    ++inits; initialized = initOk; controllerStatus = 1; return initOk;
  }
  static NimBLEScan* getScan() { return &mockScan; }
  static bool isInitialized() { return initialized; }
  static bool deinit(bool deleteObjects) {
    assert(!deleteObjects); ++deinits;
    if (deinitOk) { initialized = false; mockScan.scanning = false; }
    if (deinitOk && controllerStops) controllerStatus = ESP_BT_CONTROLLER_STATUS_IDLE;
    return deinitOk;
  }
};
bool NimBLEDevice::initialized = false, NimBLEDevice::initOk = true;
bool NimBLEDevice::deinitOk = true, NimBLEDevice::controllerStops = true;
int NimBLEDevice::inits = 0, NimBLEDevice::deinits = 0;
void ble_npl_event_deinit(ble_npl_event* e) {
  assert(!NimBLEDevice::isInitialized() && !e->queued && hostEvents.empty());
  ++scanEventReleases; e->fn = nullptr; e->argument = nullptr;
}
bool full_time_tx = false, trustedClock = true;
bool clockTrusted() { return trustedClock; }
struct Schedule { int start_min, end_min; } schedules[4]{};
int schedule_count = 0, radioRefreshes = 0;
void radioRequestRefresh() { ++radioRefreshes; }
constexpr int HTTP_POST = 1;
struct ServerMock {
  std::map<std::string, std::function<void()>> routes;
  std::map<std::string, String> args;
  int code = 0;
  String response;
  bool hasArg(const char* key) const { return args.count(key); }
  String arg(const char* key) const { auto i = args.find(key); return i == args.end() ? String() : i->second; }
  size_t contentLength = 0;
  void setContentLength(size_t n) { contentLength = n; }
  void sendContent(const char* data, size_t n) { response.s.append(data,n); assert(response.length()==contentLength); }
  void send(int c, const char*, const String& body) { code = c; response = body; }
  void on(const char* path, int, std::function<void()> fn) { routes[path] = fn; }
  void post(const char* path, std::map<std::string, String> values = {}) {
    args = std::move(values); code = 0; response = ""; routes.at(path)(); assert(code);
  }
} server;
std::map<std::string, std::string> flash;
struct File {
  std::string* contents = nullptr;
  size_t position = 0;
  explicit operator bool() const { return contents; }
  size_t size() const { return contents ? contents->size() : 0; }
  bool seek(size_t offset) { position = offset; return offset <= size(); }
  size_t read(uint8_t* out, size_t count) {
    const size_t available = position < size() ? size() - position : 0;
    count = std::min(count, available);
    if (count) std::memcpy(out, contents->data() + position, count);
    position += count; return count;
  }
  size_t write(const uint8_t* data, size_t count) {
    contents->append(reinterpret_cast<const char*>(data), count); return count;
  }
  String readStringUntil(char end) {
    if (position >= contents->length()) return "";
    size_t next = contents->find(end, position);
    if (next == std::string::npos) next = contents->length();
    String line(contents->substr(position, next - position)); position = next + 1; return line;
  }
  size_t print(const String& s) { *contents += s.s; return s.length(); }
  void flush() {}
  void close() {}
};
struct LittleFsMock {
  File open(const char* name, const char* mode) {
    if (*mode == 'w') { flash[name] = ""; return {&flash[name]}; }
    auto it = flash.find(name); return it == flash.end() ? File{} : File{&it->second};
  }
  bool rename(const char* from, const char* to) { flash[to] = flash.at(from); flash.erase(from); return true; }
} LittleFS;
constexpr const char *CONFIG_FILE = "config", *CONFIG_TEMP_FILE = "config.tmp", *DEFAULT_TZ_NAME = "Australia/Brisbane";
constexpr int SN_JJY_E = 0, SN_BPC = 5, WIFI_POWER_ALWAYS_ON = 0, WIFI_POWER_SCHEDULED = 1;
char ssid[64] = "test", passwd[64] = "";
String timezone_name(DEFAULT_TZ_NAME);
int full_time_station = SN_JJY_E, transmission_offset_minutes = 0, wifiPowerMode = 0;
bool wifiAccessEnabled = false;
bool crashDumpEnabled = false;
namespace RadioCrashDumpGate {
bool gateEnabled = false, gateAvailable = true;
void setEnabled(bool value) { gateEnabled = value; }
bool available() { return gateAvailable; }
}
int wifiAccessStart = 1080, wifiAccessEnd = 1200;
String wifiAccessTimezone("Australia/Brisbane");
constexpr const char *DEFAULT_BT_TIMEZONE = "Australia/Brisbane";
String btTimezoneName(DEFAULT_BT_TIMEZONE);
int btTimeOffsetMinutes = 0;
std::atomic<bool> activityLedEnabled{true};
bool configDirty = false;
unsigned long configDirtyBecause = 0;
constexpr unsigned long CONFIG_SAVE_DEBOUNCE_MS = 3000;
bool filesystemAvailable = true, configStorageFault = false;
static char storageSerializationBuffer[2048];
static bool validTimezoneName(const String&) { return true; }

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
#define CASIO_BASIC_REQUEST_CHAR    "26eb002c-b012-49a8-b1f8-394fb2032b0f"

#define BT_SYNC_SLOT_COUNT 4
#define BT_PREPARE_MINUTES 5
#define BT_WINDOW_AFTER_MINUTES 5
#define BT_MANUAL_WINDOW_MINUTES 7
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
uint8_t btFontMode[BT_WATCH_PROFILE_COUNT] = {0,0,0,0}; // 0=keep, 1=Standard/new, 2=Classic
uint8_t btLastWatchAddressType = 0xFF;
uint32_t btConnectionAttempts = 0;
uint32_t btWriteAcknowledgements = 0;
std::atomic<uint32_t> btNotifications{0};
std::atomic<uint32_t> btResponseErrors{0};
const char *btDeliveryEvidence = "No delivery yet";
static void bluetoothLocalTime(time_t utc, struct tm &out);
bool btProfileDoneToday(int profile) {
  if (profile < 0 || profile >= BT_WATCH_PROFILE_COUNT) return false;
  time_t now = time(nullptr); struct tm day; bluetoothLocalTime(now, day);
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
// Delivery history is per protocol/profile and informational. Each enabled
// automatic slot listens every day, including after an earlier delivery.
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
bool btAlwaysWaitEnabled = true;
bool btIdlePowerSaveEnabled = false;
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
String btFontLastStatus = "No font override requested";
// Battery samples are runtime status only: never serialize them to LittleFS.
CasioWatchBattery::Reading btBatteryReadings[BT_WATCH_PROFILE_COUNT];
const char *btBatteryStatuses[BT_WATCH_PROFILE_COUNT] = {
  "Not read since boot", "Not read since boot", "Not read since boot", "Not read since boot"
};
CasioWatchBattery::Reading btPendingBattery;
const char *btPendingBatteryStatus = "Not read during this sync";
std::atomic<uint32_t> btLastSyncEpoch{0};
std::atomic<bool> btLastOutcomeSuccessful{false};
uint32_t btProtocolLastSyncEpoch[BT_PROTOCOL_COUNT] = {};
uint32_t btProfileLastSyncEpoch[BT_WATCH_PROFILE_COUNT] = {};
uint8_t btProfileLastSyncProtocol[BT_WATCH_PROFILE_COUNT] = {};
String btProfileLastSyncAddress[BT_WATCH_PROFILE_COUNT];
bool btHistoryPersistEnabled = true;
uint32_t btHistoryGeneration = 0;
int64_t btHistorySavedDay = -1;
int64_t btHistoryAttemptDay = -1;
uint32_t btHistorySavedEpoch = 0;
uint32_t btHistorySnapshotGeneration = 0;
String btHistorySaveStatus = "No daily snapshot saved yet";
#define BT_HISTORY_FILE "/bt-sync-status.bin"
#define BT_HISTORY_TEMP_FILE "/bt-sync-status.tmp"
// Only previously stored legacy history is copied during config rewrites.
// New runtime history must never leak into an ordinary settings save.
struct BtLegacyHistoryState {
  int syncYear = -1, syncYday = -1;
  String date;
  int protocolYear[BT_PROTOCOL_COUNT] = {-1,-1,-1};
  int protocolYday[BT_PROTOCOL_COUNT] = {-1,-1,-1};
  int profileYear[BT_WATCH_PROFILE_COUNT] = {-1,-1,-1,-1};
  int profileYday[BT_WATCH_PROFILE_COUNT] = {-1,-1,-1,-1};
} btLegacyHistory;

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



static bool bleOperationCancelled() { return radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned(); }
static bool bluetoothMemoryAvailable(bool = false) { return true; }
static const char* btPhase = "Idle";
static void setBluetoothPhase(const char* phase) { btPhase = phase; }
static int btDeferredSlot = -1;
static unsigned long btShutdownRetryMillis = 0;
static bool btInitFailed = false;
static RadioBleScanControl btScanControl;
bool mockQuiescent = true, writeResult = true, preemptWrite = false;
static bool serviceBtClientQuarantine() { return !btBleBusy || mockQuiescent; }
int disconnects = 0, barriersReleased = 0, writes = 0, lastWriteProtocol = -1;
static bool btClientQuiescent() { return mockQuiescent; }
static void disconnectGShock() { ++disconnects; btBleBusy = !mockQuiescent; }
static void releaseBtClientBarrier() { ++barriersReleased; }
static bool stopPartiallyInitializedBluetooth() {
  if (NimBLEDevice::isInitialized()) return false;
  if (NimBLEDevice::controllerStops) controllerStatus = ESP_BT_CONTROLLER_STATUS_IDLE;
  return controllerStatus == ESP_BT_CONTROLLER_STATUS_IDLE;
}
bool performGShockBX5600Sync() {
  ++writes; lastWriteProtocol = BT_PROTOCOL_BX5600_MIP;
  if (preemptWrite) radioBleArbiter.requestRf();
  return writeResult;
}
bool performCasioStandardTimeSync(int p) {
  ++writes; lastWriteProtocol = p;
  if (preemptWrite) radioBleArbiter.requestRf();
  return writeResult;
}
void initBluetoothSync();
static bool bluetoothScanMayStart();
static bool bluetoothControllerNeeded();
static bool shutdownIdleBluetooth();
static int btMinutesOfDay(const struct tm &);
static bool btMinuteInBluetoothWindow(int, int);
static void btSlotOccurrenceDate(int, const struct tm &, int &, int &);
void serviceBluetoothSync();
void resetBluetoothDayIfNeeded();
void stopBluetoothWindow();
void startBluetoothWindow(int, bool = false);
void saveConfig();
static void captureLegacyBluetoothHistory();
static void loadBluetoothHistory();
static bool saveBluetoothHistoryNow(time_t);

static char apiResponseBuffer[8192];
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

bool radioScheduleActiveNow(void) {
  if (full_time_tx || radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned()) return true;
  time_t epoch = time(nullptr); struct tm local; localtime_r(&epoch, &local);
  int minute = local.tm_hour * 60 + local.tm_min;
  for (int i=0;i<schedule_count;++i)
    if (minute >= schedules[i].start_min && minute < schedules[i].end_min) return true;
  return false;
}

bool bluetoothActivityPresent(void) { return radioBleArbiter.bleOwned(); }

static bool bluetoothScanMayStart(void) {
  return btBleInitialized && btWindowActive && !btBleBusy &&
         !bleOperationCancelled() && radioBleArbiter.bleOwned() && bluetoothMemoryAvailable();
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

void initBluetoothSync(void) {
  if (btIdlePowerSaveEnabled && !bluetoothControllerNeeded()) return;
  if (btBleInitialized || (btShutdownRetryMillis && (long)(millis()-btShutdownRetryMillis)<0) ||
      bleOperationCancelled()) return;
  if (!bluetoothMemoryAvailable(true)) {
    btLastSyncStatus = "Bluetooth deferred: insufficient internal memory";
    btShutdownRetryMillis = millis() + 1000;
    return;
  }
  if (!radioBleArbiter.acquireBle()) return;
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
  if (!btScan) {
    btLastSyncStatus = "Bluetooth scanner allocation failed";
    btRadioSuspendRequested = true;
    Serial.println("ERROR: BLE scanner unavailable; shutting down before any RF handoff");
    return;
  }
  static GShockScanCallbacks scanCallbacks;
  btScan->setScanCallbacks(&scanCallbacks, false);
  btScan->setActiveScan(true);
  btScan->setInterval(80);
  btScan->setWindow(60);
  btScan->setMaxResults(0);
  // One-second scan completion reports advertisers lacking a response. Avoid
  // a second timeout callout that could outlive a controller restart.
  btScan->setScanResponseTimeout(0);
  if (!btScanControl.initialize(btScan, BT_SCAN_SLICE_MS, bluetoothScanMayStart)) {
    btLastSyncStatus = "Bluetooth scan control initialization failed";
    btRadioSuspendRequested = true;
    Serial.println("ERROR: Bluetooth scan control unavailable; RF remains gated");
    return;
  }
  Serial.println("BT: NimBLE Casio time-only service ready");
}

void shutdownBluetoothForRadio(void) {
  if (!radioBleArbiter.bleOwned()) return;
  if (btShutdownRetryMillis && (long)(millis()-btShutdownRetryMillis) < 0) return;
  // Preserve explicit requests across RF preemption. Automatic windows retain
  // their slot, and Always Wait is restored from its persistent setting.
  if (btPairModeActive) btPairRequested = true;
  else if (btSyncActiveSlot >= 0) {
    btDeferredSlot = btSyncActiveSlot;
    Serial.printf("BT SCHED: automatic slot %d paused by RF; will resume if its Bluetooth-time window is still open\n",
                  btDeferredSlot + 1);
  } else if (!btPersistentWaitActive && btWindowActive) btManualSyncRequested = true;
  stopBluetoothWindow();
  cancelBtResponse();
  disconnectGShock();
  if (!btScanControl.quiescent() || !btClientQuiescent()) {
    btLastSyncStatus = "RF blocked: Bluetooth scan/disconnect teardown did not finish";
    setBluetoothPhase("Bluetooth shutdown failed; RF off");
    Serial.println("ERROR: BLE scan/disconnect teardown incomplete; LF carrier remains OFF");
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
  btScanControl.releaseAfterHostStop();
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
  bluetoothLocalTime(epoch, day);
  btSyncDayComplete = false; // Informational: any watch profile delivered today.
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i)
    if (btProfileDoneToday(i)) btSyncDayComplete = true;
}

static bool bluetoothControllerNeeded(void) {
  if (btAlwaysWaitEnabled || btPairRequested || btManualSyncRequested ||
      btWindowActive || btBleBusy || btPairModeActive) return true;
  time_t epoch = time(nullptr); struct tm local; bluetoothLocalTime(epoch, local);
  for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) {
    if (!btSyncEnabled[i] || !btMinuteInBluetoothWindow(btMinutesOfDay(local), btSyncTimes[i])) continue;
    int year, yday; btSlotOccurrenceDate(i, local, year, yday);
    if (btDeferredSlot == i || btSlotAttemptYear[i] != year || btSlotAttemptYday[i] != yday) return true;
  }
  return false;
}

static bool shutdownIdleBluetooth(void) {
  if (!radioBleArbiter.bleOwned()) { setBluetoothPhase("Bluetooth idle power saving"); return true; }
  if (btBleBusy || !btClientQuiescent() ||
      (btShutdownRetryMillis && (long)(millis() - btShutdownRetryMillis) < 0)) return false;
  stopBluetoothWindow();
  if (!btScanControl.quiescent()) {
    btShutdownRetryMillis = millis() + 1000;
    setBluetoothPhase("Bluetooth scan shutdown pending");
    return false;
  }
  releaseBtClientBarrier();
  const bool stopped = NimBLEDevice::isInitialized() ? NimBLEDevice::deinit(false)
                                                    : stopPartiallyInitializedBluetooth();
  const bool off = stopped && !NimBLEDevice::isInitialized() &&
                   esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
  btBleInitialized = NimBLEDevice::isInitialized();
  if (!off) {
    btShutdownRetryMillis = millis() + 1000;
    setBluetoothPhase("Bluetooth idle shutdown failed");
    return false; // Retain ownership until the physical controller is off.
  }
  btShutdownRetryMillis = 0;
  btScanControl.releaseAfterHostStop();
  radioBleArbiter.finishBleShutdown(true);
  setBluetoothPhase("Bluetooth idle power saving");
  Serial.println("BT: host/controller OFF between listening windows");
  return true;
}

static int btMinutesOfDay(const struct tm &t) {
  return t.tm_hour * 60 + t.tm_min;
}

static bool btMinuteInBluetoothWindow(int currentMinute, int slotMinute) {
  const int start = (slotMinute - BT_PREPARE_MINUTES + 1440) % 1440;
  const int elapsed = (currentMinute - start + 1440) % 1440;
  return elapsed <= BT_PREPARE_MINUTES + BT_WINDOW_AFTER_MINUTES;
}

static void btSlotOccurrenceDate(int slot, const struct tm &local, int &year, int &yday) {
  year = local.tm_year + 1900;
  yday = local.tm_yday;
  const int delta = btSyncTimes[slot] - btMinutesOfDay(local);
  auto daysInYear = [](int y) { return 365 + ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0); };
  if (delta < -720) {
    if (++yday >= daysInYear(year)) { ++year; yday = 0; }
  } else if (delta > 720) {
    if (--yday < 0) { --year; yday = daysInYear(year) - 1; }
  }
}

bool bluetoothTimeSlotConflicts(int minuteOfDay) {
  if (full_time_tx) return true;
  // Bluetooth slots are entered in the independent BT timezone. Convert the
  // slot to the selected station timezone before comparing it with LF radio
  // schedules. RF still wins if a DST boundary changes the exact overlap.
  time_t now = time(nullptr);
  struct tm btNow, stationNow;
  bluetoothLocalTime(now, btNow);
  localtime_r(&now, &stationNow);
  int timezoneDelta = (stationNow.tm_hour * 60 + stationNow.tm_min) -
                      (btNow.tm_hour * 60 + btNow.tm_min);
  for (int m = -BT_PREPARE_MINUTES; m <= BT_WINDOW_AFTER_MINUTES; ++m) {
    int test = minuteOfDay + m;
    while (test < 0) test += 1440;
    while (test >= 1440) test -= 1440;
    int stationMinute = (test + timezoneDelta) % 1440;
    if (stationMinute < 0) stationMinute += 1440;
    for (int i = 0; i < schedule_count; ++i) {
      if (schedules[i].end_min > schedules[i].start_min &&
          stationMinute >= schedules[i].start_min &&
          stationMinute < schedules[i].end_min) return true;
    }
  }
  return false;
}

void startBluetoothWindow(int slot, bool persistent) {
  if (!btBleInitialized || bleOperationCancelled() || btBleBusy || btWindowActive) return;
  if (slot >= 0) {
    btPairModeActive = false;
    if (slot >= BT_SYNC_SLOT_COUNT || !btSyncEnabled[slot]) return;
    time_t epoch=time(nullptr); struct tm local; bluetoothLocalTime(epoch,local);
    btSlotOccurrenceDate(slot, local, btSlotAttemptYear[slot], btSlotAttemptYday[slot]);
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
  unsigned long remainingSeconds = (unsigned long)BT_MANUAL_WINDOW_MINUTES * 60UL;
  if (slot>=0) {
    time_t epoch=time(nullptr); struct tm local; bluetoothLocalTime(epoch,local);
    const int start=(btSyncTimes[slot]-BT_PREPARE_MINUTES+1440)%1440;
    const int finish=(btSyncTimes[slot]+BT_WINDOW_AFTER_MINUTES)%1440;
    const int elapsedMinutes=(btMinutesOfDay(local)-start+1440)%1440;
    const int windowSeconds=(BT_PREPARE_MINUTES+BT_WINDOW_AFTER_MINUTES+1)*60;
    const int elapsedSeconds=elapsedMinutes*60+local.tm_sec;
    if (elapsedSeconds >= windowSeconds) { stopBluetoothWindow(); return; }
    remainingSeconds=(unsigned long)(windowSeconds-elapsedSeconds);
    Serial.printf("BT SCHED: Watch %u target %02d:%02d %s; listen window %02d:%02d-%02d:%02d; entered at %02d:%02d:%02d\n",
                  btActiveProfile+1, btSyncTimes[slot]/60, btSyncTimes[slot]%60,
                  btTimezoneName.c_str(), start/60, start%60, finish/60, finish%60,
                  local.tm_hour, local.tm_min, local.tm_sec);
  }
  btWindowEndMillis = millis()+remainingSeconds*1000UL;
  if (!persistent && WiFi.status() == WL_CONNECTED) sntp_restart();
  setBluetoothPhase(btPairModeActive ? "Pairing" :
                    slot >= 0 ? "Automatic sync window active" : "Waiting for watch");
  Serial.printf("BT: %s for Watch %u using %s protocol\n",
                persistent ? "Always Wait listening" : btPhase,
                btActiveProfile+1, btProtocolName(btActiveProtocol));
}

void stopBluetoothWindow(void) {
  btWindowActive = false;
  if (btBleInitialized && !btScanControl.stop())
    Serial.println("BT: scan stop not acknowledged; retaining host/controller ownership");
  clearBtDiscovery();
  btSyncActiveSlot = -1;
  btPairModeActive = false;
  btPersistentWaitActive = false;
  setBluetoothPhase("Idle");
}

static void resetBluetoothSlotAttempt(int slot) {
  btSlotAttemptYear[slot] = -1;
  btSlotAttemptYday[slot] = -1;
  if (btDeferredSlot == slot) btDeferredSlot = -1;
  if (btSyncActiveSlot == slot) stopBluetoothWindow();
}

static void commitBluetoothBattery(void) {
  auto &reading = btBatteryReadings[btActiveProfile];
  // A replacement becomes bound only after TIME succeeds. Do not attach an
  // old watch's reading to a newly paired device when its battery read failed.
  if (btActiveProtocol != BT_PROTOCOL_BX5600_MIP ||
      !reading.matches(btProfileAddress[btActiveProfile].c_str())) reading = {};
  if (btPendingBattery.matches(btProfileAddress[btActiveProfile].c_str()) &&
      btActiveProtocol == BT_PROTOCOL_BX5600_MIP) reading = btPendingBattery;
  btBatteryStatuses[btActiveProfile] = btPendingBatteryStatus;
}

bool attemptBluetoothSync(void) {
  btPendingBattery = {};
  btPendingBatteryStatus = btActiveProtocol == BT_PROTOCOL_BX5600_MIP
                            ? "Not read during this sync"
                            : "Unavailable: selected protocol has no validated battery reading";
  btFontLastStatus = btFontMode[btActiveProfile] && btActiveProtocol != BT_PROTOCOL_BX5600_MIP
                    ? "Font skipped: selected protocol does not support this option"
                    : "No font override requested";
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
    time_t syncEpoch=time(nullptr); struct tm syncLocal; bluetoothLocalTime(syncEpoch,syncLocal);
    bool bindingChanged = false;
    btSuccessYear[protocol] = syncLocal.tm_year + 1900;
    btSuccessYday[protocol] = syncLocal.tm_yday;
    btProfileSuccessYear[btActiveProfile] = syncLocal.tm_year + 1900;
    btProfileSuccessYday[btActiveProfile] = syncLocal.tm_yday;
    if (btPairModeActive) {
      bindingChanged = btProfileAddress[btActiveProfile] != btLastWatchAddress ||
                       btProfileName[btActiveProfile] != btLastWatchName ||
                       btProfileProtocol[btActiveProfile] != btActiveProtocol;
      btProfileAddress[btActiveProfile] = btLastWatchAddress;
      btProfileName[btActiveProfile] = btLastWatchName;
      btProfileProtocol[btActiveProfile] = btActiveProtocol;
    }
    commitBluetoothBattery();
    btSyncDayComplete = true;
    if (protocol == BT_PROTOCOL_BX5600_MIP) {
      btSyncLastYear = btSuccessYear[protocol];
      btSyncLastYday = btSuccessYday[protocol];
    }
    char d[24]; strftime(d,sizeof(d),"%Y-%m-%d %H:%M:%S",&syncLocal);
    btLastSyncDate = d;
    btLastSyncStatus = String("Watch ") + (btActiveProfile+1) + ": " + btProtocolName(protocol) + " - time write delivered (watch unverified)";
    btLastSyncEpoch.store((uint32_t)syncEpoch, std::memory_order_release);
    btLastOutcomeSuccessful.store(true, std::memory_order_release);
    btProtocolLastSyncEpoch[protocol] = (uint32_t)syncEpoch;
    btProfileLastSyncEpoch[btActiveProfile] = (uint32_t)syncEpoch;
    btProfileLastSyncProtocol[btActiveProfile] = btActiveProtocol;
    btProfileLastSyncAddress[btActiveProfile] = btLastWatchAddress;
    // Binding changes remain required configuration; routine history is not.
    if (bindingChanged) saveConfig();
    saveBluetoothHistoryNow(syncEpoch);
  } else {
    btLastSyncStatus = String(btProtocolName(protocol)) + " - sync attempt failed";
    btLastOutcomeSuccessful.store(false, std::memory_order_release);
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
  if (btIdlePowerSaveEnabled && !bluetoothControllerNeeded()) {
    shutdownIdleBluetooth();
    return;
  }
  if (!btBleInitialized) initBluetoothSync();
  if (!btBleInitialized || bleOperationCancelled()) return;
  resetBluetoothDayIfNeeded();
  if (!serviceBtClientQuarantine()) {
    setBluetoothPhase("Bluetooth disconnect recovery pending; RF remains gated");
    return;
  }
  // A previously timed-out host teardown may finish later. Only loopTask can
  // clear the quarantine, after the queued GAP barrier completes.
  if (btBleBusy && btClientQuiescent()) disconnectGShock();
  if (!btBleBusy && !bluetoothMemoryAvailable()) {
    btScanControl.stop();
    setBluetoothPhase("Bluetooth deferred: low internal memory");
    return;
  }

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
    time_t epoch=time(nullptr); struct tm local; bluetoothLocalTime(epoch,local);
    int nm = btMinutesOfDay(local);
    for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) {
      bool deferred = btDeferredSlot == i;
      if (!btMinuteInBluetoothWindow(nm, btSyncTimes[i])) {
        if (deferred) btDeferredSlot = -1;
        continue;
      }
      if (!btSyncEnabled[i]) continue;
      int occurrenceYear, occurrenceYday;
      btSlotOccurrenceDate(i, local, occurrenceYear, occurrenceYday);
      if (!deferred && btSlotAttemptYear[i]==occurrenceYear && btSlotAttemptYday[i]==occurrenceYday) continue;
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
      if (!btScanControl.stop()) {
        btLastSyncStatus = "Bluetooth scan teardown pending; connection deferred";
        return;
      }
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
      if (!btScanControl.start()) {
        btLastSyncStatus = "Bluetooth scan start failed";
        setBluetoothPhase("Waiting for watch");
      } else if (!btPairModeActive && btSyncActiveSlot < 0) setBluetoothPhase("Scanning");
    }
  }
}

static bool bluetoothSettingsMutable(void) {
  if (btBleBusy || btPairModeActive || btPairRequested) return false;
  // Manual selection controls the next explicit/passive window. An automatic
  // slot already has its own active profile/protocol and must keep listening.
  if (btWindowActive && btSyncActiveSlot < 0) stopBluetoothWindow();
  btManualSyncRequested = false;
  return true;
}

static bool bluetoothSlotSettingsMutable(void) {
  // Validation must not cancel a window before the remaining checks succeed.
  return !btBleBusy && !btPairModeActive && !btPairRequested;
}

static const char* bluetoothStateText(void) {
  if (btRadioSuspendRequested || radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned())
    return strstr(btPhase, "failed") ? btPhase : "Paused for radio transmission";
  if (btBleBusy) return btPhase;
  if (btPairRequested || btPairModeActive) return "Pairing";
  if (btManualSyncRequested) return "Waiting for watch";
  if (btWindowActive && btSyncActiveSlot >= 0) return "Automatic sync window active";
  if (btWindowActive) return btScan && btScan->isScanning() ? "Scanning" : "Waiting for watch";
  if (btAlwaysWaitEnabled && btProfileAddress[btManualProfile].length() != 17) return "No watch paired";
  return btPhase;
}

static void sendBoundedJson(RadioJsonWriter& json, int code = 200) {
  if (!json.ok()) {
    server.send(503, "application/json", "{\"status\":\"error\",\"message\":\"Response capacity exceeded\"}");
    return;
  }
  server.setContentLength(json.size());
  server.send(code, "application/json", String());
  server.sendContent(json.data(), json.size());
}

static int btWeekday(int year, int month, int day)
{
  static const int table[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) --year;
  return (year + year / 4 - year / 100 + year / 400 + table[month - 1] + day) % 7;
}

static int btNthSunday(int year, int month, int ordinal)
{
  const int firstWeekday = btWeekday(year, month, 1);
  return 1 + ((7 - firstWeekday) % 7) + (ordinal - 1) * 7;
}

static int btLastSunday(int year, int month)
{
  int days = 31;
  if (month == 4 || month == 6 || month == 9 || month == 11) days = 30;
  else if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    days = leap ? 29 : 28;
  }
  return days - ((btWeekday(year, month, days)) % 7);
}

static int btDayOfYear(int year, int month, int day)
{
  static const int beforeMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  return beforeMonth[month - 1] + day - 1 + (month > 2 && leap ? 1 : 0);
}

static bool btDstAtUtc(const String &zoneName, const struct tm &utc)
{
  const int year = utc.tm_year + 1900;
  const int month = utc.tm_mon + 1;
  const int day = utc.tm_mday;
  const int hour = utc.tm_hour;
  if (zoneName == "Australia/Sydney") {
    // 02:00 AEST / 03:00 AEDT on Sunday are both 16:00 UTC on
    // Saturday. Day-of-year arithmetic also handles a Sunday on the
    // first of April/October, whose UTC transition is in March/September.
    const int startDay = btDayOfYear(year, 10, btNthSunday(year, 10, 1)) - 1;
    const int endDay = btDayOfYear(year, 4, btNthSunday(year, 4, 1)) - 1;
    const int utcSeconds = ((btDayOfYear(year, month, day) * 24 + hour) * 60 + utc.tm_min) * 60 + utc.tm_sec;
    return utcSeconds >= startDay * 86400 + 16 * 3600 ||
           utcSeconds < endDay * 86400 + 16 * 3600;
  }
  if (zoneName == "Europe/London") {
    const int start = btLastSunday(year, 3);
    const int end = btLastSunday(year, 10);
    if (month > 3 && month < 10) return true;
    if (month == 3) return day > start || (day == start && hour >= 1);
    if (month == 10) return day < end || (day == end && hour < 1);
    return false;
  }
  if (zoneName == "America/New_York" || zoneName == "America/Los_Angeles") {
    const int start = btNthSunday(year, 3, 2);
    const int end = btNthSunday(year, 11, 1);
    const int startHour = zoneName == "America/New_York" ? 7 : 10;
    const int endHour = zoneName == "America/New_York" ? 6 : 9;
    if (month > 3 && month < 11) return true;
    if (month == 3) return day > start || (day == start && hour >= startHour);
    if (month == 11) return day < end || (day == end && hour < endHour);
  }
  return false;
}

static int btBaseOffsetSeconds(const String &zoneName)
{
  if (zoneName == "Australia/Brisbane" || zoneName == "Australia/Sydney") return 10 * 3600;
  if (zoneName == "Asia/Tokyo") return 9 * 3600;
  if (zoneName == "Asia/Shanghai") return 8 * 3600;
  if (zoneName == "America/New_York") return -5 * 3600;
  if (zoneName == "America/Los_Angeles") return -8 * 3600;
  return 0; // UTC and Europe/London standard time
}

static void bluetoothLocalTime(time_t utc, struct tm &out)
{
  struct tm utcTm;
  gmtime_r(&utc, &utcTm);
  const bool dst = btDstAtUtc(btTimezoneName, utcTm);
  int offset = btBaseOffsetSeconds(btTimezoneName) + (dst ? 3600 : 0) + btTimeOffsetMinutes * 60;
  time_t adjusted = utc + offset;
  gmtime_r(&adjusted, &out);
  out.tm_isdst = dst ? 1 : 0;
}

static bool configFileLayoutValid(File &file)
{
  const size_t size = file.size();
  if (size == 0 || size >= sizeof(storageSerializationBuffer)) return false;
  uint8_t bytes[64];
  size_t lineLength = 0, lines = 0;
  bool valid = true;
  for (size_t offset = 0; valid && offset < size;) {
    const size_t count = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
    if (file.read(bytes, count) != count) { valid = false; break; }
    for (size_t i=0;i<count;++i) {
      const uint8_t value = bytes[i];
      if (value == '\n') { ++lines; lineLength = 0; }
      else if (!value || ++lineLength > 64) { valid = false; break; }
    }
    offset += count;
  }
  if (lineLength) ++lines; // Legacy records may omit their final newline.
  // Bound every later readStringUntil allocation and accept legacy short
  // records only when they contain SSID, password and timezone fields.
  return file.seek(0) && valid && lines >= 3;
}

void loadConfig(void)
{
  activityLedEnabled = true;
  btAlwaysWaitEnabled = true;
  btIdlePowerSaveEnabled = false;
  btHistoryPersistEnabled = true;
  btHistoryGeneration = 0;
  crashDumpEnabled = false;
  RadioCrashDumpGate::setEnabled(false);
  wifiAccessEnabled = false;
  wifiAccessStart = 1080; wifiAccessEnd = 1200;
  wifiAccessTimezone = "Australia/Brisbane";
  btLastSyncDate = "";
  btSyncLastYear = btSyncLastYday = -1;
  for (int i=0;i<BT_PROTOCOL_COUNT;++i) btSuccessYear[i] = btSuccessYday[i] = -1;
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) btProfileSuccessYear[i] = btProfileSuccessYday[i] = -1;
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i) btFontMode[i] = 0;
  for (int i = 0; i < BT_SYNC_SLOT_COUNT; ++i) btSyncEnabled[i] = true;
  File configFile;
  if (filesystemAvailable) configFile = LittleFS.open(CONFIG_FILE, "r");
  if (configFile && !configFileLayoutValid(configFile)) {
    configFile.close();
    configFile = File();
    configStorageFault = true;
    Serial.println("Stored config is oversized or malformed; file retained, safe defaults and setup AP selected");
  }
  if (!configFile) {
    strcpy(ssid, ""); strcpy(passwd, "");
    timezone_name = DEFAULT_TZ_NAME;
    full_time_tx = false;
    wifiPowerMode = WIFI_POWER_ALWAYS_ON;
    captureLegacyBluetoothHistory();
    loadBluetoothHistory();
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
    if (line == "0" || line.equalsIgnoreCase("false")) btSyncEnabled[i] = false;
    else if (line == "1" || line.equalsIgnoreCase("true")) btSyncEnabled[i] = true;
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
    const bool validDate = completed.tm_year == oldYear - 1900 &&
                           completed.tm_mon == oldMonth - 1 && completed.tm_mday == oldDay;
    if (validDate) {
      btLastSyncStatus = "Last time write delivered (saved; watch unverified)";
      btDeliveryEvidence = "Saved delivery record; watch display unverified";
    }
    if (validDate && oldYear == btSyncLastYear && completed.tm_yday == btSyncLastYday) {
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
  if (alwaysWait == "0" || alwaysWait.equalsIgnoreCase("false")) btAlwaysWaitEnabled = false;
  else if (alwaysWait == "1" || alwaysWait.equalsIgnoreCase("true")) btAlwaysWaitEnabled = true;
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    // Older bindings inherit their existing manual/scheduled profile protocol.
    btProfileProtocol[i] = (i == btManualProfile) ? btManualProtocol : (uint8_t)BT_PROTOCOL_BX5600_MIP;
    for (int slot=0;slot<BT_SYNC_SLOT_COUNT;++slot)
      if (btSyncProfile[slot] == i) { btProfileProtocol[i] = btSyncProtocol[slot]; break; }
    String protocol = configFile.readStringUntil('\n'); protocol.trim();
    if (protocol.length() && validBtProtocol(protocol.toInt())) btProfileProtocol[i] = protocol.toInt();
  }
  // Appended BLE-only civil-time settings. Older configurations end before
  // these lines and retain the safe Brisbane/no-offset defaults.
  String btZone = configFile.readStringUntil('\n'); btZone.trim();
  if (btZone.length() && validTimezoneName(btZone)) btTimezoneName = btZone;
  String btOffset = configFile.readStringUntil('\n'); btOffset.trim();
  if (btOffset.length()) btTimeOffsetMinutes = btOffset.toInt();
  if (btTimeOffsetMinutes < -720 || btTimeOffsetMinutes > 840) btTimeOffsetMinutes = 0;
  // Appended in V4.6: no extra line in older files means activity flashing
  // remains enabled. A malformed value also retains this default.
  String activityLed = configFile.readStringUntil('\n'); activityLed.trim();
  if (activityLed == "0" || activityLed.equalsIgnoreCase("false")) activityLedEnabled = false;
  else if (activityLed == "1" || activityLed.equalsIgnoreCase("true")) activityLedEnabled = true;
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i) {
    String mode = configFile.readStringUntil('\n'); mode.trim();
    if (mode == "1" || mode == "2") btFontMode[i] = mode.toInt();
  }
  String idlePower = configFile.readStringUntil('\n'); idlePower.trim();
  btIdlePowerSaveEnabled = idlePower == "1" || idlePower.equalsIgnoreCase("true");
  // V4.13 append-only settings: old files retain enabled history and no
  // daily access schedule. History itself is stored separately from config.
  String retain = configFile.readStringUntil('\n'); retain.trim();
  if (retain == "0" || retain.equalsIgnoreCase("false")) btHistoryPersistEnabled = false;
  String generation = configFile.readStringUntil('\n'); generation.trim();
  if (generation.length()) {
    char *end = nullptr;
    unsigned long value = strtoul(generation.c_str(), &end, 10);
    if (end && *end == '\0' && generation.c_str()[0] != '-') btHistoryGeneration = (uint32_t)value;
  }
  String access = configFile.readStringUntil('\n'); access.trim();
  bool storedAccess = access == "1" || access.equalsIgnoreCase("true");
  String accessStart = configFile.readStringUntil('\n'); accessStart.trim();
  String accessEnd = configFile.readStringUntil('\n'); accessEnd.trim();
  String accessZone = configFile.readStringUntil('\n'); accessZone.trim();
  int start = accessStart.toInt(), end = accessEnd.toInt();
  if (accessStart == String(start) && accessEnd == String(end) &&
      RadioWifiAccessWindow::validMinutes(start, end) && validTimezoneName(accessZone)) {
    wifiAccessStart = start; wifiAccessEnd = end;
    wifiAccessTimezone = accessZone; wifiAccessEnabled = storedAccess;
  }
  // V4.14 append-only setting: old/corrupt files keep crash dumps Off.
  String crash = configFile.readStringUntil('\n'); crash.trim();
  crashDumpEnabled = RadioCrashDumpGate::available() &&
                     (crash == "1" || crash.equalsIgnoreCase("true"));
  RadioCrashDumpGate::setEnabled(crashDumpEnabled);
  // One-time V2.9 completion migration (existing four times all use Watch 1).
  if (btProfileSuccessYear[0]<0 && btSuccessYear[BT_PROTOCOL_BX5600_MIP]>=2020) {
    btProfileSuccessYear[0]=btSuccessYear[BT_PROTOCOL_BX5600_MIP];
    btProfileSuccessYday[0]=btSuccessYday[BT_PROTOCOL_BX5600_MIP];
  }
  for (int i=0; ssid[i]; i++) if (ssid[i]=='\r'||ssid[i]=='\n') ssid[i]='\0';
  for (int i=0; passwd[i]; i++) if (passwd[i]=='\r'||passwd[i]=='\n') passwd[i]='\0';
  configFile.close();
  captureLegacyBluetoothHistory();
  loadBluetoothHistory();
  Serial.printf("Loaded config: SSID=%s, TZ=%s, Full TX=%s, WiFi mode=%s\n",
                ssid, timezone_name.c_str(), full_time_tx ? "ON":"OFF",
                wifiPowerMode == WIFI_POWER_SCHEDULED ? "Scheduled" : "Always on");
}

static void captureLegacyBluetoothHistory(void) {
  btLegacyHistory.syncYear = btSyncLastYear;
  btLegacyHistory.syncYday = btSyncLastYday;
  btLegacyHistory.date = btLastSyncDate;
  for (int i=0;i<BT_PROTOCOL_COUNT;++i) {
    btLegacyHistory.protocolYear[i] = btSuccessYear[i];
    btLegacyHistory.protocolYday[i] = btSuccessYday[i];
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    btLegacyHistory.profileYear[i] = btProfileSuccessYear[i];
    btLegacyHistory.profileYday[i] = btProfileSuccessYday[i];
  }
}

static void clearBluetoothRuntimeHistory(void) {
  btLastSyncEpoch.store(0);
  btLastOutcomeSuccessful.store(false);
  btLastSyncDate = ""; btLastSyncStatus = "Never synced";
  btLastWatchName = ""; btLastWatchAddress = "";
  btFontLastStatus = "No font result retained across reboot";
  btDeliveryEvidence = "No delivery yet";
  btSyncLastYear = btSyncLastYday = -1; btSyncDayComplete = false;
  btConnectionAttempts = btWriteAcknowledgements = 0;
  btNotifications.store(0); btResponseErrors.store(0);
  for (int i=0;i<BT_PROTOCOL_COUNT;++i) {
    btSuccessYear[i] = btSuccessYday[i] = -1;
    btProtocolLastSyncEpoch[i] = 0;
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    btProfileSuccessYear[i] = btProfileSuccessYday[i] = -1;
    btProfileLastSyncEpoch[i] = 0; btProfileLastSyncProtocol[i] = 0;
    btProfileLastSyncAddress[i] = "";
    btBatteryReadings[i] = {};
    btBatteryStatuses[i] = "Not read since boot";
  }
}

static void loadBluetoothHistory(void) {
  clearBluetoothRuntimeHistory();
  btHistorySavedDay = btHistoryAttemptDay = -1;
  btHistorySavedEpoch = 0; btHistorySnapshotGeneration = 0;
  if (!filesystemAvailable) {
    btHistorySaveStatus = "Storage unavailable; Bluetooth history stays in RAM";
    return;
  }
  BtSyncHistory::Snapshot snapshot{};
  File file = LittleFS.open(BT_HISTORY_FILE, "r");
  const bool exists = (bool)file;
  const bool valid = exists && file.size() == sizeof(snapshot) &&
                     file.read(reinterpret_cast<uint8_t *>(&snapshot), sizeof(snapshot)) == sizeof(snapshot) &&
                     BtSyncHistory::valid(snapshot);
  if (file) file.close();
  if (valid) {
    // Read quota metadata even when retention is Off or the generation was
    // invalidated. Toggling/rebooting cannot buy another save on the same day.
    btHistorySavedDay = snapshot.savedDay;
    btHistorySavedEpoch = (uint32_t)snapshot.successEpoch;
    btHistorySnapshotGeneration = snapshot.generation;
  }
  if (!btHistoryPersistEnabled) {
    btHistorySaveStatus = "Off: Bluetooth history stays in RAM only";
    return;
  }
  if (!valid) {
    // Retain an old installation's already-saved civil date, but do not invent
    // a UTC timestamp/LED age or write a migration record during startup.
    if (btHistoryGeneration == 0 && btLegacyHistory.date.length()) {
      btLastSyncDate = btLegacyHistory.date;
      btLastSyncStatus = "Last time write delivered (legacy saved date; watch unverified)";
      btDeliveryEvidence = "Legacy saved delivery record; watch display unverified";
      btSyncLastYear = btLegacyHistory.syncYear; btSyncLastYday = btLegacyHistory.syncYday;
      for (int i=0;i<BT_PROTOCOL_COUNT;++i) {
        btSuccessYear[i] = btLegacyHistory.protocolYear[i];
        btSuccessYday[i] = btLegacyHistory.protocolYday[i];
      }
      for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
        btProfileSuccessYear[i] = btLegacyHistory.profileYear[i];
        btProfileSuccessYday[i] = btLegacyHistory.profileYday[i];
      }
    }
    btHistorySaveStatus = exists ? "Invalid daily snapshot ignored; no startup write" : "No daily snapshot saved yet";
    return;
  }
  if (snapshot.generation != btHistoryGeneration) {
    btHistorySaveStatus = "Previous snapshot disabled; waiting for an eligible successful sync";
    return;
  }
  btLastSyncEpoch.store((uint32_t)snapshot.successEpoch);
  btLastOutcomeSuccessful.store(true);
  struct tm local; bluetoothLocalTime((time_t)snapshot.successEpoch, local);
  char date[24]; strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
  btLastSyncDate = date;
  btLastSyncStatus = String("Watch ") + ((int)snapshot.lastProfile + 1) + ": " +
                     btProtocolName(snapshot.lastProtocol) + " - time write delivered (saved daily snapshot; watch unverified)";
  btDeliveryEvidence = "Saved daily ATT-acknowledged delivery snapshot; watch display unverified";
  btLastWatchAddress = snapshot.profileAddress[snapshot.lastProfile];
  if (btProfileAddress[snapshot.lastProfile].equalsIgnoreCase(btLastWatchAddress.c_str()))
    btLastWatchName = btProfileName[snapshot.lastProfile];
  btConnectionAttempts = snapshot.connections; btWriteAcknowledgements = snapshot.ackedWrites;
  btNotifications.store(snapshot.notifications); btResponseErrors.store(snapshot.responseErrors);
  for (int i=0;i<BT_PROTOCOL_COUNT;++i) {
    btProtocolLastSyncEpoch[i] = snapshot.protocolEpoch[i];
    if (!snapshot.protocolEpoch[i]) continue;
    bluetoothLocalTime((time_t)snapshot.protocolEpoch[i], local);
    btSuccessYear[i] = local.tm_year + 1900; btSuccessYday[i] = local.tm_yday;
  }
  btSyncLastYear = btSuccessYear[BT_PROTOCOL_BX5600_MIP];
  btSyncLastYday = btSuccessYday[BT_PROTOCOL_BX5600_MIP];
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    if (!snapshot.profileEpoch[i] || snapshot.profileProtocol[i] != btProfileProtocol[i] ||
        !btProfileAddress[i].equalsIgnoreCase(snapshot.profileAddress[i])) continue;
    btProfileLastSyncEpoch[i] = snapshot.profileEpoch[i];
    btProfileLastSyncProtocol[i] = snapshot.profileProtocol[i];
    btProfileLastSyncAddress[i] = snapshot.profileAddress[i];
    bluetoothLocalTime((time_t)snapshot.profileEpoch[i], local);
    btProfileSuccessYear[i] = local.tm_year + 1900; btProfileSuccessYday[i] = local.tm_yday;
  }
  btHistorySaveStatus = "First-success daily snapshot restored; later events were RAM only";
}

static bool saveBluetoothHistoryNow(time_t epoch) {
  if (!btHistoryPersistEnabled) {
    btHistorySaveStatus = "Off: Bluetooth history stays in RAM only";
    return false;
  }
  if (!BtSyncHistory::saveDue(true, (int64_t)epoch, btHistorySavedDay, btHistoryAttemptDay)) {
    btHistorySaveStatus = "Daily save limit: subsequent sync results stay in RAM";
    return false;
  }
  btHistoryAttemptDay = BtSyncHistory::brisbaneDay((int64_t)epoch);
  BtSyncHistory::Snapshot snapshot{};
  // Explicit zeroing makes padding deterministic for the bounded binary CRC.
  memset(&snapshot, 0, sizeof(snapshot));
  snapshot.generation = btHistoryGeneration;
  snapshot.savedDay = btHistoryAttemptDay; snapshot.successEpoch = (int64_t)epoch;
  snapshot.lastProfile = btActiveProfile; snapshot.lastProtocol = btActiveProtocol;
  snapshot.connections = btConnectionAttempts; snapshot.ackedWrites = btWriteAcknowledgements;
  snapshot.notifications = btNotifications.load(); snapshot.responseErrors = btResponseErrors.load();
  for (int i=0;i<BT_PROTOCOL_COUNT;++i) snapshot.protocolEpoch[i] = btProtocolLastSyncEpoch[i];
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    if (!btProfileLastSyncEpoch[i] || btProfileLastSyncProtocol[i] != btProfileProtocol[i] ||
        !btProfileAddress[i].equalsIgnoreCase(btProfileLastSyncAddress[i].c_str())) continue;
    snapshot.profileEpoch[i] = btProfileLastSyncEpoch[i];
    snapshot.profileProtocol[i] = btProfileLastSyncProtocol[i];
    strlcpy(snapshot.profileAddress[i], btProfileLastSyncAddress[i].c_str(), sizeof(snapshot.profileAddress[i]));
  }
  BtSyncHistory::seal(snapshot);
  if (!BtSyncHistory::valid(snapshot)) {
    btHistorySaveStatus = "History snapshot invalid; time delivered but no history saved";
    return false;
  }
  File file = LittleFS.open(BT_HISTORY_TEMP_FILE, "w");
  if (!file) { btHistorySaveStatus = "History save failed: temporary file unavailable; no automatic retry"; return false; }
  const size_t written = file.write(reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot));
  file.flush(); file.close();
  File check = LittleFS.open(BT_HISTORY_TEMP_FILE, "r");
  bool verified = written == sizeof(snapshot) && check && check.size() == sizeof(snapshot);
  uint8_t block[64];
  for (size_t offset = 0; verified && offset < sizeof(snapshot);) {
    const size_t remaining = sizeof(snapshot) - offset;
    const size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
    verified = check.read(block, count) == count &&
               memcmp(block, reinterpret_cast<const uint8_t *>(&snapshot) + offset, count) == 0;
    offset += count;
  }
  if (check) check.close();
  if (!verified || !LittleFS.rename(BT_HISTORY_TEMP_FILE, BT_HISTORY_FILE)) {
    btHistorySaveStatus = "History save failed: prior snapshot retained; no automatic retry";
    return false;
  }
  btHistorySavedDay = snapshot.savedDay;
  btHistorySavedEpoch = (uint32_t)epoch;
  btHistorySnapshotGeneration = btHistoryGeneration;
  btHistorySaveStatus = "First successful sync today saved immediately";
  Serial.printf("BT: daily status snapshot saved (%u bytes); later syncs today stay in RAM\n", (unsigned)sizeof(snapshot));
  return true;
}

static bool tempFileMatches(const char *path, const char *expected, size_t length)
{
  File check = LittleFS.open(path, "r");
  if (!check) return false;
  bool valid = check.size() == length;
  uint8_t bytes[64];
  for (size_t offset = 0; valid && offset < length;) {
    const size_t count = (length - offset < sizeof(bytes)) ? length - offset : sizeof(bytes);
    valid = check.read(bytes, count) == count && memcmp(bytes, expected + offset, count) == 0;
    offset += count;
  }
  check.close();
  return valid;
}

static bool configSaveFailed(const char *reason)
{
  configDirty = false;
  configStorageFault = true;
  Serial.printf("Config save failed: %s; previous file retained, automatic retries disabled\n", reason);
  return false;
}

bool writeConfigNow(void)
{
  configDirty = false; // Explicit calls consume any pending background save.
  if (!filesystemAvailable) return configSaveFailed("storage unavailable");
  RadioConfigWriter record(storageSerializationBuffer, sizeof(storageSerializationBuffer));
  record.line(ssid); record.line(passwd); record.line(timezone_name.c_str());
  record.line(full_time_tx ? "1":"0"); record.number(full_time_station);
  record.number(transmission_offset_minutes); record.number(wifiPowerMode);
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) record.line(btSyncEnabled[i] ? "1":"0");
  record.number(btLegacyHistory.syncYear); record.number(btLegacyHistory.syncYday);
  record.line(btLegacyHistory.date.c_str());
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) record.number(btSyncProtocol[i]);
  record.number(btManualProtocol);
  for (int i=BT_PROTOCOL_STANDARD;i<BT_PROTOCOL_COUNT;++i) {
    record.number(btLegacyHistory.protocolYear[i]); record.number(btLegacyHistory.protocolYday[i]);
  }
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) record.number(btSyncTimes[i]);
  for (int i=0;i<BT_SYNC_SLOT_COUNT;++i) record.number(btSyncProfile[i]);
  record.number(btManualProfile);
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) {
    record.number(btLegacyHistory.profileYear[i]); record.number(btLegacyHistory.profileYday[i]);
  }
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) record.line(btProfileAddress[i].c_str());
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) record.line(btProfileName[i].c_str(), true);
  record.line(btAlwaysWaitEnabled ? "1" : "0");
  for (int i=0;i<BT_WATCH_PROFILE_COUNT;++i) record.number(btProfileProtocol[i]);
  record.line(btTimezoneName.c_str());
  record.number(btTimeOffsetMinutes);
  record.line(activityLedEnabled.load() ? "1" : "0");
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i) record.number(btFontMode[i]);
  record.line(btIdlePowerSaveEnabled ? "1" : "0");
  record.line(btHistoryPersistEnabled ? "1" : "0");
  record.unsignedNumber(btHistoryGeneration);
  record.line(wifiAccessEnabled ? "1" : "0");
  record.number(wifiAccessStart); record.number(wifiAccessEnd); record.line(wifiAccessTimezone.c_str());
  record.line(crashDumpEnabled ? "1" : "0");
  const size_t expectedLines = 24 + 4 * BT_SYNC_SLOT_COUNT + 6 * BT_WATCH_PROFILE_COUNT +
                               2 * (BT_PROTOCOL_COUNT - BT_PROTOCOL_STANDARD);
  if (!record.complete(expectedLines)) return configSaveFailed("invalid or oversized complete record");
  File f=LittleFS.open(CONFIG_TEMP_FILE,"w");
  if (!f) return configSaveFailed("temp open");
  const size_t written = f.write(reinterpret_cast<const uint8_t *>(storageSerializationBuffer), record.size());
  f.flush(); f.close();
  if (written != record.size() ||
      !tempFileMatches(CONFIG_TEMP_FILE, storageSerializationBuffer, record.size()) ||
      !LittleFS.rename(CONFIG_TEMP_FILE,CONFIG_FILE)) return configSaveFailed("write/readback/rename");
  configStorageFault = false;
  Serial.println("Config saved atomically");
  return true;
}

void saveConfig(void)
{
  if (configStorageFault) return; // A failed commit never starts a periodic retry loop.
  configDirty = true;
  configDirtyBecause = millis();
}
void registerRoutes() {
server.on("/api/settings", HTTP_POST, []() {
    if (!server.hasArg("bt_always_wait")) {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Missing Always Wait setting\"}"); return;
    }
    String v=server.arg("bt_always_wait");
    if (v!="0" && v!="1" && v!="true" && v!="false") {
      server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid Always Wait value\"}"); return;
    }
    const bool previousAlwaysWait = btAlwaysWaitEnabled;
    btAlwaysWaitEnabled=(v=="1" || v=="true");
    if (!writeConfigNow()) {
      btAlwaysWaitEnabled = previousAlwaysWait; saveConfig();
      server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Always Wait could not be saved; previous value retained\"}"); return;
    }
    if (!btAlwaysWaitEnabled && btPersistentWaitActive && !btBleBusy) {
      stopBluetoothWindow(); setBluetoothPhase("Idle");
    }
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
      RadioJsonWriter json(apiResponseBuffer, sizeof(apiResponseBuffer));
      json.append("{\"status\":\"error\",\"message\":").quoted(bluetoothStateText()).append("}");
      sendBoundedJson(json, 409); return;
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
    RadioJsonWriter json(apiResponseBuffer, sizeof(apiResponseBuffer));
    json.append("{\"status\":\"ok\",\"state\":").quoted(bluetoothStateText());
    json.key("message").quoted("Manual sync window will restart safely").append("}");
    sendBoundedJson(json);
  });
server.on("/test/protocol", HTTP_POST, []() {
if (server.hasArg("bt_manual_protocol")) {
      int p = server.arg("bt_manual_protocol").toInt();
      if (server.arg("bt_manual_protocol") != String(p) || !validBtProtocol(p) ||
          !bluetoothSettingsMutable()) {
        server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"invalid protocol or Bluetooth window is active\"}");
        return;
      }
      const uint8_t previousProtocol = btManualProtocol;
      btManualProtocol = (uint8_t)p;
      if (!writeConfigNow()) {
        btManualProtocol = previousProtocol; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
});
server.on("/api/config", HTTP_POST, []() {
if (server.hasArg("crash_dump_enabled")) {
      String value = server.arg("crash_dump_enabled");
      if (value != "0" && value != "1" && value != "true" && value != "false") {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid crash-dump setting\"}"); return;
      }
      const bool enabled = value == "1" || value == "true";
      if (enabled && !RadioCrashDumpGate::available()) {
        server.send(503,"application/json","{\"status\":\"error\",\"message\":\"Crash-dump storage unavailable in this ESP32 core\"}"); return;
      }
      const bool previousEnabled = crashDumpEnabled;
      crashDumpEnabled = enabled;
      if (!writeConfigNow()) {
        crashDumpEnabled = previousEnabled; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Crash-dump setting could not be saved; previous value retained\"}"); return;
      }
      RadioCrashDumpGate::setEnabled(crashDumpEnabled);
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }if (server.hasArg("bt_history_persist")) {
      String value = server.arg("bt_history_persist");
      if (value != "0" && value != "1" && value != "true" && value != "false") {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid Bluetooth history retention value\"}"); return;
      }
      const bool previousEnabled = btHistoryPersistEnabled;
      const uint32_t previousGeneration = btHistoryGeneration;
      const bool enabled = value == "1" || value == "true";
      if (!enabled && previousEnabled && btHistoryGeneration == UINT32_MAX) {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"History generation exhausted; setting retained\"}"); return;
      }
      btHistoryPersistEnabled = enabled;
      // A disabled snapshot must never reappear after Off -> On -> reboot.
      // Config stores this invalidation; no history write/delete is needed.
      if (!enabled && previousEnabled) ++btHistoryGeneration;
      if (!writeConfigNow()) {
        btHistoryPersistEnabled = previousEnabled; btHistoryGeneration = previousGeneration;
        saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"History retention could not be saved; previous value retained\"}"); return;
      }
      btHistorySaveStatus = enabled ? "On: next eligible first-success snapshot will be saved"
                                   : "Off: Bluetooth history stays in RAM only";
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }if (server.hasArg("wifi_access_enabled") || server.hasArg("wifi_access_start") ||
        server.hasArg("wifi_access_end") || server.hasArg("wifi_access_timezone")) {
      if (!server.hasArg("wifi_access_enabled") || !server.hasArg("wifi_access_start") ||
          !server.hasArg("wifi_access_end") || !server.hasArg("wifi_access_timezone")) {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Complete daily Wi-Fi schedule required\"}"); return;
      }
      String value = server.arg("wifi_access_enabled");
      int start = server.arg("wifi_access_start").toInt();
      int end = server.arg("wifi_access_end").toInt();
      String zone = server.arg("wifi_access_timezone");
      if ((value != "0" && value != "1" && value != "true" && value != "false") ||
          server.arg("wifi_access_start") != String(start) || server.arg("wifi_access_end") != String(end) ||
          !RadioWifiAccessWindow::validMinutes(start, end) || !validTimezoneName(zone)) {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid daily Wi-Fi schedule; start and end must differ\"}"); return;
      }
      const bool previousEnabled = wifiAccessEnabled;
      const int previousStart = wifiAccessStart, previousEnd = wifiAccessEnd, previousMode = wifiPowerMode;
      const String previousZone = wifiAccessTimezone;
      wifiAccessEnabled = value == "1" || value == "true";
      wifiAccessStart = start; wifiAccessEnd = end; wifiAccessTimezone = zone;
      if (wifiAccessEnabled) wifiPowerMode = WIFI_POWER_SCHEDULED;
      if (!writeConfigNow()) {
        wifiAccessEnabled = previousEnabled; wifiAccessStart = previousStart; wifiAccessEnd = previousEnd;
        wifiAccessTimezone = previousZone; wifiPowerMode = previousMode;
        saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Wi-Fi schedule could not be saved; previous settings retained\"}"); return;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }if (server.hasArg("bt_font_profile") || server.hasArg("bt_font_mode")) {
      int profile = server.arg("bt_font_profile").toInt();
      int mode = server.arg("bt_font_mode").toInt();
      if (!server.hasArg("bt_font_profile") || !server.hasArg("bt_font_mode") ||
          server.arg("bt_font_profile") != String(profile) || server.arg("bt_font_mode") != String(mode) ||
          profile < 0 || profile >= BT_WATCH_PROFILE_COUNT || mode < 0 || mode > 2) {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid watch font selection\"}"); return;
      }
      const uint8_t previous = btFontMode[profile];
      btFontMode[profile] = mode;
      if (!writeConfigNow()) {
        btFontMode[profile] = previous; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Watch font choice could not be saved; previous value retained\"}"); return;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }if (server.hasArg("bt_idle_power_save")) {
      String value = server.arg("bt_idle_power_save");
      if (value != "0" && value != "1" && value != "true" && value != "false") {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid Bluetooth power-save value\"}"); return;
      }
      const bool previous = btIdlePowerSaveEnabled;
      btIdlePowerSaveEnabled = value == "1" || value == "true";
      if (!writeConfigNow()) {
        btIdlePowerSaveEnabled = previous; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth power saving could not be saved; previous value retained\"}"); return;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}"); return;
    }if (server.hasArg("activity_led_enabled")) {
      String value = server.arg("activity_led_enabled");
      if (value != "0" && value != "1" && value != "true" && value != "false") {
        server.send(400,"application/json","{\"status\":\"error\",\"message\":\"Invalid activity LED value\"}");
        return;
      }
      const bool previousEnabled = activityLedEnabled.load();
      activityLedEnabled = value == "1" || value == "true";
      if (!writeConfigNow()) {
        activityLedEnabled = previousEnabled;
        saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Activity LED setting could not be saved; previous value retained\"}");
        return;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}");
      return;
    }if (server.hasArg("bt_manual_protocol")) {
      int p = server.arg("bt_manual_protocol").toInt();
      if (server.arg("bt_manual_protocol") != String(p) || !validBtProtocol(p) ||
          !bluetoothSettingsMutable()) {
        server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"invalid protocol or Bluetooth window is active\"}");
        return;
      }
      const uint8_t previousProtocol = btManualProtocol;
      btManualProtocol = (uint8_t)p;
      if (!writeConfigNow()) {
        btManualProtocol = previousProtocol; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }if (server.hasArg("bt_manual_profile")) {
      int p=server.arg("bt_manual_profile").toInt();
      if (server.arg("bt_manual_profile") != String(p) || p<0 || p>=BT_WATCH_PROFILE_COUNT || !bluetoothSettingsMutable()) {
        server.send(409,"application/json","{\"status\":\"error\",\"message\":\"invalid/busy profile\"}");return;
      }
      const uint8_t previousProfile = btManualProfile;
      btManualProfile=(uint8_t)p;
      if (!writeConfigNow()) {
        btManualProfile = previousProfile; saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
      }
      server.send(200,"application/json","{\"status\":\"ok\"}");return;
    }if (server.hasArg("bt_slot")) {
      int slot = server.arg("bt_slot").toInt();
      if (slot < 0 || slot >= BT_SYNC_SLOT_COUNT) {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"invalid Bluetooth slot\"}");
        return;
      }
      if (server.hasArg("bt_slot_time")) {
        String requested=server.arg("bt_slot_time");
        int m=requested.toInt();
        if (requested != String(m) || m<0 || m>=1440 ||
            btOverlapsOtherEnabledSlot(slot,m) || !bluetoothSlotSettingsMutable()) {
          server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Time invalid, busy, or overlaps another Bluetooth window\"}"); return;
        }
        const int previousTime = btSyncTimes[slot];
        btSyncTimes[slot] = m;
        if (!writeConfigNow()) {
          btSyncTimes[slot] = previousTime; saveConfig();
          server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
        }
        if (previousTime != m) resetBluetoothSlotAttempt(slot);
        server.send(200,"application/json","{\"status\":\"ok\"}");return;
      }
      if (server.hasArg("bt_slot_profile")) {
        int p=server.arg("bt_slot_profile").toInt();
        if (server.arg("bt_slot_profile") != String(p) || p<0 || p>=BT_WATCH_PROFILE_COUNT || !bluetoothSlotSettingsMutable()) {
          server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Invalid/busy watch profile\"}");return;
        }
        // One logical watch has one protocol across its enabled sync slots.
        for (int i=0;i<BT_SYNC_SLOT_COUNT;++i)
          if (i!=slot && btSyncEnabled[i] && btSyncProfile[i]==p &&
              btSyncProtocol[i]!=btSyncProtocol[slot]) {
            server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Watch profile uses another protocol\"}");return;
          }
        const uint8_t previousProfile = btSyncProfile[slot];
        btSyncProfile[slot] = (uint8_t)p;
        if (!writeConfigNow()) {
          btSyncProfile[slot] = previousProfile; saveConfig();
          server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
        }
        if (previousProfile != p) resetBluetoothSlotAttempt(slot);
        server.send(200,"application/json","{\"status\":\"ok\"}");return;
      }
      if (server.hasArg("bt_slot_protocol")) {
        int p = server.arg("bt_slot_protocol").toInt();
        if (server.arg("bt_slot_protocol") != String(p) || !validBtProtocol(p) ||
            !bluetoothSlotSettingsMutable()) {
          server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"invalid protocol or Bluetooth window is active\"}");
          return;
        }
        for (int i=0;i<BT_SYNC_SLOT_COUNT;++i)
          if (i!=slot && btSyncEnabled[i] && btSyncProfile[i]==btSyncProfile[slot] &&
              btSyncProtocol[i]!=p) {
            server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Watch profile uses another protocol\"}");return;
          }
        const uint8_t previousProtocol = btSyncProtocol[slot];
        btSyncProtocol[slot] = (uint8_t)p;
        if (!writeConfigNow()) {
          btSyncProtocol[slot] = previousProtocol; saveConfig();
          server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}"); return;
        }
        if (previousProtocol != p) resetBluetoothSlotAttempt(slot);
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
      if (en && (full_time_tx ||
                 btOverlapsOtherEnabledSlot(slot,btSyncTimes[slot]))) {
        server.send(409, "application/json",
                    "{\"status\":\"error\",\"message\":\"Bluetooth time conflicts with full-time radio or another Bluetooth window\"}");
        return;
      }
      if (btBleBusy && btSyncActiveSlot == slot) {
        server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth transaction is active; retry when it finishes\"}");
        return;
      }
      const bool previousEnabled = btSyncEnabled[slot];
      btSyncEnabled[slot] = en;
      // A recurring listening switch must survive an immediate power cycle.
      // Acknowledge only after the atomic flash replacement has succeeded.
      if (!writeConfigNow()) {
        btSyncEnabled[slot] = previousEnabled;
        saveConfig();
        server.send(500,"application/json","{\"status\":\"error\",\"message\":\"Bluetooth setting could not be saved; previous value retained\"}");
        return;
      }
      if (previousEnabled != en || !en) resetBluetoothSlotAttempt(slot);
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
});
}
static void bind(int profile = 0, int protocol = BT_PROTOCOL_BX5600_MIP) {
  btProfileAddress[profile] = "aa:bb:cc:dd:ee:ff";
  btProfileName[profile] = "Original watch";
  btProfileProtocol[profile] = protocol;
}
int main() {
  btAlwaysWaitEnabled=false;
  std::fill(std::begin(btSyncEnabled),std::end(btSyncEnabled),false);
  bind(0, BT_PROTOCOL_BX5600_MIP);
  initBluetoothSync();
  startBluetoothWindow(-1);
  serviceBluetoothSync();
  assert(mockScan.scanning && radioBleArbiter.bleOwned());
  // Host stops dispatching its queue while loopTask remains healthy.
  allowHostProgress=false;
  radioBleArbiter.requestRf();
  auto started=tick;
  for(unsigned retry=0;retry<80;++retry) {
    shutdownBluetoothForRadio();
    tick += 1000;
  }
  assert(tick-started > 70000 && NimBLEDevice::initialized);
  assert(radioBleArbiter.bleOwned() && !radioBleArbiter.rfOwned());
  assert(!btBleBusy && !btScanControl.quiescent());
  std::puts("REPRO: 80 bounded shutdown retries over >70 seconds with a stalled scan host leave RF gated forever; client quarantine never starts");
}
