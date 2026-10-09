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
FIRMWARE = ROOT / 'firmware/RadioClock_V4_14/RadioClock_V4_14.ino'


def extract_function(source, name):
    match = re.search(r'^static (?:bool|void|size_t|int|const char\s*\*)\s*' + name + r'\([^;]+?\)\s*\{', source, re.M)
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
#include "CasioBxProtocol.h"
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_NPL_TIME_FOREVER UINT32_MAX
#define BLE_HS_EDONE 14
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ATT_ERR(error) (0x100 + (error))
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 8
#define BLE_ATT_ERR_INSUFFICIENT_ENC 15
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 3
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
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
uint32_t internalFreeBytes = 262144, internalLargestBytes = 131072;
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
uint32_t heap_caps_get_free_size(int) { return internalFreeBytes; }
uint32_t heap_caps_get_largest_free_block(int) { return internalLargestBytes; }
uint32_t hostStackBytes = 8192;
uint32_t uxTaskGetStackHighWaterMark(void*) { return hostStackBytes; }
void applicationProgressCheckpoint() {}
int controlledRecoveries = 0;
void applicationForceSafeRestart() { ++controlledRecoveries; }
void delay(unsigned long ms);
struct ble_npl_event { void (*fn)(ble_npl_event*) = nullptr; };
struct ble_npl_callout { ble_npl_event ev; bool active = false; uint32_t expiry = 0; };
std::vector<ble_npl_callout*> hostTimers;
bool failMonitorReset = false;
#define BLE_NPL_OK 0
#define NIMBLE_RADIOCLOCK_CALLOUT_INIT ble_npl_callout_init
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
  bool operator==(const NimBLEUUID& other) const { return uuid == other.uuid; }
  bool operator!=(const NimBLEUUID& other) const { return !(*this == other); }
};
struct NimBLEAddress { NimBLEAddress(std::string, uint8_t) {} };
struct NimBLERemoteCharacteristic {
  bool response = false, noResponse = false, notify = false, indicate = false;
  bool lastWriteResponse = false, writeResult = true;
  bool subscribeResult = true, subscribed = false;
  int writes = 0, subscriptions = 0;
  uint16_t attributeHandle = 0;
  std::deque<int> completionStatuses;
  std::vector<std::vector<uint8_t>> payloads;
  std::string uuid = "test";
  std::function<void()> onWrite;
  bool canWrite() const { return response; }
  bool canWriteNoResponse() const { return noResponse; }
  bool canNotify() const { return notify; }
  bool canIndicate() const { return indicate; }
  NimBLEUUID getUUID() const { return NimBLEUUID(uuid.c_str()); }
  uint16_t getHandle() const { return attributeHandle; }
  bool subscribe(bool, void (*)(NimBLERemoteCharacteristic*,uint8_t*,size_t,bool),bool) {
    ++subscriptions; subscribed = subscribeResult; return subscribeResult;
  }
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
std::function<void()> serviceDiscoveryHook;
class NimBLEClientCallbacks {
public:
  virtual void onConnect(NimBLEClient*) {}
  virtual void onConnectFail(NimBLEClient*, int) {}
  virtual void onDisconnect(NimBLEClient*, int) {}
  virtual void onMTUChange(NimBLEClient*, uint16_t) {}
};
struct NimBLEClient {
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  bool pendingConnect = false, pendingDisconnect = false, missingService = false;
  bool failStart = false, securityResult = true;
  uint16_t mtu = 255;
  bool selfDelete = false;
  int connectCalls = 0, securityCalls = 0;
  NimBLEClientCallbacks* callbacks = nullptr;
  uint16_t getConnHandle() const { return handle; }
  uint16_t getMTU() const { return mtu; }
  bool secureConnection() { ++securityCalls; return securityResult; }
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
  NimBLERemoteService* getService(NimBLEUUID) {
    if (serviceDiscoveryHook) serviceDiscoveryHook();
    return missingService || handle == BLE_HS_CONN_HANDLE_NONE ? nullptr : &featureService;
  }
} retainedClient;
struct ble_gatt_error { int status; uint16_t att_handle = 0; };
struct ble_gatt_attr {};
struct NimBLETaskData {
  int m_flags = 0;
  bool released = false;
  void* m_pBuf = nullptr;
  explicit NimBLETaskData(void* = nullptr, int flags = 0, void* buffer = nullptr)
      : m_flags(flags), m_pBuf(buffer) {}
};
std::function<void()> pendingGattCallback;
bool holdGattUntilDisconnect = false;
struct NimBLEUtils {
  static bool taskWait(NimBLETaskData& task, uint32_t) {
    const uint32_t started = tick;
    while (!task.released && tick - started < 10000) delay(5);
    assert(task.released); return true;
  }
  static void taskRelease(const NimBLETaskData& task, int status = 0) {
    auto& result = const_cast<NimBLETaskData&>(task);
    result.m_flags = status; result.released = true;
  }
  static const char* returnCodeToString(int) { return "mock ATT status"; }
};
NimBLERemoteCharacteristic& mockAttribute(uint16_t handle) {
  for (auto* c : featureService.chars) if (c->attributeHandle == handle) return *c;
  assert(false && "Unknown GATT handle"); return dataChar;
}
int mockWrite(uint16_t handle, const void* bytes, uint16_t length, bool response) {
  auto& c = mockAttribute(handle);
  c.lastWriteResponse = response; ++c.writes;
  const auto* data = static_cast<const uint8_t*>(bytes);
  c.payloads.emplace_back(data, data + length);
  if (c.onWrite) c.onWrite();
  if (!c.completionStatuses.empty()) {
    int status = c.completionStatuses.front(); c.completionStatuses.pop_front(); return status;
  }
  return c.writeResult ? 0 : BLE_HS_ENOTCONN;
}
int ble_gattc_write_no_rsp_flat(uint16_t connection, uint16_t handle, const void* bytes, uint16_t length) {
  assert(connection != BLE_HS_CONN_HANDLE_NONE); return mockWrite(handle, bytes, length, false);
}
int ble_gattc_write_flat(uint16_t connection, uint16_t handle, const void* bytes, uint16_t length,
                        int (*callback)(uint16_t,const ble_gatt_error*,ble_gatt_attr*,void*),void* argument) {
  assert(connection != BLE_HS_CONN_HANDLE_NONE);
  assert(!pendingGattCallback);
  const int status = mockWrite(handle, bytes, length, true);
  pendingGattCallback = [=] {
    const ble_gatt_error error{retainedClient.handle == BLE_HS_CONN_HANDLE_NONE ? BLE_HS_ENOTCONN : status};
    callback(connection, &error, nullptr, argument);
  };
  return 0;
}
NimBLEClient* btClient = nullptr;
NimBLERemoteService* btService = nullptr;
NimBLERemoteCharacteristic *btSpRequest = nullptr, *btSpData = nullptr, *btSetChar = nullptr;
struct NimBLEDevice {
  static int allocations;
  static uint16_t preferredMtu;
  static NimBLEClient* createClient() { ++allocations; return &retainedClient; }
  static bool setMTU(uint16_t value) { preferredMtu = value; return true; }
  static void deleteClient(NimBLEClient*) { assert(false && "Client must remain alive during GAP teardown"); }
};
int NimBLEDevice::allocations = 0;
uint16_t NimBLEDevice::preferredMtu = 0;
bool btBleBusy = false, btBleInitialized = true, rfDemand = false, allowHostProgress = true;
std::function<void()> nextDelayHook;
String btLastWatchAddress("aa:bb:cc:dd:ee:ff");
uint8_t btLastWatchAddressType = 0;
uint32_t btConnectionAttempts = 0, btWriteAcknowledgements = 0, btResponseErrors = 0, btNotifications = 0;
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
'''

DRIVER = r'''
bool observedCallbackBarrier = false, observedGapBarrier = false;
void delay(unsigned long ms) {
  tick += ms;
  if (nextDelayHook) { auto hook = std::move(nextDelayHook); nextDelayHook = {}; hook(); }
  if (!allowHostProgress) return;
  for(auto* co : hostTimers) {
    if(co->active && static_cast<int32_t>(tick - co->expiry) >= 0) {
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
  if (pendingGattCallback && ((!rfDemand && !holdGattUntilDisconnect && !retainedClient.pendingDisconnect) || btClientQuiescent())) {
    auto callback = std::move(pendingGattCallback); pendingGattCallback = {}; callback();
  }
}
int main() {
  requestChar.noResponse = true;
  requestChar.uuid = "request";
  requestChar.attributeHandle = 1;
  dataChar.response = dataChar.notify = true;
  dataChar.uuid = "data";
  dataChar.attributeHandle = 2;
  timeChar.response = true;
  timeChar.uuid = "time";
  timeChar.attributeHandle = 3;
  // A genuine SP_REQUEST Write Without Response characteristic is accepted.
  assert(bluetoothHostStackMinimum() == 0); // Unknown before the first host callback.
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(bluetoothHostStackMinimum() == 8192);
  hostStackBytes = 6144;
  delay(25);
  assert(bluetoothHostStackMinimum() == 6144);
  hostStackBytes = 8192;
  assert(btBleBusy && !btClientQuiescent());
  assert(!retainedClient.selfDelete);
  assert(NimBLEDevice::allocations == 1);
  assert(NimBLEDevice::preferredMtu == 517);
  assert(dataChar.subscribed && dataChar.subscriptions == 1);
  assert(requestChar.subscriptions == 0 && timeChar.subscriptions == 0);
  uint8_t payload[] = {0x05};
  assert(writeBt(btSpRequest,payload,sizeof(payload),false));
  assert(!requestChar.lastWriteResponse && btWriteAcknowledgements == 0);
  assert(!writeBt(btSpRequest,payload,sizeof(payload),true));
  assert(writeBt(btSpData,payload,sizeof(payload),true));
  assert(dataChar.lastWriteResponse && btWriteAcknowledgements == 1);
  // TIME emits unrelated notifications on real watches. Even a matching
  // header must not start, or append to, the SP_DATA response stream.
  prepareBtResponse(0x05);
  uint8_t first[] = {0x05,0x11};
  uint8_t continuation[] = {0x22,0x33};
  gshockNotifyCallback(btSetChar,first,sizeof(first),true);
  assert(btResponseLength == 0);
  gshockNotifyCallback(btSpData,first,sizeof(first),true);
  assert(btResponseLength == sizeof(first));
  gshockNotifyCallback(btSetChar,continuation,sizeof(continuation),true);
  assert(btResponseLength == sizeof(first));
  gshockNotifyCallback(btSpData,continuation,sizeof(continuation),true);
  assert(btResponseLength == sizeof(first) + sizeof(continuation));
  cancelBtResponse();
  // The complete 133-byte settings echo needs one ATT Write Request. A low
  // MTU must reject it without submitting or silently truncating any prefix.
  std::vector<uint8_t> settings(133);
  for (size_t i = 0; i < settings.size(); ++i) settings[i] = static_cast<uint8_t>(i);
  int writesBeforeMtuGuard = dataChar.writes;
  retainedClient.mtu = 23;
  assert(!writeBt(btSpData,settings.data(),settings.size(),true));
  assert(dataChar.writes == writesBeforeMtuGuard);
  retainedClient.mtu = CasioBxProtocol::kMinimumMtu;
  assert(writeBt(btSpData,settings.data(),settings.size(),true));
  assert(dataChar.payloads.back() == settings);
  // An ATT rejection must be reported as failure and never counted as an ack.
  uint32_t acknowledgements = btWriteAcknowledgements;
  dataChar.completionStatuses.push_back(BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN));
  assert(!writeBt(btSpData,settings.data(),settings.size(),true));
  assert(btWriteAcknowledgements == acknowledgements);
  // Authentication retry repeats the complete original packet exactly once.
  int securityBeforeRetry = retainedClient.securityCalls;
  size_t payloadsBeforeRetry = dataChar.payloads.size();
  dataChar.completionStatuses = {BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN),0};
  assert(writeBt(btSpData,settings.data(),settings.size(),true));
  assert(retainedClient.securityCalls == securityBeforeRetry + 1);
  assert(dataChar.payloads.size() == payloadsBeforeRetry + 2);
  assert(dataChar.payloads[payloadsBeforeRetry] == settings && dataChar.payloads.back() == settings);
  assert(btWriteAcknowledgements == acknowledgements + 1);
  dataChar.completionStatuses = {BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN),
                                 BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN)};
  assert(!writeBt(btSpData,settings.data(),settings.size(),true));
  assert(dataChar.completionStatuses.empty());
  assert(retainedClient.securityCalls == securityBeforeRetry + 2);
  retainedClient.mtu = 255;
  // Font replies use the basic-settings channel, never the SP_DATA stream.
  timeChar.uuid = CASIO_SET_CHAR;
  dataChar.uuid = CASIO_SP_DATA_CHAR;
  btExpectedHeader=0x13;btResponseActive=true;btResponseLength=0;btResponseOverflow=false;
  uint8_t basic[17]={0x13};
  gshockNotifyCallback(&dataChar,basic,sizeof(basic),true);assert(btResponseLength==0);
  gshockNotifyCallback(&timeChar,basic,9,true);assert(btResponseLength==9);
  gshockNotifyCallback(&timeChar,basic+9,8,true);assert(btResponseLength==17 && !btResponseOverflow);
  btResponseActive=false;
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
  };
  started = tick;
  assert(!writeBt(btSpData,payload,sizeof(payload),true));
  assert(btClientQuiescent() && tick - started < 100);
  rfDemand = false;
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
  // Notifications on TIME cannot stand in for the required SP_DATA stream.
  dataChar.notify = false;
  timeChar.notify = true;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent());
  assert(timeChar.subscriptions == 0);
  dataChar.notify = true;
  dataChar.subscribeResult = false;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent());
  dataChar.subscribeResult = true;
  // Negotiation that remains at ATT MTU 23 cannot run this protocol safely.
  retainedClient.mtu = 23;
  started = tick;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(tick - started >= 2000 && btClientQuiescent());
  retainedClient.mtu = 255;
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
  // Internal memory admission rejects an attempt before SDK allocation and
  // does not confuse a large total heap with a fragmented internal heap.
  assert(bluetoothMemoryAvailable(true));
  internalLargestBytes = 4096;
  int connectionsBeforePressure = retainedClient.connectCalls;
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(retainedClient.connectCalls == connectionsBeforePressure && !btBleBusy);
  internalLargestBytes = 131072;
  internalFreeBytes = 16384;
  assert(!bluetoothMemoryAvailable() && !bluetoothMemoryAvailable(true));
  internalFreeBytes = 262144;
  // Total-transaction timeout interrupts a blocked ATT write on the host.
  // The callback is cancelled and released before stack task storage expires.
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  holdGattUntilDisconnect = true;
  dataChar.onWrite = [] { tick = btTransactionStartedMillis.load() + BT_TRANSACTION_DEADLINE_MS - 20; };
  assert(!writeBt(btSpData,payload,sizeof(payload),true));
  assert(!pendingGattCallback && btClientQuiescent());
  dataChar.onWrite = {};
  holdGattUntilDisconnect = false;
  disconnectGShock();
  assert(!btTransactionActive.load());
  // Discovery uses synchronous SDK waits too; the same independent deadline
  // disconnects that procedure, including a millis() wrap during the wait.
  tick = UINT32_MAX - 255;
  serviceDiscoveryHook = [] {
    while (retainedClient.handle != BLE_HS_CONN_HANDLE_NONE) delay(5);
  };
  assert(!connectGShock(BT_PROTOCOL_BX5600_MIP));
  assert(btClientQuiescent() && !btBleBusy && !btTransactionActive.load());
  serviceDiscoveryHook = {};
  // Quarantine can recover from a late callback. A permanently stalled GAP
  // teardown instead requests one bounded, RF-safe recovery after 60 seconds.
  assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
  allowHostProgress = false;
  disconnectGShock();
  assert(!serviceBtClientQuarantine() && btClientQuarantineActive);
  tick += 59999;
  assert(!serviceBtClientQuarantine() && controlledRecoveries == 0);
  tick += 1;
  assert(!serviceBtClientQuarantine() && controlledRecoveries == 1);
  allowHostProgress = true;
  disconnectGShock();
  assert(btClientQuiescent() && !btClientQuarantineActive);
  // Repeated quiescent shutdown/reinitialization keeps exactly one retained
  // client and leaves no queued application event/callout after each cycle.
  for (int cycle = 0; cycle < 1000; ++cycle) {
    assert(connectGShock(BT_PROTOCOL_BX5600_MIP));
    assert(writeBt(btSpData,payload,sizeof(payload),true));
    disconnectGShock();
    releaseBtClientBarrier();
    assert(hostTimers.empty() && hostEvents.empty());
    assert(NimBLEDevice::allocations == 1);
  }
  std::puts("BLE lifecycle, RF cancellation, MTU guards, ATT errors, authentication retry and SP_DATA notifications passed");
}
'''


class BleLifecycleTest(unittest.TestCase):
    def run_real_firmware_helpers(self, sanitized=False):
        self.assertIsNotNone(shutil.which('g++'), 'g++ is required for host lifecycle tests')
        source = FIRMWARE.read_text()
        start = source.index('static std::atomic<bool> btClientConnected')
        end = source.index('static void gshockNotifyCallback', start)
        lifecycle = source[start:end]
        names = ['gshockNotifyCallback', 'waitBt', 'copyBt', 'btWriteComplete', 'writeBt', 'disconnectGShock',
                 'validateBtCharacteristic', 'connectGShock', 'bxRequest']
        stage = re.search(r'^static const char\s*\*btGattStage\s*=\s*[^;]+;', source, re.M)
        self.assertIsNotNone(stage, 'GATT stage diagnostic state missing')
        functions = stage.group(0) + '\n\n' + '\n\n'.join(extract_function(source, n) for n in names)
        with tempfile.TemporaryDirectory(prefix='radioclock-ble-test-') as tmp:
            unit = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test'
            unit.write_text(MOCKS + '\n' + lifecycle + '\n' + functions + '\n' + DRIVER)
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-I', str(FIRMWARE.parent), str(unit), '-o', str(binary)]
            if sanitized:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

    def test_real_firmware_helpers(self):
        self.run_real_firmware_helpers()

    def test_real_firmware_helpers_sanitized(self):
        self.run_real_firmware_helpers(sanitized=True)


if __name__ == '__main__':
    unittest.main()
