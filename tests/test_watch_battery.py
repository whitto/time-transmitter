#!/usr/bin/env python3
"""Exercise captured GW-BX5600 battery replies and the shipped BLE read path.

Packet fixtures come from public watch/official-app captures. These host tests
check packet validation and session behavior; real watch/radio acceptance is
still needed for the physical BLE transport and reported battery estimate.
"""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_bluetooth_workflow as workflow

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = workflow.FIRMWARE


def extract_function(source, name):
    match = re.search(
        r'^(?:static )?(?:bool|void|size_t|int|String)\s+' + name +
        r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'Function {name} missing')
    return workflow.balanced_block(source, match.start())


DECODE_DRIVER = r'''
#include "CasioWatchBattery.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
  FIXTURE_CASES
  uint8_t reply[] = {0x28, 14, 0x18, 0, 0, 0, 2, 0, 0};
  for (unsigned raw = 14; raw <= 24; ++raw) {
    reply[1] = raw; uint8_t percent = 251;
    assert(CasioWatchBattery::decodeBx(reply, sizeof(reply), percent));
    assert(percent == (raw - 14) * 10);
  }
  // Clamping follows the authoritative model calibration, including real 0%.
  for (unsigned raw : {0U, 13U, 25U, 255U}) {
    reply[1] = raw; uint8_t percent = 251;
    assert(CasioWatchBattery::decodeBx(reply, sizeof(reply), percent));
    assert(percent == (raw < 14 ? 0 : 100));
  }
  // A truncated valid prefix or excess payload is never a battery reading.
  std::vector<uint8_t> payload(600, 0);
  std::memcpy(payload.data(), reply, sizeof(reply));
  for (size_t size = 0; size <= payload.size(); ++size) {
    if (size == sizeof(reply)) continue;
    uint8_t percent = 251;
    assert(!CasioWatchBattery::decodeBx(payload.data(), size, percent));
    assert(percent == 251);
  }
  uint8_t percent = 251;
  assert(!CasioWatchBattery::decodeBx(nullptr, sizeof(reply), percent));
  assert(percent == 251);
  reply[0] = 0x13;
  assert(!CasioWatchBattery::decodeBx(reply, sizeof(reply), percent));
  assert(percent == 251);
  std::puts("Captured GW-BX5600 battery mapping: 60% and 100%; strict packet validation and clamped endpoints passed");
}
'''


