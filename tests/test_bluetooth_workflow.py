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
FIRMWARE = ROOT / 'firmware/RadioClock_V4_0_Casio_BLE_Reliability/RadioClock_V4_0_Casio_BLE_Reliability.ino'


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
                      r'\([^;]+?\)\s*\{', source, re.M)
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
#include <functional>
#include <map>
#include <string>
#include <strings.h>
#include "RadioBleArbiter.h"
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
time_t fakeEpoch = 1704110400; // 2024-01-01 12:00 UTC: deterministic slot/day checks.
time_t fakeTime(time_t* out) { if (out) *out = fakeEpoch; return fakeEpoch; }
#define time fakeTime
constexpr int WL_CONNECTED = 3;
struct WiFiMock { int status() const { return WL_CONNECTED; } } WiFi;
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
  bool scanning = false, startOk = true;
  int starts = 0, stops = 0;
  NimBLEScanCallbacks* callback = nullptr;
  void setScanCallbacks(NimBLEScanCallbacks* c, bool) { callback = c; }
  void setActiveScan(bool) {}
  void setInterval(int) {}
  void setWindow(int) {}
  void setMaxResults(int) {}
  bool isScanning() const { return scanning; }
  bool start(unsigned long, bool, bool) { ++starts; return scanning = startOk; }
  bool stop() { ++stops; scanning = false; return true; }
} mockScan;
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
constexpr const char *DEFAULT_BT_TIMEZONE = "Australia/Brisbane";
String btTimezoneName(DEFAULT_BT_TIMEZONE);
int btTimeOffsetMinutes = 0;
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
void serviceBluetoothSync();
void resetBluetoothDayIfNeeded();
void stopBluetoothWindow();
void startBluetoothWindow(int, bool = false);
void saveConfig();
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
int main(int argc, char**) {
  setenv("TZ", "UTC0", 1); tzset();
  std::fill(std::begin(btSyncEnabled), std::end(btSyncEnabled), false);
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
  server.post("/test/protocol", {{"bt_manual_protocol", "1"}}); assert(server.code == 200 && configDirty);
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
  for (int i = 0; i < BT_WATCH_PROFILE_COUNT; ++i) { btProfileAddress[i] = ""; btProfileProtocol[i] = 0; }
  loadConfig();
  assert(btAlwaysWaitEnabled && btManualProtocol == BT_PROTOCOL_STANDARD);
  assert(btTimezoneName == "Asia/Tokyo" && btTimeOffsetMinutes == 30);
  assert(btProfileAddress[0] == "11:22:33:44:55:66" && btProfileAddress[1] == "aa:bb:cc:dd:ee:ff");
  assert(btProfileProtocol[0] == 1 && btProfileProtocol[1] == 2 && btProfileProtocol[2] == 1 && btProfileProtocol[3] == 2);

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

  // RF has priority, with disconnect and physical-controller failures keeping RF off.
  mockQuiescent = false;
  assert(!radioBleArbiter.requestRf()); serviceBluetoothSync();
  assert(radioBleArbiter.bleOwned() && !radioBleArbiter.rfOwned());
  assert(NimBLEDevice::deinits == 0 && barriersReleased == 0 && !btWindowActive);
  server.post("/api/bluetooth-sync"); assert(server.code == 409);
  server.post("/api/bluetooth-pair"); assert(server.code == 409);
  tick += 1001; mockQuiescent = true; NimBLEDevice::deinitOk = false;
  serviceBluetoothSync(); assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
  tick += 1001; NimBLEDevice::deinitOk = true; NimBLEDevice::controllerStops = false;
  serviceBluetoothSync(); assert(radioBleArbiter.bleOwned() && !radioBleArbiter.requestRf());
  tick += 1001; NimBLEDevice::controllerStops = true;
  serviceBluetoothSync();
  assert(!btBleInitialized && btSuspendedByRadio && radioRefreshes == 1);
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
  trustedClock = false;
  server.post("/api/bluetooth-sync"); assert(server.code == 503);
  server.post("/api/bluetooth-pair"); assert(server.code == 503);
  std::puts("BLE workflow, explicit pairing, HTTP retry, protocol persistence and fail-closed RF shutdown passed");
}
'''


class BluetoothWorkflowTest(unittest.TestCase):
    def test_real_firmware_workflow(self):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for host workflow tests')
        source = FIRMWARE.read_text()
        globals_start = source.index('enum CasioBleProtocol')
        globals_end = source.index('void initBluetoothSync(void);', globals_start)
        scan_start = source.index('class GShockScanCallbacks')
        scan = balanced_block(source, scan_start) + ';'
        names = ['btWatchNameMatches', 'clearBtDiscovery', 'consumeBtDiscovery',
                 'cancelBtResponse', 'radioScheduleActiveNow', 'bluetoothActivityPresent',
                 'initBluetoothSync', 'shutdownBluetoothForRadio', 'resetBluetoothDayIfNeeded',
                 'btMinutesOfDay', 'bluetoothTimeSlotConflicts', 'startBluetoothWindow',
                 'stopBluetoothWindow', 'attemptBluetoothSync', 'serviceBluetoothSync',
                 'bluetoothSettingsMutable', 'bluetoothState', 'btWeekday', 'btNthSunday',
                 'btLastSunday', 'btDstAtUtc', 'btBaseOffsetSeconds', 'bluetoothLocalTime',
                 'loadConfig', 'writeConfigNow', 'saveConfig']
        functions = '\n\n'.join(extract_function(source, n) for n in names)
        routes = '\n'.join(extract_route(source, path) for path in
                           ['/api/settings', '/api/bluetooth-pair', '/api/bluetooth-sync'])
        protocol_branch = balanced_block(source, source.index('if (server.hasArg("bt_manual_protocol"))'))
        routes = ('void registerRoutes() {\n' + routes +
                  '\nserver.on("/test/protocol", HTTP_POST, []() {\n' + protocol_branch + '\n});\n}')
        # Forward declaration precedes initBluetoothSync, which creates this callback class.
        functions = functions.replace('void initBluetoothSync(void) {', scan + '\n\nvoid initBluetoothSync(void) {', 1)
        unit_source = '\n'.join([MOCKS, source[globals_start:globals_end], HELPERS, functions, routes, DRIVER])
        with tempfile.TemporaryDirectory(prefix='radioclock-workflow-test-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(unit_source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            subprocess.run([str(binary), 'partial-init'], check=True)


if __name__ == '__main__':
    unittest.main()
