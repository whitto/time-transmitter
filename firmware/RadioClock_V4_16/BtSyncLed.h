#pragma once

#include <stdint.h>

// Blue GPIO2 policy only. RF timing and its envelope LED have separate owners.
namespace BtSyncLed {
constexpr int64_t kEarliestClockUtc = 1577836800;  // 2020-01-01 UTC
constexpr int64_t kSuccessHoldSeconds = 24 * 60 * 60;
constexpr uint32_t kBlinkPhaseMs = 250;

inline bool successEligible(int64_t nowUtc, int64_t successUtc,
                            bool lastOutcomeSuccess) {
  // A saved success cannot light the LED until the clock is plausible. Use
  // absolute UTC age so changing a display/schedule timezone cannot extend it.
  return lastOutcomeSuccess && nowUtc >= kEarliestClockUtc &&
         successUtc >= kEarliestClockUtc && nowUtc >= successUtc &&
         nowUtc - successUtc < kSuccessHoldSeconds;
}

inline bool output(uint32_t nowMs, bool enabled, bool syncing,
                   bool successEligible, uint32_t &phaseStartMs,
                   bool &level, bool &wasActive) {
  if (!enabled || !syncing) {
    phaseStartMs = nowMs;
    level = enabled && successEligible;
    wasActive = false;
    return level;
  }
  if (!wasActive) {
    wasActive = true;
    phaseStartMs = nowMs;
    level = true;  // Even a short connection starts with a visible pulse.
    return true;
  }
  // Unsigned elapsed arithmetic preserves cadence through millis() rollover.
  const uint32_t phases = static_cast<uint32_t>(nowMs - phaseStartMs) / kBlinkPhaseMs;
  if (phases) {
    phaseStartMs += phases * kBlinkPhaseMs;
    if (phases & 1U) level = !level;
  }
  return level;
}
}  // namespace BtSyncLed
