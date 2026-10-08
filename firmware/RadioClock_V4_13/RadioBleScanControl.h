#pragma once
#include <atomic>
#include <stdint.h>

// Include after Arduino, NimBLEScan and the NimBLE NPL/port declarations.
// GAP marks a scan inactive before its completion callback finishes. Running
// start()/stop() on loopTask can therefore destroy a device still used by the
// host. This controller runs BOTH operations on the host's event queue.
// The caller is loopTask; initialize/release must not race another caller.
class RadioBleScanControl {
public:
  bool initialize(NimBLEScan *scan, uint32_t durationMs, bool (*canStart)()) {
    if (initialized_) return scan_ == scan && quiescent();
    if (!scan || !canStart) return false;
    scan_ = scan;
    durationMs_ = durationMs;
    canStart_ = canStart;
    ble_npl_event_init(&event_, dispatch, this);
    stopped_.store(true, std::memory_order_release);
    initialized_ = true;
    return true;
  }

  bool start() { return execute(Command::Start); }
  bool stop() { return !initialized_ || execute(Command::Stop); }

  bool quiescent() const {
    return !initialized_ ||
      (!pending_.load(std::memory_order_acquire) && stopped_.load(std::memory_order_acquire));
  }

  // Call only AFTER NimBLEDevice::deinit() has stopped the host. A timeout
  // leaves this static event and its command alive until host acknowledgement.
  // Never reset/free an event while its callback or queue can still use it.
  bool releaseAfterHostStop() {
    if (!quiescent()) return false;
    if (initialized_) ble_npl_event_deinit(&event_);
    event_ = {};
    initialized_ = false;
    scan_ = nullptr;
    canStart_ = nullptr;
    return true;
  }

private:
  enum class Command : uint8_t { Start, Stop };
  static constexpr uint32_t kWaitMs = 2000;
  ble_npl_event event_{};
  NimBLEScan *scan_ = nullptr;
  bool (*canStart_)() = nullptr;
  uint32_t durationMs_ = 0;
  bool initialized_ = false;
  std::atomic<Command> command_{Command::Stop};
  std::atomic<bool> pending_{false};
  std::atomic<bool> stopped_{true};
  std::atomic<bool> succeeded_{false};

  bool waitForAcknowledgement() const {
    const uint32_t started = millis();
    while (pending_.load(std::memory_order_acquire)) {
      if ((uint32_t)(millis() - started) >= kWaitMs) return false;
      delay(1);
    }
    return true;
  }

  bool execute(Command command) {
    if (!initialized_ || !scan_ || !waitForAcknowledgement()) return false;
    command_.store(command, std::memory_order_relaxed);
    succeeded_.store(false, std::memory_order_relaxed);
    if (command == Command::Start) stopped_.store(false, std::memory_order_relaxed);
    pending_.store(true, std::memory_order_release);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &event_);
    return waitForAcknowledgement() && succeeded_.load(std::memory_order_acquire);
  }

  static void dispatch(ble_npl_event *event) {
    auto *control = static_cast<RadioBleScanControl *>(ble_npl_event_get_arg(event));
    if (!control || !control->pending_.load(std::memory_order_acquire)) return;
    bool succeeded = false;
    const Command command = control->command_.load(std::memory_order_relaxed);
    if (command == Command::Stop) {
      // Unconditional stop also supplies a queue barrier when GAP already
      // reports inactive but the previous DISC_COMPLETE callback was running.
      succeeded = control->scan_->stop();
    } else if (control->canStart_()) {
      // RF demand can arrive after loopTask queues this request. Recheck here,
      // on the host, immediately before starting the controller operation.
      succeeded = control->scan_->isScanning() ||
        control->scan_->start(control->durationMs_, false, true);
    }
    const bool inactive = !control->scan_->isScanning();
    control->stopped_.store(inactive && (command != Command::Stop || succeeded),
                            std::memory_order_relaxed);
    control->succeeded_.store(succeeded, std::memory_order_relaxed);
    control->pending_.store(false, std::memory_order_release);
  }
};
