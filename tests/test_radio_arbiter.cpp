#include "../firmware/RadioClock_V4_15/RadioBleArbiter.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

// Run with:
// g++ -std=c++17 -O2 -pthread tests/test_radio_arbiter.cpp -o /tmp/radio-arbiter-test
// /tmp/radio-arbiter-test
// These exercise the firmware's real ownership gate. Physical controller and
// carrier flags are models; RF waveforms and NimBLE teardown require hardware.
static void require(bool value, const char *expression, int line) {
  if (!value) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    std::abort();
  }
}
#define CHECK(expression) require((expression), #expression, __LINE__)

using Owner = RadioBleArbiter::Owner;

class Gate {
public:
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    condition_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return open_; });
  }
private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool open_ = false;
};

static void failedShutdownKeepsRfOff() {
  RadioBleArbiter arbiter;
  CHECK(arbiter.owner() == Owner::Idle);
  CHECK(arbiter.acquireBle());
  CHECK(!arbiter.requestRf());
  CHECK(arbiter.rfRequested());
  CHECK(!arbiter.finishBleShutdown(false));
  for (int attempt = 0; attempt < 20; ++attempt) {
    CHECK(!arbiter.requestRf());
    CHECK(!arbiter.acquireBle());
    CHECK(arbiter.bleOwned());
  }
  CHECK(arbiter.finishBleShutdown(true));
  // RF demand survives successful shutdown: another BLE init cannot race ahead.
  CHECK(!arbiter.acquireBle());
  CHECK(arbiter.requestRf());
  CHECK(arbiter.rfOwned());
  CHECK(!arbiter.acquireBle());
  arbiter.releaseRf();
  CHECK(!arbiter.rfRequested());
  CHECK(arbiter.acquireBle());
  CHECK(arbiter.finishBleShutdown(true));
}

static void rfOwnershipSpansZeroCarrierSlots() {
  RadioBleArbiter arbiter;
  CHECK(arbiter.requestRf());
  // The MSF zero-carrier slots and JJY/WWVB/DCF reduced-carrier slots belong to
  // the ongoing transmission, so neither means Bluetooth may initialize.
  const unsigned envelope[] = {1, 0, 0, 1, 2, 2, 1, 0, 1, 1};
  for (int second = 0; second < 60; ++second) {
    for (unsigned carrierState : envelope) {
      (void)carrierState;
      CHECK(arbiter.rfOwned());
      CHECK(arbiter.rfRequested());
      CHECK(!arbiter.acquireBle());
      CHECK(!arbiter.finishBleShutdown(true));
      CHECK(arbiter.requestRf()); // repeated scheduler requests are idempotent
    }
  }
  // Model the carrier being silenced, then give up the complete RF session.
  arbiter.releaseRf();
  CHECK(arbiter.owner() == Owner::Idle);
  CHECK(arbiter.acquireBle());
  CHECK(arbiter.finishBleShutdown(true));
}

static void rfDemandDuringInitializationOrCancellation() {
  RadioBleArbiter arbiter;
  Gate initializationStarted, demandRaised, initializationCancelled;
  std::thread ble([&] {
    CHECK(arbiter.acquireBle());
    initializationStarted.open();
    demandRaised.wait();
    // Failed/cancelled init is still responsible for a partially enabled
    // controller. Until physical OFF is confirmed, RF stays blocked.
    CHECK(!arbiter.finishBleShutdown(false));
    CHECK(arbiter.bleOwned());
    initializationCancelled.open();
  });
  initializationStarted.wait();
  CHECK(!arbiter.requestRf());
  demandRaised.open();
  initializationCancelled.wait();
  CHECK(!arbiter.requestRf());
  ble.join();
  CHECK(arbiter.finishBleShutdown(true));
  CHECK(!arbiter.acquireBle());
  CHECK(arbiter.requestRf());
  arbiter.releaseRf();
}

