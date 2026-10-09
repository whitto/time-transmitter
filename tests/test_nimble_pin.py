#!/usr/bin/env python3
"""Exercise the shipped NimBLE source corrections and dependency integrity."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_bluetooth_workflow import balanced_block

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "libraries/NimBLE-Arduino"


def function(path, signature):
    source = (LIB / path).read_text()
    return balanced_block(source, source.index(signature))


class NimblePinTest(unittest.TestCase):
    def cpp(self, source, expected=0, sanitized=False):
        with tempfile.TemporaryDirectory(prefix="radioclock-nimble-pin-") as tmp:
            unit, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
            unit.write_text(source)
            command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(binary)]
            if sanitized:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, expected)

    def test_exact_source_inventory(self):
        subprocess.run(["python3", str(ROOT / "scripts/validate-nimble-source.py")], check=True)
        # Extra source files would be compiled by Arduino and must be rejected.
        with tempfile.TemporaryDirectory(prefix="radioclock-extra-source-") as tmp:
            import shutil
            copy = Path(tmp) / "library"
            shutil.copytree(LIB, copy)
            (copy / "src/unreviewed.cpp").write_text("void unintended() {}\n")
            result = subprocess.run(["python3", str(ROOT / "scripts/validate-nimble-source.py"), str(copy)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unreviewed/missing NimBLE files", result.stderr)

    def test_queued_host_timer_shutdown_and_restart(self):
        reset = function("src/nimble/nimble/host/src/ble_hs.c", "static void\nble_hs_timer_reset(uint32_t ticks)")
        mocks = r'''
#include <cassert>
#include <cstdint>
struct ble_npl_event { void (*fn)(ble_npl_event*); };
struct ble_npl_callout { ble_npl_event ev; bool active; } ble_hs_timer{};
bool enabled = true;
unsigned callbacks = 0, deinitializations = 0;
bool ble_hs_is_enabled() { return enabled; }
void ble_npl_callout_stop(ble_npl_callout* co) { co->active = false; }
void ble_npl_callout_deinit(ble_npl_callout* co) { assert(!co->active); co->ev.fn = nullptr; ++deinitializations; }
int ble_npl_callout_reset(ble_npl_callout* co, uint32_t) { co->active = true; return 0; }
#define BLE_HS_DBG_ASSERT_EVAL(value) assert(value)
void timerCallback(ble_npl_event*) { ++callbacks; }
'''
        driver = r'''
int main() {
  for (unsigned cycle = 0; cycle < 10000; ++cycle) {
    ble_hs_timer.ev.fn = timerCallback;
    enabled = true;
    ble_hs_timer_reset(1);
    // Expiry has already queued this event before STOPPING is published.
    auto* queued = &ble_hs_timer.ev;
    enabled = false;
    ble_hs_timer_reset(0);
    if (queued->fn == nullptr) return 11;
    queued->fn(queued);
    // Final SDK teardown happens after queue dispatch and host stop.
    ble_npl_callout_deinit(&ble_hs_timer);
  }
  assert(callbacks == 10000 && deinitializations == 10000);
}
'''
        self.cpp(mocks + reset + driver, sanitized=True)
        # The pre-fix line deterministically invalidates an already queued event.
        before = reset.replace("ble_npl_callout_stop(&ble_hs_timer);",
                               "ble_npl_callout_stop(&ble_hs_timer);\nble_npl_callout_deinit(&ble_hs_timer);")
        self.cpp(mocks + before + driver, expected=11)

    def test_checked_timer_allocation_contract(self):
        checked = function("src/nimble/porting/npl/freertos/src/npl_os_freertos.c",
                           "int\nnpl_freertos_callout_init_checked(")
        mocks = r'''
#include <cassert>
#include <cstring>
#define CONFIG_BT_NIMBLE_USE_ESP_TIMER 1
#define ESP_OK 0
struct ble_npl_event;
using ble_npl_event_fn = void(ble_npl_event*);
struct ble_npl_event { ble_npl_event_fn* fn; void* arg; };
struct ble_npl_eventq {};
struct ble_npl_callout { ble_npl_event ev; ble_npl_eventq* evq; void* handle; };
struct esp_timer_create_args_t { void(*callback)(void*); void* arg; const char* name; };
void ble_npl_event_fn_wrapper(void*) {}
bool allocationFails = false;
int esp_timer_create(const esp_timer_create_args_t* args, void** handle) {
  assert(args->callback && args->arg && args->name);
  *handle = reinterpret_cast<void*>(0x1234);
  return allocationFails ? -1 : ESP_OK;
}
void callback(ble_npl_event*) {}
'''
        driver = r'''
int main() {
  ble_npl_callout timer{};
  ble_npl_eventq queue;
  allocationFails = true;
  assert(npl_freertos_callout_init_checked(&timer,&queue,callback,&queue) != 0);
  assert(!timer.handle && !timer.evq && !timer.ev.fn && !timer.ev.arg);
  allocationFails = false;
  assert(npl_freertos_callout_init_checked(&timer,&queue,callback,&queue) == 0);
  assert(timer.handle && timer.evq == &queue && timer.ev.fn == callback && timer.ev.arg == &queue);
}
'''
        self.cpp(mocks + checked + driver, sanitized=True)
        generic = function("src/nimble/porting/npl/freertos/src/npl_os_freertos.c",
                           "int\nnpl_freertos_callout_init(")
        self.assertIn("ESP_ERROR_CHECK(esp_timer_create", generic,
                      "Unchecked host callers must retain the upstream failure contract")

    def test_real_singleton_factory_and_client_timer_failures(self):
        device = (LIB / "src/NimBLEDevice.cpp").read_text()
        factory = balanced_block(device, device.index("NimBLEClient* NimBLEDevice::createClient(const"))
        scanner = balanced_block(device, device.index("NimBLEScan* NimBLEDevice::getScan()"))
        source = (LIB / "src/NimBLEClient.cpp").read_text()
        start = source.index("NimBLEClient::NimBLEClient(")
        constructor = source[start:source.index("} // NimBLEClient", start) + 1]
        destructor = balanced_block(source, source.index("NimBLEClient::~NimBLEClient()"))
        mocks = r'''
#include <array>
#include <cassert>
#include <cstdlib>
#include <new>
#include <vector>
#define CONFIG_BT_NIMBLE_EXT_ADV 0
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_GAP_INITIAL_CONN_ITVL_MIN 24
#define BLE_GAP_INITIAL_CONN_ITVL_MAX 40
#define BLE_GAP_INITIAL_CONN_LATENCY 0
#define BLE_GAP_INITIAL_SUPERVISION_TIMEOUT 400
#define BLE_GAP_INITIAL_CONN_MIN_CE_LEN 0
#define BLE_GAP_INITIAL_CONN_MAX_CE_LEN 0
#define NIMBLE_MAX_CONNECTIONS 1
#define NIMBLE_LOGE(...) ((void)0)
struct NimBLEAddress {};
struct NimBLEClientCallbacks {};
struct ble_npl_event {};
struct ble_npl_callout { bool initialized = false; };
bool objectAllocationFails = false, timerAllocationFails = false;
unsigned timerStops = 0, timerDeinits = 0, destructions = 0;
void* nimble_port_get_dflt_eventq() { return nullptr; }
int checkedTimer(ble_npl_callout* co, void*, void(*)(ble_npl_event*), void*) {
  co->initialized = !timerAllocationFails; return timerAllocationFails ? -1 : 0;
}
#define NIMBLE_RADIOCLOCK_CALLOUT_INIT checkedTimer
void ble_npl_callout_stop(ble_npl_callout* co) { assert(co->initialized); ++timerStops; }
void ble_npl_callout_deinit(ble_npl_callout* co) { assert(co->initialized); co->initialized=false; ++timerDeinits; }
struct Params { template<class... T> Params(T...) {} };
class NimBLEClient {
public:
  static NimBLEClientCallbacks defaultCallbacks;
  explicit NimBLEClient(const NimBLEAddress&);
  ~NimBLEClient();
  static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    return objectAllocationFails ? nullptr : std::malloc(bytes);
  }
  static void operator delete(void* ptr) noexcept { ++destructions; std::free(ptr); }
  static void connectEstablishedTimerCb(ble_npl_event*) {}
  void deleteServices() { assert(m_svcVec.empty()); }
  NimBLEAddress m_peerAddress;
  int m_lastErr, m_connectTimeout;
  void* m_pTaskData;
  std::vector<int*> m_svcVec;
  NimBLEClientCallbacks* m_pClientCallbacks;
  int m_connHandle, m_terminateFailCount, m_asyncSecureAttempt;
  struct Config { bool deleteCallbacks = false; } m_config;
  enum State { DISCONNECTED } m_connStatus;
  ble_npl_callout m_connectEstablishedTimer;
  bool m_connectTimerInitialized = false, m_connectCallbackPending;
  int m_connectFailRetryCount;
  Params m_connParams;
};
NimBLEClientCallbacks NimBLEClient::defaultCallbacks;
class NimBLEScan {
public:
  static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    return objectAllocationFails ? nullptr : std::malloc(bytes);
  }
  static void operator delete(void* ptr) noexcept { std::free(ptr); }
};
class NimBLEDevice {
public:
  static std::array<NimBLEClient*,1> m_pClients;
  static NimBLEScan* m_pScan;
  static NimBLEClient* createClient(const NimBLEAddress&);
  static NimBLEScan* getScan();
};
std::array<NimBLEClient*,1> NimBLEDevice::m_pClients{};
NimBLEScan* NimBLEDevice::m_pScan = nullptr;
'''
        driver = r'''
int main() {
  objectAllocationFails = true;
  assert(!NimBLEDevice::getScan() && !NimBLEDevice::createClient(NimBLEAddress{}));
  assert(!NimBLEDevice::m_pClients[0] && destructions == 0);
  objectAllocationFails = false;
  timerAllocationFails = true;
  assert(!NimBLEDevice::createClient(NimBLEAddress{}));
  assert(!NimBLEDevice::m_pClients[0] && destructions == 1 && timerStops == 0 && timerDeinits == 0);
  timerAllocationFails = false;
  auto* client = NimBLEDevice::createClient(NimBLEAddress{});
  assert(client && client->m_connectTimerInitialized);
  assert(!NimBLEDevice::createClient(NimBLEAddress{}));
  auto* scan = NimBLEDevice::getScan();
  assert(scan && NimBLEDevice::getScan() == scan);
  delete scan;
  delete client;
  assert(timerStops == 1 && timerDeinits == 1 && destructions == 2);
}
'''
        self.cpp(mocks + constructor + destructor + scanner + factory + driver, sanitized=True)


if __name__ == "__main__":
    unittest.main()
