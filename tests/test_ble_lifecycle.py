#!/usr/bin/env python3
"""Exercise the firmware's real BLE lifecycle and write helpers with host mocks.

The mock GAP disconnect calls onDisconnect before clearing its connection handle,
matching NimBLE-Arduino 2.5.1. This catches the teardown ordering behind the
reported LoadProhibited crash. Hardware/radio behavior still needs device tests.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/RadioClock_V3_2_2_Casio_BLE_Reliability/RadioClock_V3_2_2_Casio_BLE_Reliability.ino'


def extract_function(source, name):
    match = re.search(r'^static (?:bool|void|size_t) ' + name + r'\([^;]+?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f'Function {name} missing')
    depth = 1
    end = match.end()
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[match.start():end]


MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BT_REQUEST_TIMEOUT_MS 5000UL
#define BT_PROTOCOL_BX5600_MIP 0
#define CASIO_WATCH_FEATURES_SERVICE "service"
#define CASIO_SP_REQUEST_CHAR "request"
#define CASIO_SP_DATA_CHAR "data"
#define CASIO_SET_CHAR "time"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
class String {
public:
  std::string s;
  String(const char* p = "") : s(p) {}
  String& operator+=(const char* p) { s += p; return *this; }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  void trim() { while(!s.empty() && s.back() == ' ') s.pop_back(); }
};
struct SerialMock {
  void println(const char*) {}
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
uint32_t tick = 0;
uint32_t millis() { return tick; }
void delay(unsigned long ms);
struct ble_npl_event { void (*fn)(ble_npl_event*) = nullptr; };
struct ble_npl_callout { ble_npl_event ev; bool active = false; uint32_t expiry = 0; };
std::vector<ble_npl_callout*> hostTimers;
bool failMonitorReset = false;
#define BLE_NPL_OK 0
uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
int ble_npl_callout_init(ble_npl_callout* co, void*, void (*fn)(ble_npl_event*), void*) {
  co->ev.fn = fn; hostTimers.push_back(co); return 0;
}
int ble_npl_callout_reset(ble_npl_callout* co, uint32_t ticks) {
  if (failMonitorReset) return -1;
  co->active = true; co->expiry = tick + ticks; return BLE_NPL_OK;
}
void ble_npl_callout_stop(ble_npl_callout* co) { co->active = false; }
void ble_npl_callout_deinit(ble_npl_callout* co) {
  assert(!co->active);
  hostTimers.erase(std::remove(hostTimers.begin(),hostTimers.end(),co),hostTimers.end());
  co->ev.fn = nullptr;
}
std::deque<ble_npl_event*> hostEvents;
void ble_npl_event_init(ble_npl_event* e, void (*fn)(ble_npl_event*), void*) { e->fn = fn; }
void ble_npl_event_deinit(ble_npl_event* e) { e->fn = nullptr; }
void* nimble_port_get_dflt_eventq() { return nullptr; }
void ble_npl_eventq_put(void*, ble_npl_event* e) {
  if (std::find(hostEvents.begin(),hostEvents.end(),e) == hostEvents.end()) hostEvents.push_back(e);
}
void ble_npl_eventq_remove(void*, ble_npl_event* e) {
  hostEvents.erase(std::remove(hostEvents.begin(),hostEvents.end(),e),hostEvents.end());
}
struct NimBLEUUID {
  std::string uuid;
  NimBLEUUID(const char* u) : uuid(u) {}
  std::string toString() const { return uuid; }
};
struct NimBLEAddress { NimBLEAddress(std::string, uint8_t) {} };
struct NimBLERemoteCharacteristic {
  bool response = false, noResponse = false, notify = false, indicate = false;
  bool lastWriteResponse = false, writeResult = true;
  int writes = 0;
  std::string uuid = "test";
  std::function<void()> onWrite;
  bool canWrite() const { return response; }
  bool canWriteNoResponse() const { return noResponse; }
  bool canNotify() const { return notify; }
  bool canIndicate() const { return indicate; }
  NimBLEUUID getUUID() const { return NimBLEUUID(uuid.c_str()); }
  bool writeValue(const uint8_t*, size_t, bool r) {
    lastWriteResponse = r; ++writes;
    if (onWrite) onWrite();
    return writeResult;
  }
  bool subscribe(bool, void (*)(NimBLERemoteCharacteristic*,uint8_t*,size_t,bool),bool) { return true; }
};
NimBLERemoteCharacteristic requestChar, dataChar, timeChar;
struct NimBLERemoteService {
  bool missingRequest = false;
  std::vector<NimBLERemoteCharacteristic*> chars{&requestChar,&dataChar,&timeChar};
  NimBLERemoteCharacteristic* getCharacteristic(const NimBLEUUID& u) {
    if (u.uuid == CASIO_SP_REQUEST_CHAR) return missingRequest ? nullptr : &requestChar;
    if (u.uuid == CASIO_SP_DATA_CHAR) return &dataChar;
    return &timeChar;
  }
  const auto& getCharacteristics() { return chars; }
} featureService;
class NimBLEClient;
class NimBLEClientCallbacks {
public:
  virtual void onConnect(NimBLEClient*) {}
  virtual void onConnectFail(NimBLEClient*, int) {}
  virtual void onDisconnect(NimBLEClient*, int) {}
};
struct NimBLEClient {
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  bool pendingConnect = false, pendingDisconnect = false, missingService = false;
  bool failStart = false;
  bool selfDelete = false;
  int connectCalls = 0;
  NimBLEClientCallbacks* callbacks = nullptr;
  uint16_t getConnHandle() const { return handle; }
  bool isConnected() const { return handle != BLE_HS_CONN_HANDLE_NONE && !pendingDisconnect; }
  void setClientCallbacks(NimBLEClientCallbacks* c, bool) { callbacks = c; }
  void setSelfDelete(bool a, bool b) { selfDelete = a || b; }
  void setConnectTimeout(int) {}
  void setConnectRetries(int) {}
  bool connect(const NimBLEAddress&,bool refresh,bool asynchronous) {
    assert(refresh && asynchronous);
    ++connectCalls;
    if(failStart) return false;
    pendingConnect = true;
    handle = 1;
    return true;
  }
  bool disconnect() { pendingDisconnect = true; pendingConnect = false; return true; }
  bool cancelConnect() { pendingDisconnect = true; pendingConnect = false; return true; }
  NimBLERemoteService* getService(NimBLEUUID) { return missingService ? nullptr : &featureService; }
} retainedClient;
NimBLEClient* btClient = nullptr;
NimBLERemoteService* btService = nullptr;
NimBLERemoteCharacteristic *btSpRequest = nullptr, *btSpData = nullptr, *btSetChar = nullptr;
struct NimBLEDevice {
  static int allocations;
  static NimBLEClient* createClient() { ++allocations; return &retainedClient; }
  static void deleteClient(NimBLEClient*) { assert(false && "Client must remain alive during GAP teardown"); }
};
int NimBLEDevice::allocations = 0;
bool btBleBusy = false, btBleInitialized = true, rfDemand = false, allowHostProgress = true;
std::function<void()> nextDelayHook;
String btLastWatchAddress("aa:bb:cc:dd:ee:ff");
uint8_t btLastWatchAddressType = 0;
uint32_t btConnectionAttempts = 0, btWriteAcknowledgements = 0, btResponseErrors = 0;
size_t btResponseLength = 0;
unsigned long btLastFragmentMillis = 0;
bool btResponseOverflow = false, btResponseActive = false;
uint8_t btExpectedHeader = 0, btResponseBuffer[512] = {};
static bool bleOperationCancelled() { return rfDemand; }
static void setBluetoothPhase(const char*) {}
static void cancelBtResponse() { btResponseActive = false; }
static void prepareBtResponse(uint8_t header) {
  btResponseActive = true; btExpectedHeader = header; btResponseLength = 0;
  btLastFragmentMillis = millis(); btResponseOverflow = false;
}
static void gshockNotifyCallback(NimBLERemoteCharacteristic*,uint8_t*,size_t,bool) {}
'''

DRIVER = r'''
bool observedCallbackBarrier = false, observedGapBarrier = false;
void delay(unsigned long ms) {
  tick += ms;
  if (nextDelayHook) { auto hook = std::move(nextDelayHook); nextDelayHook = {}; hook(); }
  if (!allowHostProgress) return;
  for(auto* co : hostTimers) {
    if(co->active && tick >= co->expiry) {
      co->active = false;
      ble_npl_eventq_put(nullptr,&co->ev);
    }
  }
  if (!hostEvents.empty()) {
    auto* e = hostEvents.front(); hostEvents.pop_front(); e->fn(e);
  } else if (retainedClient.pendingDisconnect) {
    retainedClient.callbacks->onDisconnect(&retainedClient, 534);
    assert(!btClientQuiescent());
    observedCallbackBarrier = true;
    // These library writes happen AFTER the application callback.
    retainedClient.handle = BLE_HS_CONN_HANDLE_NONE;
    retainedClient.pendingDisconnect = false;
    assert(!btClientQuiescent());
    observedGapBarrier = true;
  } else if (retainedClient.pendingConnect) {
    retainedClient.pendingConnect = false;
    retainedClient.callbacks->onConnect(&retainedClient);
  }
}
int main() {
  requestChar.noResponse = true;
  requestChar.uuid = "request";
  dataChar.response = dataChar.notify = true;
  dataChar.uuid = "data";
  timeChar.response = true;
  timeChar.uuid = "time";
  // A genuine SP_REQUEST Write Without Response characteristic is accepted.
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btBleBusy && !btClientQuiescent());
  assert(!retainedClient.selfDelete);
  assert(NimBLEDevice::allocations == 1);
  uint8_t payload[] = {0x05};
  assert(writeBt(btSpRequest,payload,sizeof(payload),false));
  assert(!requestChar.lastWriteResponse && btWriteAcknowledgements == 0);
  assert(!writeBt(btSpRequest,payload,sizeof(payload),true));
  assert(writeBt(btSpData,payload,sizeof(payload),true));
  assert(dataChar.lastWriteResponse && btWriteAcknowledgements == 1);
  // RF demand cancels both writes and notification waits immediately.
  rfDemand = true;
  int previousWrites = dataChar.writes;
  assert(!writeBt(btSpData,payload,sizeof(payload),true));
  assert(dataChar.writes == previousWrites);
  btResponseActive = true;
  uint32_t started = tick;
  assert(!waitBt(1) && !btResponseActive && tick == started);
  rfDemand = false;
  // Simulate loopTask blocked inside an ATT write when RF becomes due. The
  // host callout must disconnect independently, without awaiting ATT timeout.
  dataChar.onWrite = [] {
    rfDemand = true;
    uint32_t started = tick;
    while (!btClientQuiescent() && tick - started < 100) delay(5);
    assert(btClientQuiescent() && tick - started < 100);
    rfDemand = false;
    dataChar.writeResult = false; // ATT operation released by disconnect.
  };
  assert(!writeBt(btSpData,payload,sizeof(payload),true));
  dataChar.writeResult = true;
  dataChar.onWrite = {};
  disconnectGShock();
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  // A too-large complete response must never be echoed as a truncated prefix.
  requestChar.onWrite = [] {
    std::memset(btResponseBuffer,0,25); btResponseBuffer[0] = 0x05;
    btResponseLength = 25; btLastFragmentMillis = millis();
  };
  uint8_t reply[20];
  assert(bxRequest(payload,sizeof(payload),0x05,16,reply,sizeof(reply)) == 0);
  requestChar.onWrite = {};
  // Teardown waits past onDisconnect and the remaining GAP-handler writes.
  disconnectGShock();
  assert(observedCallbackBarrier && observedGapBarrier);
  assert(btClient == &retainedClient && btClientQuiescent() && !btBleBusy);
  // Missing characteristics fail safely, retaining the same client object.
  featureService.missingRequest = true;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent() && !btBleBusy && NimBLEDevice::allocations == 1);
  featureService.missingRequest = false;
  dataChar.response = false;
  dataChar.noResponse = true;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent());
  dataChar.response = true;
  timeChar.response = false;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent());
  timeChar.response = true;
  // A schedule becoming due during asynchronous connection also cancels safely.
  nextDelayHook = [] { rfDemand = true; };
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent() && !btBleBusy);
  rfDemand = false;
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  // A stalled host cannot permit client reuse or RF ownership handoff.
  allowHostProgress = false;
  disconnectGShock();
  assert(btBleBusy && !btClientQuiescent());
  int attempts = retainedClient.connectCalls;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(retainedClient.connectCalls == attempts && NimBLEDevice::allocations == 1);
  allowHostProgress = true;
  disconnectGShock();
  assert(btClientQuiescent());
  releaseBtClientBarrier();
  assert(!btClientBarrierEventInitialized && !btClientPreemptionInitialized && hostEvents.empty());
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientBarrierEventInitialized && NimBLEDevice::allocations == 1);
  disconnectGShock();
  // Immediate start failure needs no nonexistent GAP disconnect event.
  retainedClient.failStart = true;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent() && !btBleBusy);
  retainedClient.failStart = false;
  // Monitoring failure also closes the connection instead of weakening RF priority.
  failMonitorReset = true;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent());
  failMonitorReset = false;
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  failMonitorReset = true;
  delay(25);
  assert(!btClientPreemptionEnabled.load());
  assert(!writeBt(btSpData,payload,sizeof(payload),true));
  disconnectGShock();
  assert(btClientQuiescent());
  failMonitorReset = false;
  std::puts("BLE lifecycle, RF cancellation, characteristic capabilities and write modes passed");
}
'''


class BleLifecycleTest(unittest.TestCase):
    def test_real_firmware_helpers(self):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for host lifecycle tests')
        source = FIRMWARE.read_text()
        start = source.index('static std::atomic<bool> btClientConnected')
        end = source.index('static void gshockNotifyCallback', start)
        lifecycle = source[start:end]
        names = ['waitBt', 'copyBt', 'writeBt', 'disconnectGShock',
                 'validateBtCharacteristic', 'connectGShock', 'bxRequest']
        functions = '\n\n'.join(extract_function(source, n) for n in names)
        with tempfile.TemporaryDirectory(prefix='radioclock-ble-test-') as tmp:
            unit = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            unit.write_text(MOCKS + '\n' + lifecycle + '\n' + functions + '\n' + DRIVER)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