static void cancelledDemandReopensBle() {
  RadioBleArbiter arbiter;
  CHECK(arbiter.acquireBle());
  CHECK(!arbiter.requestRf());
  // Clock confidence/schedule can disappear while shutdown is outstanding.
  // The radio task cancels demand but cannot steal BLE ownership.
  arbiter.releaseRf();
  CHECK(!arbiter.rfRequested());
  CHECK(arbiter.bleOwned());
  CHECK(!arbiter.requestRf());
  CHECK(arbiter.finishBleShutdown(true));
  CHECK(arbiter.requestRf());
  arbiter.releaseRf();
  CHECK(arbiter.acquireBle());
  CHECK(arbiter.finishBleShutdown(true));
}

static void simultaneousAcquisitionsNeverBothSucceed() {
  for (int attempt = 0; attempt < 512; ++attempt) {
    RadioBleArbiter arbiter;
    Gate start;
    bool bleAcquired = false, rfAcquired = false;
    std::thread ble([&] { start.wait(); bleAcquired = arbiter.acquireBle(); });
    std::thread radio([&] { start.wait(); rfAcquired = arbiter.requestRf(); });
    start.open();
    ble.join();
    radio.join();
    CHECK(!(bleAcquired && rfAcquired));
    CHECK(arbiter.rfRequested());
    if (bleAcquired) {
      CHECK(arbiter.bleOwned());
      CHECK(arbiter.finishBleShutdown(true));
    }
    // If BLE won CAS but observed newly raised demand before init, both calls
    // may report false. The next radio attempt must still make progress.
    CHECK(arbiter.requestRf());
    CHECK(arbiter.rfOwned());
    CHECK(!arbiter.acquireBle());
    arbiter.releaseRf();
  }
}

static void concurrentPhysicalLifecycleStress() {
  constexpr unsigned BLE_ON = 1, RF_ON = 2;
  RadioBleArbiter arbiter;
  std::atomic<unsigned> physicalState{0};
  std::atomic<bool> radioDone{false};
  std::atomic<unsigned> bleSessions{0};
  Gate firstBleInitialized, startRadio;

  auto runBleSession = [&] {
    // Acquisition must precede even the start of controller initialization.
    CHECK((physicalState.fetch_or(BLE_ON) & RF_ON) == 0);
    ++bleSessions;
    std::this_thread::yield();
    // Model an unsuccessful shutdown pass without relinquishing ownership.
    CHECK(!arbiter.finishBleShutdown(false));
    std::this_thread::yield();
    CHECK((physicalState.fetch_and(~BLE_ON) & RF_ON) == 0);
    CHECK(arbiter.finishBleShutdown(true));
  };

  std::thread ble([&] {
    CHECK(arbiter.acquireBle());
    firstBleInitialized.open();
    startRadio.wait();
    runBleSession();
    while (!radioDone.load()) {
      if (arbiter.acquireBle()) runBleSession();
      else std::this_thread::yield();
    }
  });
  std::thread radio([&] {
    firstBleInitialized.wait();
    CHECK(!arbiter.requestRf());
    startRadio.open();
    for (int session = 0; session < 10000; ++session) {
      while (!arbiter.requestRf()) std::this_thread::yield();
      CHECK((physicalState.fetch_or(RF_ON) & BLE_ON) == 0);
      // The carrier can be silent within a transmission; ownership remains.
      CHECK((physicalState.fetch_and(~RF_ON) & BLE_ON) == 0);
      std::this_thread::yield();
      CHECK(!arbiter.acquireBle());
      CHECK((physicalState.fetch_or(RF_ON) & BLE_ON) == 0);
      CHECK((physicalState.fetch_and(~RF_ON) & BLE_ON) == 0);
      arbiter.releaseRf();
      std::this_thread::yield();
    }
    radioDone = true;
  });
  ble.join();
  radio.join();
  CHECK(physicalState.load() == 0);
  CHECK(arbiter.owner() == Owner::Idle);
  CHECK(!arbiter.rfRequested());
  CHECK(bleSessions.load() > 0);
  std::cout << "Concurrent stress: 10000 RF sessions, " << bleSessions.load()
            << " BLE sessions; no modeled overlap\n";
}

int main() {
  failedShutdownKeepsRfOff();
  rfOwnershipSpansZeroCarrierSlots();
  rfDemandDuringInitializationOrCancellation();
  cancelledDemandReopensBle();
  simultaneousAcquisitionsNeverBothSucceed();
  concurrentPhysicalLifecycleStress();
  std::cout << "RadioBleArbiter tests passed\n";
}
