#pragma once
#include <stdint.h>

struct RadioPauseLease {
  bool acquired = false;
  bool wasPaused = false;
};

// Every access is protected by the firmware's radioCommandMux. The RF task
// alone changes hardware. A timeout discards only its own pending generation;
// it can never leave an unacknowledged pause as the committed desired state.
class RadioPauseControl {
public:
  uint32_t submit(bool pause, uint64_t nowUs, uint64_t timeoutUs) {
    if (++generation_ == 0) ++generation_;
    pending_ = true;
    requested_ = pause;
    deadlineUs_ = nowUs + timeoutUs;
    acknowledged_ = 0;
    return generation_;
  }
  bool cancel(uint32_t generation) {
    if (!pending_ || generation != generation_) return false;
    pending_ = false;
    return true;
  }
  bool desired(uint64_t nowUs) {
    if (pending_ && nowUs >= deadlineUs_) pending_ = false;
    return pending_ ? requested_ : committed_;
  }
  uint32_t pendingGeneration(uint64_t nowUs) {
    (void)desired(nowUs);
    return pending_ ? generation_ : 0;
  }
  bool acknowledge(uint32_t generation, uint64_t nowUs) {
    if (!generation || !pending_ || generation != generation_ || nowUs >= deadlineUs_)
      return false;
    committed_ = requested_;
    pending_ = false;
    acknowledged_ = generation;
    return true;
  }
  bool acknowledged(uint32_t generation) const { return acknowledged_ == generation; }
  bool committed() const { return committed_; }
  void commitWithoutTask(bool pause) { committed_ = pause; pending_ = false; }
private:
  uint32_t generation_ = 0;
  uint32_t acknowledged_ = 0;
  uint64_t deadlineUs_ = 0;
  bool pending_ = false;
  bool requested_ = false;
  bool committed_ = false;
};

enum RadioReliabilityFault : uint8_t {
  RADIO_FAULT_NONE = 0,
  RADIO_FAULT_TASK_CREATION = 1,
  RADIO_FAULT_TIMER_ALLOCATION = 2,
  RADIO_FAULT_TIMER_STALLED = 3,
  RADIO_FAULT_TASK_STALLED = 4,
  RADIO_FAULT_MONITOR_CREATION = 5,
  RADIO_FAULT_LOOP_STALLED = 6,
  RADIO_FAULT_RESUME_TIMEOUT = 7
};

// Monotonic 64-bit deadlines; no millis rollover, wall-clock or TZ dependency.
struct RadioProgressPolicy {
  static constexpr uint64_t TimerStallUs = 250000;
  static constexpr uint64_t RadioStallUs = 500000;
  static constexpr uint64_t RadioRestartUs = 3000000;
  static constexpr uint64_t LoopRestartUs = 70000000;
  static constexpr uint64_t RetrySpacingUs = 5000000;
  static constexpr uint32_t MaximumTimerRetries = 3;
  static bool elapsed(uint64_t nowUs, uint64_t lastUs, uint64_t limitUs) {
    return nowUs >= lastUs && nowUs - lastUs > limitUs;
  }
};
