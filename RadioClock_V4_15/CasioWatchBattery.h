#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Independently implemented from GW-BX5600 vendor 0x28 watch-condition
// captures and the model calibration in izivkov/gshock_api and its web app.
// This is an estimate in 10% steps, not a direct battery state-of-charge value.
namespace CasioWatchBattery {
constexpr uint8_t kHeader = 0x28;
constexpr size_t kBxResponseSize = 9;
constexpr uint32_t kReplyTimeoutMs = 1500;

inline bool decodeBx(const uint8_t *packet, size_t size, uint8_t &percent) {
  if (!packet || size != kBxResponseSize || packet[0] != kHeader) return false;
  const int estimate = (static_cast<int>(packet[1]) - 14) * 10;
  percent = static_cast<uint8_t>(estimate < 0 ? 0 : estimate > 100 ? 100 : estimate);
  return true;
}

struct Reading {
  bool available = false;
  uint8_t percent = 0;
  time_t sampledUtc = 0;
  char address[18] = {};

  bool matches(const char *boundAddress) const {
    return available && boundAddress && strlen(boundAddress) == 17 &&
           strcasecmp(address, boundAddress) == 0;
  }
};
}  // namespace CasioWatchBattery