NOTIFICATION_MOCKS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
using portMUX_TYPE = int;
#define CASIO_SP_DATA_CHAR "sp-data"
#define CASIO_SET_CHAR "time-all-features"
#define BT_REQUEST_TIMEOUT_MS 5000UL
struct SerialMock {
  void println(const char*) {}
  template<class... T> void printf(const char*, T...) {}
} Serial;
struct NimBLEUUID {
  std::string uuid;
  explicit NimBLEUUID(const char* s): uuid(s) {}
  bool operator==(const NimBLEUUID& other) const { return uuid == other.uuid; }
};
struct NimBLERemoteCharacteristic {
  const char* uuid;
  bool noResponse = true, notify = true, indicate = false, subscribeOk = true;
  unsigned subscriptions = 0;
  NimBLEUUID getUUID() const { return NimBLEUUID(uuid); }
  bool canWriteNoResponse() const { return noResponse; }
  bool canWrite() const { return true; }
  bool canNotify() const { return notify; }
  bool canIndicate() const { return indicate; }
  bool subscribe(bool, void(*)(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool), bool) {
    ++subscriptions; return subscribeOk;
  }
} allFeatures{CASIO_SET_CHAR}, spData{CASIO_SP_DATA_CHAR}, unrelated{"other"};
portMUX_TYPE btResponseMux = 0;
bool btResponseActive = false, btResponseOverflow = false;
uint8_t btExpectedHeader = 0, btResponseBuffer[512];
size_t btResponseLength = 0;
unsigned long btLastFragmentMillis = 0;
unsigned btNotifications = 0, btResponseErrors = 0;
std::atomic<bool> btClientConnected{true}, btClientPreemptionEnabled{true};
bool cancelled = false;
uint32_t tick = 0, elapsed = 0;
uint32_t millis() { return tick; }
bool bleOperationCancelled() { return cancelled; }
struct ScheduledAction { uint32_t after; std::function<void()> fn; };
std::vector<ScheduledAction> scheduled;
void delay(uint32_t ms) {
  tick += ms; elapsed += ms;
  while (!scheduled.empty() && elapsed >= scheduled.front().after) {
    auto fn = scheduled.front().fn; scheduled.erase(scheduled.begin()); fn();
  }
}
void reset(uint32_t start = 0) {
  cancelled = false; btClientConnected = true; btClientPreemptionEnabled = true;
  btResponseActive = false; btResponseOverflow = false;
  btResponseLength = btNotifications = btResponseErrors = 0;
  tick = start; elapsed = 0; scheduled.clear();
}
'''


NOTIFICATION_DRIVER = r'''
int main() {
  uint8_t packet[] = {0x28, 0x14, 0x18, 0, 0, 0, 2, 0, 0};
  uint8_t wrongHeader[] = {0x13, 0x14};
  reset(); prepareBtResponse(0x28);
  gshockNotifyCallback(&spData, packet, sizeof(packet), true);
  gshockNotifyCallback(&unrelated, packet, sizeof(packet), true);
  gshockNotifyCallback(&allFeatures, wrongHeader, sizeof(wrongHeader), true);
  assert(btResponseLength == 0);
  gshockNotifyCallback(&allFeatures, packet, 4, true);
  gshockNotifyCallback(&spData, packet + 4, sizeof(packet) - 4, true);
  assert(btResponseLength == 4);
  gshockNotifyCallback(&allFeatures, packet + 4, sizeof(packet) - 4, true);
  assert(btResponseLength == sizeof(packet) && btNotifications == 2);
  assert(waitBt(sizeof(packet), 1500) && elapsed == 300 && !btResponseActive);
  uint8_t copied[9] = {};
  assert(copyBt(copied, sizeof(copied)) == sizeof(copied));
  assert(std::memcmp(copied, packet, sizeof(packet)) == 0);
  gshockNotifyCallback(&allFeatures, packet, sizeof(packet), true);
  assert(btResponseLength == sizeof(packet)); // Late notification cannot append.

  // Normal SP replies remain on SP_DATA, including their continuations.
  reset(); prepareBtResponse(0x05); packet[0] = 0x05;
  gshockNotifyCallback(&allFeatures, packet, sizeof(packet), true);
  assert(btResponseLength == 0);
  gshockNotifyCallback(&spData, packet, sizeof(packet), true);
  assert(waitBt(sizeof(packet), 1500));
  packet[0] = 0x28;

  // Allow fragments inside the existing 300ms settle gap, counting the budget
  // from the start of the wait, and leave ordinary requests at their 5s default.
  reset(); prepareBtResponse(0x28);
  gshockNotifyCallback(&allFeatures, packet, 4, true);
  scheduled.push_back({200, [&]() { gshockNotifyCallback(&allFeatures, packet + 4, 5, true); }});
  assert(waitBt(sizeof(packet), 1500) && elapsed == 500);
  reset(); prepareBtResponse(0x28);
  assert(!waitBt(sizeof(packet), 1500) && elapsed == 1500 && btResponseErrors == 1);
  assert(!btResponseActive);
  reset(); prepareBtResponse(0x05);
  assert(!waitBt(sizeof(packet)) && elapsed == 5000);

  // RF or disconnect interrupts an optional read immediately and clears its
  // receiver, even when a valid reply arrived just before that interruption.
  for (int failure = 0; failure < 3; ++failure) {
    reset(); prepareBtResponse(0x28);
    gshockNotifyCallback(&allFeatures, packet, sizeof(packet), true);
    scheduled.push_back({100, [failure]() {
      if (failure == 0) cancelled = true;
      if (failure == 1) btClientConnected = false;
      if (failure == 2) btClientPreemptionEnabled = false;
    }});
    assert(!waitBt(sizeof(packet), 1500) && elapsed == 100 && !btResponseActive);
  }
  reset(); prepareBtResponse(0x28);
  std::vector<uint8_t> excessive(513, 0); excessive[0] = 0x28;
  gshockNotifyCallback(&allFeatures, excessive.data(), excessive.size(), true);
  assert(btResponseOverflow && !btResponseActive && btResponseLength == 0);
  assert(!waitBt(sizeof(packet), 1500) && elapsed == 0);
  reset(UINT32_MAX - 100); prepareBtResponse(0x28);
  gshockNotifyCallback(&allFeatures, packet, sizeof(packet), true);
  assert(waitBt(sizeof(packet), 1500) && elapsed == 300); // millis wrap remains bounded.
  std::puts("Battery notification channel, fragmented reply, 1.5s wait budget, cancellation and overflow guards passed");
}
'''


TRANSPORT_MOCKS = r'''
#include "CasioWatchBattery.h"
#include "CasioBxProtocol.h"
#include <sys/time.h>
#define CASIO_BASIC_REQUEST_CHAR "basic-request"
constexpr int BT_PROTOCOL_BX5600_MIP = 0;
constexpr size_t BT_RESPONSE_CAPACITY = 512;
NimBLERemoteCharacteristic batteryRequest{CASIO_BASIC_REQUEST_CHAR};
std::vector<char> order;
std::vector<std::vector<uint8_t>> timePackets, spPackets;
bool disconnectDuringDiscovery = false, missingRequest = false;
bool failRequest = false, timeoutReply = false, wrongChannel = false;
bool disconnectDuringWait = false, cancelDuringWait = false, preemptDuringWait = false;
bool failTime = false, fontDelay = false;
unsigned requests = 0, timeWrites = 0, disconnects = 0, capturesRead = 0;
size_t copyLimit = 512;
std::vector<uint8_t> hardwareReply = {0x28, 0x14, 0x18, 0, 0, 0, 2, 0, 0};
struct NimBLERemoteService {
  NimBLERemoteCharacteristic* getCharacteristic(NimBLEUUID uuid) {
    assert(uuid.uuid == CASIO_BASIC_REQUEST_CHAR);
    if (disconnectDuringDiscovery) btClientConnected = false;
    return missingRequest ? nullptr : &batteryRequest;
  }
} service;
NimBLERemoteService* btService = &service;
NimBLERemoteCharacteristic* btSetChar = &allFeatures;
NimBLERemoteCharacteristic* btSpData = &spData;
uint8_t btActiveProfile = 0, btActiveProtocol = BT_PROTOCOL_BX5600_MIP;
bool btPairModeActive = false;
String btLastWatchName = "GW-BX5600", btLastWatchAddress = "aa:bb:cc:dd:ee:ff";
String btProfileName[4], btProfileAddress[4];
CasioWatchBattery::Reading btPendingBattery, btBatteryReadings[4];
const char* btPendingBatteryStatus = "Not read", *btBatteryStatuses[4] = {};
const char* btGattStage = "idle", *btDeliveryEvidence = "none";
time_t batteryTime(time_t*) { return 1704110400 + elapsed / 1000; }
#define time batteryTime
time_t sampledEpoch = 0;
int sampleTime(struct timeval* tv, void*) {
  order.push_back('C'); sampledEpoch = batteryTime(nullptr);
  tv->tv_sec = sampledEpoch; tv->tv_usec = 123000; return 0;
}
#define gettimeofday sampleTime
size_t strlcpy(char* out, const char* input, size_t capacity) {
  if (capacity) { std::strncpy(out, input, capacity - 1); out[capacity - 1] = '\0'; }
  return std::strlen(input);
}
static void gshockNotifyCallback(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);
bool writeBt(NimBLERemoteCharacteristic* c, const uint8_t* data, size_t size, bool response) {
  if (cancelled || !btClientConnected || !btClientPreemptionEnabled) return false;
  if (c == &batteryRequest) {
    assert(size == 1 && data[0] == 0x28 && !response);
    assert(btResponseActive && btExpectedHeader == 0x28);
    ++requests; order.push_back('B');
    if (failRequest) return false;
    if (!timeoutReply) {
      auto* channel = wrongChannel ? &spData : &allFeatures;
      gshockNotifyCallback(channel, hardwareReply.data(), hardwareReply.size(), true);
    }
    if (disconnectDuringWait || cancelDuringWait || preemptDuringWait) {
      scheduled.push_back({100, []() {
        if (disconnectDuringWait) btClientConnected = false;
        if (cancelDuringWait) cancelled = true;
        if (preemptDuringWait) btClientPreemptionEnabled = false;
      }});
    }
  } else if (c == &allFeatures) {
    assert(response && size == 11 && data[0] == 9);
    ++timeWrites; order.push_back('T'); timePackets.emplace_back(data, data + size);
    if (failTime) return false;
    btClientConnected = false; // The watch closes after the final TIME ACK.
  } else {
    assert(c == &spData && response && (size == 35 || size == 94 || size == 133));
    spPackets.emplace_back(data, data + size);
  }
  return true;
}
bool connectGShock(int protocol) { assert(protocol == 0); return true; }
void disconnectGShock() { ++disconnects; }
bool applyBluetoothWatchFont() {
  if (fontDelay) { order.push_back('F'); delay(2000); }
  return true;
}
void bluetoothLocalTime(time_t epoch, struct tm& out) { gmtime_r(&epoch, &out); }
size_t bxRequest(const uint8_t*, size_t, uint8_t header, size_t minimum,
                 uint8_t* out, size_t capacity) {
  if (cancelled || !btClientConnected || !btClientPreemptionEnabled) return 0;
  order.push_back('S'); ++capturesRead;
  const auto* data = header == 5 ? &settingsReply : header == 3 ? &dstReply : &namesReply;
  assert(data->size() >= minimum && data->size() <= capacity);
  std::copy(data->begin(), data->end(), out); return data->size();
}
void resetBattery() {
  reset(); btService = &service; btSetChar = &allFeatures; btSpData = &spData;
  batteryRequest.noResponse = allFeatures.notify = allFeatures.subscribeOk = true;
  allFeatures.indicate = false; allFeatures.subscriptions = 0;
  missingRequest = disconnectDuringDiscovery = failRequest = timeoutReply = wrongChannel = false;
  disconnectDuringWait = cancelDuringWait = preemptDuringWait = failTime = fontDelay = false;
  requests = timeWrites = disconnects = capturesRead = 0;
  sampledEpoch = 0; copyLimit = 512;
  order.clear(); timePackets.clear(); spPackets.clear();
  hardwareReply = {0x28, 0x14, 0x18, 0, 0, 0, 2, 0, 0};
  btActiveProfile = 0; btActiveProtocol = 0; btPairModeActive = false;
  btLastWatchName = "GW-BX5600"; btLastWatchAddress = "aa:bb:cc:dd:ee:ff";
  for (unsigned i = 0; i < 4; ++i) {
    btBatteryReadings[i] = {}; btProfileName[i] = "GW-BX5600";
    btProfileAddress[i] = "aa:bb:cc:dd:ee:ff"; btBatteryStatuses[i] = "Not read";
  }
  btPendingBattery = {}; btPendingBatteryStatus = "Not read";
}
'''


TRANSPORT_DRIVER = r'''
int main() {
  resetBattery();
  assert(readBluetoothWatchBattery() && requests == 1 && allFeatures.subscriptions == 1);
  assert(elapsed == 300 && btPendingBattery.available && btPendingBattery.percent == 60);
  assert(btPendingBattery.sampledUtc == 1704110400);
  assert(btPendingBattery.matches("AA:BB:CC:DD:EE:FF"));
  assert(!btBatteryReadings[0].available); // Reads stay pending until TIME succeeds.
  assert(!std::strcmp(btPendingBatteryStatus, "Read during sync"));
  commitBluetoothBattery(); assert(btBatteryReadings[0].percent == 60);

  // These failures are optional while the connection remains available. They
  // must not change the required SP handshake or prevent a fresh TIME write.
  resetBattery(); assert(performGShockBX5600Sync());
  const auto requiredSp = spPackets;
  assert((order == std::vector<char>{'B', 'S', 'S', 'S', 'C', 'T'}));
  assert(timeWrites == 1 && disconnects == 1 && elapsed == 300);
  for (int failure = 0; failure < 12; ++failure) {
    resetBattery();
    if (failure == 0) timeoutReply = true;
    if (failure == 1) missingRequest = true;
    if (failure == 2) batteryRequest.noResponse = false; // Response-only is not a valid GET.
    if (failure == 3) allFeatures.notify = false;
    if (failure == 4) allFeatures.subscribeOk = false;
    if (failure == 5) failRequest = true;
    if (failure == 6) hardwareReply.pop_back();
    if (failure == 7) hardwareReply.push_back(0);
    if (failure == 8) hardwareReply[0] = 0x13;
    if (failure == 9) hardwareReply.resize(513);
    if (failure == 10) wrongChannel = true;
    if (failure == 11) copyLimit = 8; // A copy-size mismatch rejects a full response.
    assert(performGShockBX5600Sync() && timeWrites == 1 && disconnects == 1);
    assert(!btPendingBattery.available && spPackets == requiredSp);
    assert(elapsed <= CasioWatchBattery::kReplyTimeoutMs);
    assert(!btResponseActive && std::strcmp(btPendingBatteryStatus, "Read during sync"));
  }
  resetBattery(); allFeatures.notify = false; allFeatures.indicate = true;
  assert(readBluetoothWatchBattery()); // Supported indication subscription also works.

  for (int failure = 0; failure < 4; ++failure) {
    resetBattery();
    disconnectDuringDiscovery = failure == 0;
    disconnectDuringWait = failure == 1;
    cancelDuringWait = failure == 2;
    preemptDuringWait = failure == 3;
    assert(!performGShockBX5600Sync() && timeWrites == 0 && disconnects == 1);
    assert(!btPendingBattery.available && capturesRead == 0);
    assert(elapsed <= 100);
  }
  resetBattery(); timeoutReply = true; fontDelay = true;
  assert(performGShockBX5600Sync());
  assert(sampledEpoch == 1704110403 && timePackets.at(0)[7] == 3);
  assert((order == std::vector<char>{'B', 'S', 'S', 'S', 'F', 'C', 'T'}));
  resetBattery(); failTime = true;
  assert(!performGShockBX5600Sync() && btPendingBattery.available && timeWrites == 1);
  assert(!btBatteryReadings[0].available && disconnects == 1);
  resetBattery(); btPairModeActive = true; btLastWatchName = "";
  assert(!readBluetoothWatchBattery() && requests == 0); // Never guess a replacement model.
  resetBattery(); btLastWatchName = "GW-B5600";
  assert(!readBluetoothWatchBattery() && requests == 0);
  resetBattery(); btActiveProtocol = 1;
  assert(!readBluetoothWatchBattery() && requests == 0);
  resetBattery(); btLastWatchAddress = "";
  assert(!readBluetoothWatchBattery() && requests == 0);
  resetBattery(); btLastWatchName = ""; btProfileName[0] = "CASIO GW-BX5600";
  assert(readBluetoothWatchBattery()); // A verified existing binding supplies a missing advertised name.
  std::puts("Real optional battery GET precedes SP/TIME; unsupported and failed reads preserve time, RF/disconnect abort, and fresh TIME follows optional work");
}
'''


STATE_DRIVER = r'''
int main() {
  resetBattery();
  auto sample = [](unsigned profile, unsigned percent, const char* address) {
    auto& reading = btBatteryReadings[profile]; reading.available = true;
    reading.percent = percent; reading.sampledUtc = 1704110000;
    strlcpy(reading.address, address, sizeof(reading.address));
  };
  sample(0, 60, "aa:bb:cc:dd:ee:ff"); sample(1, 30, "11:22:33:44:55:66");
  btProfileAddress[1] = "11:22:33:44:55:66";
  btPendingBatteryStatus = "No complete battery reply";
  commitBluetoothBattery();
  assert(btBatteryReadings[0].percent == 60 && btBatteryReadings[0].sampledUtc == 1704110000);
  assert(!std::strcmp(btBatteryStatuses[0], "No complete battery reply"));
  assert(btBatteryReadings[1].percent == 30); // Active-profile failure does not touch another watch.
  assert(readBluetoothWatchBattery()); btPendingBattery.percent = 0;
  commitBluetoothBattery(); // A genuine 0% remains available, not "unknown".
  assert(btBatteryReadings[0].available && btBatteryReadings[0].percent == 0);
  assert(btBatteryReadings[0].sampledUtc == 1704110400);
  assert(btBatteryReadings[1].percent == 30);

  // Failed replacement leaves the old binding/sample intact before a TIME
  // success. A successful replacement without a reading clears the old sample.
  resetBattery(); sample(0, 60, "aa:bb:cc:dd:ee:ff");
  btPendingBattery = {}; btLastWatchAddress = "77:88:99:aa:bb:cc";
  assert(btBatteryReadings[0].matches(btProfileAddress[0].c_str()));
  btProfileAddress[0] = btLastWatchAddress; commitBluetoothBattery();
  assert(!btBatteryReadings[0].available);
  resetBattery(); assert(readBluetoothWatchBattery());
  btProfileAddress[0] = "77:88:99:aa:bb:cc"; commitBluetoothBattery();
  assert(!btBatteryReadings[0].available); // A valid sample for a different address cannot attach.
  resetBattery(); sample(0, 60, "aa:bb:cc:dd:ee:ff");
  btActiveProtocol = 1; commitBluetoothBattery();
  assert(!btBatteryReadings[0].available); // Same address under another protocol is not calibrated BX data.
  assert(!CasioWatchBattery::Reading{}.matches("aa:bb:cc:dd:ee:ff"));
  assert(!btPendingBattery.matches(nullptr));
  assert(!btPendingBattery.matches("short"));
  std::puts("Battery readings remain isolated by profile, binding and protocol; read failures retain last samples, replacement clears them, and zero is valid");
}
'''


WORKFLOW_DRIVER = r'''
int main() {
  initBluetoothSync(); bind(0); bind(1);
  btActiveProfile = 0; btActiveProtocol = BT_PROTOCOL_BX5600_MIP;
  btLastWatchName = "GW-BX5600"; btLastWatchAddress = "aa:bb:cc:dd:ee:ff";
  auto& reading = btBatteryReadings[0]; reading.available = true;
  reading.percent = 30; reading.sampledUtc = fakeEpoch - 60;
  strlcpy(reading.address, btLastWatchAddress.c_str(), sizeof(reading.address));
  btBatteryReadings[1] = reading;
  batteryReadOk = true; writeResult = false;
  assert(!attemptBluetoothSync());
  assert(reading.available && reading.percent == 30 && reading.sampledUtc == fakeEpoch - 60);
  assert(btPendingBattery.available && btPendingBattery.percent == 60);
  assert(flash.empty()); // Failed TIME cannot publish/persist its pending reading.

  writeResult = true; assert(attemptBluetoothSync());
  assert(reading.available && reading.percent == 60 && reading.sampledUtc == fakeEpoch);
  assert(btBatteryReadings[1].percent == 30);
  for (const auto& file : flash) assert(file.second.find("battery") == std::string::npos);
  batteryReadOk = false; fakeEpoch += 60; assert(attemptBluetoothSync());
  assert(!btPendingBattery.available); // Attempt reset cannot reuse an older pending sample.
  assert(reading.percent == 60 && reading.sampledUtc == fakeEpoch - 60);
  assert(!std::strcmp(btBatteryStatuses[0], "Mock optional read unavailable"));

  btPairModeActive = true; btLastWatchAddress = "11:22:33:44:55:66";
  writeResult = false; assert(!attemptBluetoothSync());
  assert(btProfileAddress[0] == "aa:bb:cc:dd:ee:ff" && reading.available);
  writeResult = true; assert(attemptBluetoothSync());
  assert(btProfileAddress[0] == "11:22:33:44:55:66" && !reading.available);
  assert(btBatteryReadings[1].available && btBatteryReadings[1].percent == 30);
  std::puts("Real sync workflow commits battery only after TIME succeeds, resets pending samples, preserves last readings on failed reads, and clears replaced bindings");
}
'''


class WatchBatteryTest(unittest.TestCase):
    def run_cpp(self, source):
        self.assertIsNotNone(shutil.which('g++'))
        with tempfile.TemporaryDirectory(prefix='radioclock-battery-') as tmp:
            unit, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            unit.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_captured_battery_packets_and_calibration(self):
        fixture = json.loads((ROOT / 'tests/fixtures/watch-battery.json').read_text())
        source = FIRMWARE.read_text()
        time_channel = re.search(r'#define CASIO_SET_CHAR\s+"([^"]+)"', source).group(1)
        sp_channel = re.search(r'#define CASIO_SP_DATA_CHAR\s+"([^"]+)"', source).group(1)
        self.assertEqual(fixture['reply_channel'], time_channel)
        self.assertNotEqual(fixture['reply_channel'], sp_channel)
        cases = []
        for i, packet in enumerate(fixture['packets']):
            data = bytes.fromhex(packet['reply'])
            cases.append('const uint8_t capture' + str(i) + '[]={' + ','.join(map(str, data)) + '};')
            cases.append('uint8_t percent' + str(i) + '=251;')
            cases.append('assert(CasioWatchBattery::decodeBx(capture' + str(i) + ',sizeof(capture' + str(i) + '),percent' + str(i) + '));')
            cases.append('assert(percent' + str(i) + '==' + str(packet['percent']) + ');')
        self.run_cpp(DECODE_DRIVER.replace('FIXTURE_CASES', '\n'.join(cases)))

    def test_notification_routing_and_bounded_wait(self):
        source = FIRMWARE.read_text()
        functions = '\n'.join(extract_function(source, name) for name in
                              ['cancelBtResponse', 'gshockNotifyCallback',
                               'prepareBtResponse', 'waitBt', 'copyBt'])
        # unsigned long is 32-bit on ESP32 but 64-bit on this host. Keep the
        # target's timestamp width for the millis-wrap check.
        functions = functions.replace('unsigned long last =', 'uint32_t last =')
        self.run_cpp(NOTIFICATION_MOCKS + functions + NOTIFICATION_DRIVER)

    def transport_source(self, driver):
        source = FIRMWARE.read_text()
        sp_source = (ROOT / 'tests/test_bx_protocol.cpp').read_text()
        captures = []
        for name in ['settingsReply', 'dstReply', 'namesReply']:
            block = re.search(r'static const Bytes ' + name + r' = hex\((.*?)\);',
                              sp_source, re.S).group(1)
            data = bytes.fromhex(''.join(re.findall(r'"([0-9a-f]+)"', block)))
            captures.append('const std::vector<uint8_t> ' + name + '={' + ','.join(map(str, data)) + '};')
        string_mock = workflow.balanced_block(workflow.MOCKS, workflow.MOCKS.index('class String')) + ';'
        functions = '\n'.join(extract_function(source, name) for name in
                              ['cancelBtResponse', 'gshockNotifyCallback',
                               'prepareBtResponse', 'waitBt', 'copyBt',
                               'readBluetoothWatchBattery', 'commitBluetoothBattery',
                               'performGShockBX5600Sync'])
        functions = functions.replace('unsigned long last =', 'uint32_t last =')
        # Inject a transport-copy fault while exercising the actual buffer-copy
        # implementation. The read must reject a size mismatch rather than
        # decode a valid-looking truncated prefix.
        functions = functions.replace('static size_t copyBt(', 'static size_t copyBtImpl(')
        copy_wrapper = ('\nstatic size_t copyBt(uint8_t* dst, size_t maxLen) { '
                        'return copyBtImpl(dst, std::min(maxLen, copyLimit)); }\n')
        functions = functions.replace('static bool readBluetoothWatchBattery(',
                                      copy_wrapper + 'static bool readBluetoothWatchBattery(', 1)
        return '\n'.join([NOTIFICATION_MOCKS, string_mock, '\n'.join(captures),
                          TRANSPORT_MOCKS, functions, driver])

    def test_optional_read_failures_and_fresh_time_session(self):
        self.run_cpp(self.transport_source(TRANSPORT_DRIVER))

    def test_profile_binding_and_last_known_sample(self):
        self.run_cpp(self.transport_source(STATE_DRIVER))

    def test_success_only_commit_and_failed_pairing_workflow(self):
        unit = workflow.BluetoothWorkflowTest().unit_source(FIRMWARE.read_text())
        old_write = workflow.extract_function(workflow.HELPERS, 'performGShockBX5600Sync')
        new_write = r'''
