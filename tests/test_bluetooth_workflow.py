#!/usr/bin/env python3
"""Run the real firmware's BLE workflow, HTTP handlers and config with host mocks.

This exercises branches in the shipped sketch rather than a second state-machine
implementation. Radio/controller and flash mocks make failures reproducible;
device tests are still needed for the radio, watch and actual NimBLE transport.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V4_13/RadioClock_V4_13.ino'


def balanced_block(source, start):
    """Extract a C++ brace block without counting braces in comments/strings."""
    begin = source.index('{', start)
    depth, pos, state = 1, begin + 1, 'code'
    while depth:
        char = source[pos]
        pair = source[pos:pos + 2]
        if state == 'line':
            if char == '\n':
                state = 'code'
        elif state == 'block':
            if pair == '*/':
                state = 'code'
                pos += 1
        elif state in ('"', "'"):
            if char == '\\':
                pos += 1
            elif char == state:
                state = 'code'
        elif pair == '//':
            state = 'line'
            pos += 1
        elif pair == '/*':
            state = 'block'
            pos += 1
        elif char in ('"', "'"):
            state = char
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
        pos += 1
    return source[start:pos]


def extract_function(source, name):
    match = re.search(r'^(?:static )?(?:bool|void|int|String) ' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'Function {name} missing')
    return balanced_block(source, match.start())


def extract_route(source, path):
    start = source.index(f'server.on("{path}", HTTP_POST, []() {{')
    return balanced_block(source, start) + ');'


MOCKS = r'''
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
  void send(int c, const char*, const String& body) { code = c; response = body; }
  void on(const char* path, int, std::function<void()> fn) { routes[path] = fn; }
  void post(const char* path, std::map<std::string, String> values = {}) {
    args = std::move(values); code = 0; response = ""; routes.at(path)(); assert(code);
  }
} server;
String jsonQuoted(const String& s) { return String("\"") + s + "\""; }
std::map<std::string, std::string> flash;
struct File {
  std::string* contents = nullptr;
  size_t position = 0;
  explicit operator bool() const { return contents; }
  size_t size() const { return contents ? contents->size() : 0; }
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
int wifiAccessStart = 1080, wifiAccessEnd = 1200;
String wifiAccessTimezone("Australia/Brisbane");
constexpr const char *DEFAULT_BT_TIMEZONE = "Australia/Brisbane";
String btTimezoneName(DEFAULT_BT_TIMEZONE);
int btTimeOffsetMinutes = 0;
std::atomic<bool> activityLedEnabled{true};
bool configDirty = false;
unsigned long configDirtyBecause = 0;
static bool validTimezoneName(const String&) { return true; }
'''

HELPERS = r'''
static bool bleOperationCancelled() { return radioBleArbiter.rfRequested() || radioBleArbiter.rfOwned(); }
static const char* btPhase = "Idle";
static void setBluetoothPhase(const char* phase) { btPhase = phase; }
static int btDeferredSlot = -1;
static unsigned long btShutdownRetryMillis = 0;
static bool btInitFailed = false;
static RadioBleScanControl btScanControl;
bool mockQuiescent = true, writeResult = true, preemptWrite = false;
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
'''

DRIVER = r'''
static void bind(int profile = 0, int protocol = BT_PROTOCOL_BX5600_MIP) {
  btProfileAddress[profile] = "aa:bb:cc:dd:ee:ff";
  btProfileName[profile] = "Original watch";
  btProfileProtocol[profile] = protocol;
}
static void discover(const char* name = "GW-BX5600", const char* address = "aa:bb:cc:dd:ee:ff") {
  assert(mockScan.callback);
  NimBLEAdvertisedDevice watch{name, address}; mockScan.callback->onResult(&watch);
}
static void manualRetry() {
  server.post("/api/bluetooth-sync"); assert(server.code == 200 && btManualSyncRequested);
  tick += 50;
  serviceBluetoothSync();
  assert(btWindowActive && !btPersistentWaitActive && !btManualSyncRequested && !btPairModeActive);
}
int main(int argc, char** argv) {
  setenv("TZ", "UTC0", 1); tzset();
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  btAlwaysWaitEnabled = false;
  if (argc > 1 && std::strcmp(argv[1], "midnight") == 0) {
    bind(); initBluetoothSync(); btSyncEnabled[0] = true; btSyncTimes[0] = 0;
    fakeEpoch = 1735653480; // Brisbane 2024-12-31 23:58: New Year's target is 2025-01-01.
    serviceBluetoothSync(); assert(btWindowActive && btSyncActiveSlot == 0);
    assert(btSlotAttemptYear[0] == 2025 && btSlotAttemptYday[0] == 0);
    discover(); serviceBluetoothSync(); assert(!btWindowActive && btSyncEnabled[0]);
    fakeEpoch += 180; serviceBluetoothSync(); assert(!btWindowActive); // 00:01 belongs to the same occurrence.
    fakeEpoch += 23 * 3600 + 57 * 60; serviceBluetoothSync();
    assert(btWindowActive && btSlotAttemptYear[0] == 2025 && btSlotAttemptYday[0] == 1);
    stopBluetoothWindow(); resetBluetoothSlotAttempt(0);
    btSyncTimes[0] = 1438; fakeEpoch = 1735653360; // Brisbane 2024-12-31 23:56, target 23:58.
    serviceBluetoothSync(); assert(btWindowActive && btSlotAttemptYear[0] == 2024 && btSlotAttemptYday[0] == 365);
    discover(); serviceBluetoothSync(); assert(!btWindowActive);
    fakeEpoch += 300; serviceBluetoothSync(); assert(!btWindowActive); // 2025-01-01 00:01 still targets Dec 31.
    fakeEpoch += 23 * 3600 + 55 * 60; serviceBluetoothSync();
    assert(btWindowActive && btSlotAttemptYear[0] == 2025 && btSlotAttemptYday[0] == 0);
    std::puts("Midnight/New Year windows retain target-day identity and reopen for the next daily occurrence");
    return 0;
  }
  if (argc > 1) {
    // A failed host init can still leave its controller enabled. RF must wait
    // for recovery, even though isInitialized() is false and no client exists.
    NimBLEDevice::initOk = false; NimBLEDevice::controllerStops = false;
    initBluetoothSync();
    assert(!btBleInitialized && btInitFailed && btRadioSuspendRequested);
    assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
    serviceBluetoothSync(); assert(radioBleArbiter.bleOwned());
    tick += 1001; NimBLEDevice::controllerStops = true;
    serviceBluetoothSync();
    assert(!radioBleArbiter.bleOwned() && btSuspendedByRadio && !btRadioSuspendRequested);
    assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf();
    NimBLEDevice::initOk = true; serviceBluetoothSync();
    assert(btBleInitialized && radioBleArbiter.bleOwned());
    std::puts("Failed BLE initialization retains RF exclusion and recovers after controller shutdown");
    return 0;
  }
  registerRoutes(); initBluetoothSync();
  assert(btBleInitialized && radioBleArbiter.bleOwned());

  // Passive listening never starts unbound or after selecting a mismatched protocol.
  btAlwaysWaitEnabled = true;
  serviceBluetoothSync(); assert(!btWindowActive && mockScan.starts == 0);
  assert(bluetoothState() == "No watch paired");
  bind(); btManualProtocol = BT_PROTOCOL_STANDARD;
  serviceBluetoothSync(); assert(!btWindowActive);
  server.post("/api/bluetooth-sync"); assert(server.code == 409 && !btManualSyncRequested);
  btManualProtocol = BT_PROTOCOL_BX5600_MIP;
  serviceBluetoothSync(); assert(btPersistentWaitActive && btWindowActive);
  assert(std::strcmp(btExpectedProfileAddress, "aa:bb:cc:dd:ee:ff") == 0);

  // Sync Now safely replaces either a passive or already-manual window.
  discover(); assert(btDiscoveryReady);
  int oldGeneration = btDiscoveryGeneration;
  manualRetry();
  assert(!btDiscoveryReady && btDiscoveryGeneration > unsigned(oldGeneration) && writes == 0);
  auto firstEnd = btWindowEndMillis;
  manualRetry(); assert(btWindowEndMillis > firstEnd);
  btBleBusy = true; mockQuiescent = false;
  server.post("/api/bluetooth-sync"); assert(server.code == 200 && btManualSyncRequested);
  auto busyEnd = btWindowEndMillis;
  serviceBluetoothSync(); assert(btManualSyncRequested && btWindowEndMillis == busyEnd);
  mockQuiescent = true; serviceBluetoothSync();
  assert(!btBleBusy && !btManualSyncRequested);

  // Pairing ignores the old address but preserves its binding until a successful write.
  server.post("/api/bluetooth-pair"); assert(server.code == 200 && btPairRequested);
  serviceBluetoothSync(); assert(btPairModeActive && btWindowActive && !btExpectedProfileAddress[0]);
  server.post("/api/bluetooth-sync"); assert(server.code == 409 && !btManualSyncRequested);
  server.post("/api/bluetooth-pair"); assert(server.code == 200 && btPairModeActive);
  server.post("/test/protocol", {{"bt_manual_protocol", "1"}}); assert(server.code == 409);
  assert(btManualProtocol == BT_PROTOCOL_BX5600_MIP);
  writeResult = false; discover("GW-BX5600", "11:22:33:44:55:66"); serviceBluetoothSync();
  assert(btPairModeActive && btWindowActive && btProfileAddress[0] == "aa:bb:cc:dd:ee:ff");
  assert(btProfileName[0] == "Original watch" && !configDirty);
  tick = btWindowEndMillis; serviceBluetoothSync();
  assert(!btWindowActive && !btPairModeActive && btProfileAddress[0] == "aa:bb:cc:dd:ee:ff");
  serviceBluetoothSync(); assert(btPersistentWaitActive);

  // Protocol selection stops passive scanning, preserves the old binding, and
  // a successful replacement commits only the selected watch/protocol.
  bind(1); bind(2); bind(3);
  server.post("/test/protocol", {{"bt_manual_protocol", "1"}}); assert(server.code == 200 && !configDirty);
  assert(btProfileProtocol[0] == BT_PROTOCOL_BX5600_MIP && !btWindowActive);
  serviceBluetoothSync(); assert(!btWindowActive);
  server.post("/api/bluetooth-pair"); serviceBluetoothSync();
  assert(btActiveProtocol == BT_PROTOCOL_STANDARD && btPairModeActive);
  discover("GW-BX5600", "11:22:33:44:55:66"); assert(!btDiscoveryReady);
  writeResult = true; discover("GW-B5600", "11:22:33:44:55:66"); serviceBluetoothSync();
  assert(!btPairModeActive && !btWindowActive && lastWriteProtocol == BT_PROTOCOL_STANDARD);
  assert(btLastSyncDate == "2024-01-01 22:00:00");
  assert(btProfileAddress[0] == "11:22:33:44:55:66" && btProfileName[0] == "GW-B5600");
  assert(btProfileProtocol[0] == BT_PROTOCOL_STANDARD && btProfileProtocol[1] == BT_PROTOCOL_BX5600_MIP);
  assert(btProfileDoneToday(0) && !btProfileDoneToday(1));
  serviceBluetoothSync(); assert(btPersistentWaitActive && btWindowActive);
  int completedWrites = writes;
  discover("GW-B5600", "aa:bb:cc:dd:ee:ff"); assert(!btDiscoveryReady);
  serviceBluetoothSync(); assert(writes == completedWrites);
  writeResult = false; discover("GW-B5600", "11:22:33:44:55:66"); serviceBluetoothSync();
  assert(btPersistentWaitActive && btWindowActive);
  writeResult = true; discover("GW-B5600", "11:22:33:44:55:66"); serviceBluetoothSync();
  assert(!btWindowActive); serviceBluetoothSync(); assert(btPersistentWaitActive);

  // Exercise the real serialization and load, including all four profile protocols.
  btTimezoneName = "Asia/Tokyo"; btTimeOffsetMinutes = 30;
  btProfileProtocol[1] = BT_PROTOCOL_ANALOGUE;
  btProfileProtocol[2] = BT_PROTOCOL_STANDARD;
  btProfileProtocol[3] = BT_PROTOCOL_ANALOGUE;
  writeConfigNow(); assert(flash.count(CONFIG_FILE) && !flash.count(CONFIG_TEMP_FILE));
  btAlwaysWaitEnabled = false; btManualProtocol = BT_PROTOCOL_BX5600_MIP;
  btLastSyncStatus = "Never synced"; btLastSyncDate = ""; btDeliveryEvidence = "No delivery yet";
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i) { btProfileAddress[i] = ""; btProfileProtocol[i] = 0; }
  loadConfig();
  assert(btAlwaysWaitEnabled && btManualProtocol == BT_PROTOCOL_STANDARD);
  assert(btTimezoneName == "Asia/Tokyo" && btTimeOffsetMinutes == 30);
  assert(btProfileAddress[0] == "11:22:33:44:55:66" && btProfileAddress[1] == "aa:bb:cc:dd:ee:ff");
  assert(btProfileProtocol[0] == 1 && btProfileProtocol[1] == 2 && btProfileProtocol[2] == 1 && btProfileProtocol[3] == 2);
  // The compact snapshot stores UTC, rendered using the restored watch timezone.
  assert(btLastSyncDate == "2024-01-01 21:30:00");
  assert(btLastSyncStatus.indexOf("delivered") >= 0 && btLastSyncStatus.indexOf("watch unverified") >= 0);
  assert(std::strstr(btDeliveryEvidence, "watch display unverified"));

  // BLE civil time is independent from the JJY timezone/offset and handles
  // the fixed-zone defaults plus an additive watch-only offset.
  struct tm btLocal = {};
  btTimezoneName = "Australia/Brisbane"; btTimeOffsetMinutes = 0;
  bluetoothLocalTime(fakeEpoch, btLocal);
  assert(btLocal.tm_year == 124 && btLocal.tm_mon == 0 && btLocal.tm_mday == 1 && btLocal.tm_hour == 22);
  btTimezoneName = "Asia/Tokyo"; btTimeOffsetMinutes = 0;
  bluetoothLocalTime(fakeEpoch, btLocal); assert(btLocal.tm_hour == 21);
  btTimezoneName = "Australia/Brisbane"; btTimeOffsetMinutes = 60;
  bluetoothLocalTime(fakeEpoch, btLocal); assert(btLocal.tm_hour == 23);
  // A 12:30 Brisbane Bluetooth slot is 02:30 in the UTC station clock used
  // by this host harness, so radio-overlap checks must convert the slot too.
  btTimeOffsetMinutes = 0; schedule_count = 1; schedules[0] = {150, 160};
  assert(bluetoothTimeSlotConflicts(750));
  schedule_count = 0;

  // Automatic Bluetooth can enter late, and disabling its active slot cancels
  // its scan/discovery immediately without delivering another time write.
  btAlwaysWaitEnabled = false;
  stopBluetoothWindow();
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
  btProfileSuccessYear[3] = -1; btProfileSuccessYday[3] = -1;
  btSyncEnabled[2] = true; btSyncTimes[2] = 1318; // 21:58 Brisbane; current BT time is 22:00.
  btSyncProfile[2] = 3; btSyncProtocol[2] = BT_PROTOCOL_ANALOGUE;
  schedule_count = 1; schedules[0] = {713, 718}; // Partial RF overlap, already finished at 12:00 UTC.
  assert(bluetoothTimeSlotConflicts(1318));
  serviceBluetoothSync();
  assert(btWindowActive && btSyncActiveSlot == 2 && btActiveProfile == 3);
  assert(btActiveProtocol == BT_PROTOCOL_ANALOGUE);
  btBleBusy = true; mockQuiescent = false;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 409 && btSyncEnabled[2] && btWindowActive);
  btBleBusy = false; mockQuiescent = true;
  discover("MTG-B1000"); assert(btDiscoveryReady);
  int beforeDisableWrites = writes;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 200 && !btSyncEnabled[2] && !btWindowActive && !mockScan.scanning);
  assert(!btDiscoveryReady && btSyncActiveSlot == -1 && btDeferredSlot == -1);
  serviceBluetoothSync(); assert(!btWindowActive && writes == beforeDisableWrites);

  // The same partially RF-overlapping slot can be re-enabled; full-time RF
  // and other enabled Bluetooth slots retain their safety constraints.
  full_time_tx = true;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 409 && !btSyncEnabled[2]);
  full_time_tx = false;
  btSyncEnabled[1] = true; btSyncTimes[1] = 1319;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 409 && !btSyncEnabled[2]);
  btSyncEnabled[1] = false;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "true"}});
  assert(server.code == 200 && btSyncEnabled[2] && btSlotAttemptYear[2] == -1);
  serviceBluetoothSync(); assert(btSyncActiveSlot == 2 && btWindowActive);

  // Time/profile/protocol changes reset that slot's attempt even when it has
  // already listened today. The next service pass uses the new settings.
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_time", "1319"}});
  assert(server.code == 200 && !btWindowActive && btSlotAttemptYear[2] == -1);
  serviceBluetoothSync(); assert(btSyncActiveSlot == 2 && btWindowActive);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_profile", "1"}});
  assert(server.code == 200 && !btWindowActive && btSlotAttemptYear[2] == -1);
  serviceBluetoothSync(); assert(btSyncActiveSlot == 2 && btActiveProfile == 1);
  bind(1, BT_PROTOCOL_STANDARD);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_protocol", "1"}});
  assert(server.code == 200 && !btWindowActive && btSlotAttemptYear[2] == -1);
  serviceBluetoothSync(); assert(btSyncActiveSlot == 2 && btActiveProtocol == BT_PROTOCOL_STANDARD);

  // Rejected edits and edits to other slots preserve the active window, its
  // discoveries, and queued explicit manual work.
  auto unchangedEnd = btWindowEndMillis;
  btSyncEnabled[0] = true; btSyncProfile[0] = 3; btSyncProtocol[0] = BT_PROTOCOL_ANALOGUE;
  btSyncTimes[0] = 600;
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_profile", "3"}});
  assert(server.code == 409 && btWindowActive && btSyncActiveSlot == 2 && btWindowEndMillis == unchangedEnd);
  btSyncEnabled[0] = false;
  server.post("/api/config", {{"bt_slot", "3"}, {"bt_slot_time", "1319"}});
  assert(server.code == 409 && btWindowActive && btSyncActiveSlot == 2 && btWindowEndMillis == unchangedEnd);
  server.post("/api/config", {{"bt_slot", "3"}, {"bt_slot_time", "900"}});
  assert(server.code == 200 && btWindowActive && btSyncActiveSlot == 2 && btWindowEndMillis == unchangedEnd);
  server.post("/api/bluetooth-sync"); assert(server.code == 200 && btManualSyncRequested);
  server.post("/api/config", {{"bt_slot", "3"}, {"bt_slot_profile", "2"}});
  assert(server.code == 200 && btManualSyncRequested && btSyncActiveSlot == 2);
  btManualSyncRequested = false;

  // Manual profile/protocol selection leaves an already-active automatic
  // slot intact. Only an explicit Pair Watch request takes it over.
  server.post("/api/config", {{"bt_manual_protocol", "2"}});
  assert(server.code == 200 && btManualProtocol == BT_PROTOCOL_ANALOGUE);
  assert(btWindowActive && btSyncActiveSlot == 2 && btActiveProtocol == BT_PROTOCOL_STANDARD);
  assert(btWindowEndMillis == unchangedEnd);
  server.post("/api/config", {{"bt_manual_profile", "3"}});
  assert(server.code == 200 && btManualProfile == 3);
  assert(btWindowActive && btSyncActiveSlot == 2 && btActiveProfile == 1 && btWindowEndMillis == unchangedEnd);
  server.post("/api/bluetooth-pair"); assert(server.code == 200 && btPairRequested);
  serviceBluetoothSync();
  assert(btWindowActive && btPairModeActive && btSyncActiveSlot == -1);
  assert(btActiveProfile == 3 && btActiveProtocol == BT_PROTOCOL_ANALOGUE && !btExpectedProfileAddress[0]);
  stopBluetoothWindow();
  server.post("/api/config", {{"bt_manual_profile", "0"}}); assert(server.code == 200);
  server.post("/api/config", {{"bt_manual_protocol", "1"}}); assert(server.code == 200);
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_time", "1318"}});
  assert(server.code == 200); serviceBluetoothSync(); assert(btSyncActiveSlot == 2);

  // RF-interrupted automatic windows resume; editing while RF owns the radio
  // clears the old deferred identity and allows the edited window to reopen.
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(btDeferredSlot == 2 && !btWindowActive && !btBleInitialized);
  assert(radioBleArbiter.requestRf());
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_time", "1320"}});
  assert(server.code == 200 && btDeferredSlot == -1 && btSlotAttemptYear[2] == -1);
  radioBleArbiter.releaseRf(); serviceBluetoothSync();
  assert(btWindowActive && btSyncActiveSlot == 2 && !btPersistentWaitActive);
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(btDeferredSlot == 2 && !btWindowActive);
  assert(radioBleArbiter.requestRf());
  server.post("/api/config", {{"bt_slot", "2"}, {"bt_slot_enabled", "false"}});
  assert(server.code == 200 && btDeferredSlot == -1 && !btSyncEnabled[2]);
  radioBleArbiter.releaseRf(); serviceBluetoothSync(); assert(!btWindowActive);
  bind(1, BT_PROTOCOL_ANALOGUE); schedule_count = 0;
  btAlwaysWaitEnabled = true;
  serviceBluetoothSync(); assert(btPersistentWaitActive);
  int previousDeinits = NimBLEDevice::deinits, previousBarriers = barriersReleased;
  int previousRefreshes = radioRefreshes;

  // RF has priority, with disconnect and physical-controller failures keeping RF off.
  mockQuiescent = false;
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(radioBleArbiter.bleOwned() && !radioBleArbiter.rfOwned());
  assert(NimBLEDevice::deinits == previousDeinits && barriersReleased == previousBarriers && !btWindowActive);
  server.post("/api/bluetooth-sync"); assert(server.code == 409);
  server.post("/api/bluetooth-pair"); assert(server.code == 409);
  tick += 1001; mockQuiescent = true; NimBLEDevice::deinitOk = false;
  serviceBluetoothSync(); assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
  tick += 1001; NimBLEDevice::deinitOk = true; NimBLEDevice::controllerStops = false;
  serviceBluetoothSync(); assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
  tick += 1001; NimBLEDevice::controllerStops = true;
  serviceBluetoothSync();
  assert(!btBleInitialized && btSuspendedByRadio && radioRefreshes == previousRefreshes + 1);
  assert(radioBleArbiter.requestRf() && radioBleArbiter.rfOwned());
  int priorInits = NimBLEDevice::inits;
  serviceBluetoothSync(); assert(NimBLEDevice::inits == priorInits);
  radioBleArbiter.releaseRf(); serviceBluetoothSync();
  assert(btBleInitialized && btPersistentWaitActive && btWindowActive && !btSuspendedByRadio);

  // A manual request interrupted during a transaction is restored after RF.
  manualRetry(); preemptWrite = true; writeResult = false;
  discover("GW-B5600", "11:22:33:44:55:66"); serviceBluetoothSync();
  assert(btSuspendedByRadio && btManualSyncRequested && !btWindowActive);
  assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf();
  preemptWrite = false; serviceBluetoothSync();
  assert(btWindowActive && !btPersistentWaitActive && !btManualSyncRequested);

  // Automatic slots use their own watch profile and can replace passive
  // listening. A success on one watch never suppresses another with the same
  // protocol; an RF-interrupted automatic slot resumes as automatic.
  stopBluetoothWindow();
  btSyncEnabled[0] = true; btSyncTimes[0] = 1322;
  btSyncProfile[0] = 1; btSyncProtocol[0] = BT_PROTOCOL_ANALOGUE;
  serviceBluetoothSync();
  assert(btSyncActiveSlot == 0 && btActiveProfile == 1 && btActiveProtocol == BT_PROTOCOL_ANALOGUE);
  assert(btManualProfile == 0 && btManualProtocol == BT_PROTOCOL_STANDARD);
  writeResult = true; discover("MTG-B1000"); serviceBluetoothSync();
  assert(btProfileDoneToday(1)); serviceBluetoothSync(); assert(btPersistentWaitActive);
  btSyncEnabled[0] = false; btSyncEnabled[1] = true; btSyncTimes[1] = 1322;
  btSyncProfile[1] = 2; btSyncProtocol[1] = BT_PROTOCOL_STANDARD;
  serviceBluetoothSync();
  assert(btSyncActiveSlot == 1 && btActiveProfile == 2 && !btProfileDoneToday(2));
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(btDeferredSlot == 1 && !btWindowActive && !btManualSyncRequested);
  assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf(); serviceBluetoothSync();
  assert(btSyncActiveSlot == 1 && btWindowActive && !btPersistentWaitActive && btDeferredSlot == -1);
  btSyncEnabled[1] = false;

  // Disabling Always Wait stops the passive window without clearing the binding.
  stopBluetoothWindow(); serviceBluetoothSync(); assert(btPersistentWaitActive);
  server.post("/api/settings", {{"bt_always_wait", "false"}});
  assert(server.code == 200 && !btAlwaysWaitEnabled && !btWindowActive);
  assert(btProfileAddress[0] == "11:22:33:44:55:66");
  server.post("/api/settings", {{"bt_always_wait", "invalid"}}); assert(server.code == 400);
  server.post("/api/settings"); assert(server.code == 400);

  // A stalled scan-completion queue blocks shutdown and therefore RF. A
  // second service attempt retains the exact queued event instead of clearing
  // or replacing its storage. Late acknowledgement permits a safe retry.
  btAlwaysWaitEnabled = true;
  serviceBluetoothSync(); assert(btPersistentWaitActive && mockScan.scanning);
  const int beforeStallDeinits = NimBLEDevice::deinits;
  allowHostProgress = false;
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(radioBleArbiter.bleOwned() && !radioBleArbiter.rfOwned());
  assert(NimBLEDevice::deinits == beforeStallDeinits && !btScanControl.quiescent());
  assert(hostEvents.size() == 1);
  auto* retainedScanEvent = hostEvents.front();
  const size_t queuedScanCount = submittedScanEvents.size();
  tick += 1001; serviceBluetoothSync();
  assert(hostEvents.size() == 1 && hostEvents.front() == retainedScanEvent);
  assert(submittedScanEvents.size() == queuedScanCount);
  assert(NimBLEDevice::deinits == beforeStallDeinits && !radioBleArbiter.requestRf());
  allowHostProgress = true; tick += 1001; serviceBluetoothSync();
  assert(!btBleInitialized && !radioBleArbiter.bleOwned() && hostEvents.empty());
  assert(NimBLEDevice::deinits == beforeStallDeinits + 1 && btScanControl.quiescent());
  assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf();
  serviceBluetoothSync(); assert(btBleInitialized && btPersistentWaitActive && mockScan.scanning);

  // A host-acknowledged STOP failure must also block RF handoff, including
  // when GAP reports inactive. Only a subsequent successful STOP can recover.
  mockScan.scanning = false; mockScan.stopOk = false;
  const int beforeFailedStopDeinits = NimBLEDevice::deinits;
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(!btScanControl.quiescent() && radioBleArbiter.bleOwned());
  assert(NimBLEDevice::deinits == beforeFailedStopDeinits && !radioBleArbiter.requestRf());
  mockScan.stopOk = true; tick += 1001; serviceBluetoothSync();
  assert(!radioBleArbiter.bleOwned() && NimBLEDevice::deinits == beforeFailedStopDeinits + 1);
  assert(radioBleArbiter.requestRf()); radioBleArbiter.releaseRf();
  serviceBluetoothSync(); assert(btBleInitialized && btPersistentWaitActive);
  server.post("/api/settings", {{"bt_always_wait", "false"}}); assert(server.code == 200);
  trustedClock = false;
  server.post("/api/bluetooth-sync"); assert(server.code == 503);
  server.post("/api/bluetooth-pair"); assert(server.code == 503);
  std::puts("BLE workflow, explicit pairing, HTTP retry, protocol persistence and fail-closed RF shutdown passed");
}
'''

class BluetoothWorkflowTest(unittest.TestCase):
    def unit_source(self, source):
        globals_start = source.index('enum CasioBleProtocol')
        globals_end = source.index('void initBluetoothSync(void);', globals_start)
        scan_start = source.index('class GShockScanCallbacks')
        scan = balanced_block(source, scan_start) + ';'
        names = ['btWatchNameMatches', 'clearBtDiscovery', 'consumeBtDiscovery',
                 'cancelBtResponse', 'radioScheduleActiveNow', 'bluetoothActivityPresent',
                 'bluetoothScanMayStart', 'initBluetoothSync', 'shutdownBluetoothForRadio', 'resetBluetoothDayIfNeeded',
                 'bluetoothControllerNeeded', 'shutdownIdleBluetooth',
                 'btMinutesOfDay', 'btMinuteInBluetoothWindow', 'btSlotOccurrenceDate', 'bluetoothTimeSlotConflicts', 'startBluetoothWindow',
                 'stopBluetoothWindow', 'resetBluetoothSlotAttempt', 'commitBluetoothBattery', 'attemptBluetoothSync', 'serviceBluetoothSync',
                 'bluetoothSettingsMutable', 'bluetoothSlotSettingsMutable', 'bluetoothState', 'btWeekday', 'btNthSunday',
                 'btLastSunday', 'btDayOfYear', 'btDstAtUtc', 'btBaseOffsetSeconds', 'bluetoothLocalTime',
                 'loadConfig', 'captureLegacyBluetoothHistory', 'clearBluetoothRuntimeHistory',
                 'loadBluetoothHistory', 'saveBluetoothHistoryNow', 'writeConfigNow', 'saveConfig']
        functions = '\n\n'.join(extract_function(source, n) for n in names)
        routes = '\n'.join(extract_route(source, path) for path in
                           ['/api/settings', '/api/bluetooth-pair', '/api/bluetooth-sync'])
        protocol_branch = balanced_block(source, source.index('if (server.hasArg("bt_manual_protocol"))'))
        profile_branch = balanced_block(source, source.index('if (server.hasArg("bt_manual_profile"))'))
        slot_branch = balanced_block(source, source.index('if (server.hasArg("bt_slot"))'))
        led_branch = balanced_block(source, source.index('if (server.hasArg("activity_led_enabled"))'))
        font_branch = balanced_block(source, source.index('if (server.hasArg("bt_font_profile") ||'))
        power_branch = balanced_block(source, source.index('if (server.hasArg("bt_idle_power_save"))'))
        history_branch = balanced_block(source, source.index('if (server.hasArg("bt_history_persist"))'))
        wifi_branch = balanced_block(source, source.index('if (server.hasArg("wifi_access_enabled") ||'))
        routes = ('void registerRoutes() {\n' + routes +
                  '\nserver.on("/test/protocol", HTTP_POST, []() {\n' + protocol_branch + '\n});' +
                  '\nserver.on("/api/config", HTTP_POST, []() {\n' + history_branch + wifi_branch + font_branch + power_branch + led_branch + protocol_branch + profile_branch + slot_branch + '\n});\n}')
        # Forward declaration precedes initBluetoothSync, which creates this callback class.
        functions = functions.replace('void initBluetoothSync(void) {', scan + '\n\nvoid initBluetoothSync(void) {', 1)
        return '\n'.join([MOCKS, source[globals_start:globals_end], HELPERS, functions, routes, DRIVER])

    def test_real_firmware_workflow(self):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for host workflow tests')
        unit_source = self.unit_source(FIRMWARE.read_text())
        with tempfile.TemporaryDirectory(prefix='radioclock-workflow-test-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(unit_source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            subprocess.run([str(binary), 'partial-init'], check=True)
            subprocess.run([str(binary), 'midnight'], check=True)


if __name__ == '__main__':
    unittest.main()