bool batteryReadOk = true;
bool performGShockBX5600Sync() {
  ++writes; lastWriteProtocol = BT_PROTOCOL_BX5600_MIP;
  if (batteryReadOk) {
    btPendingBattery.available = true; btPendingBattery.percent = 60;
    btPendingBattery.sampledUtc = fakeEpoch;
    strlcpy(btPendingBattery.address, btLastWatchAddress.c_str(), sizeof(btPendingBattery.address));
    btPendingBatteryStatus = "Read during sync";
  } else btPendingBatteryStatus = "Mock optional read unavailable";
  return writeResult;
}
'''
        unit = unit.replace(old_write, new_write, 1)
        bind = workflow.extract_function(workflow.DRIVER, 'bind')
        self.run_cpp(unit.replace(workflow.DRIVER, bind + WORKFLOW_DRIVER, 1))

    def test_battery_has_no_flash_persistence(self):
        source = FIRMWARE.read_text()
        for name in ['loadConfig', 'writeConfigNow', 'readBluetoothWatchBattery',
                     'commitBluetoothBattery']:
            body = extract_function(source, name)
            if name in ['loadConfig', 'writeConfigNow']:
                self.assertNotRegex(body, r'bt(?:Pending)?Battery')
            else:
                self.assertNotRegex(body, r'\b(?:saveConfig|writeConfigNow|LittleFS)\b')


if __name__ == '__main__':
    unittest.main()
