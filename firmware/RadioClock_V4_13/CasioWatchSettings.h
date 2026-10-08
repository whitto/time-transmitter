#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Independently implemented from the public web app's basic-settings mapping:
// izivkov/gshock-smart-sync-webapp, commit 5b3ff83dbae43a7a526232e5f35de98425ca7ac8
// src/api/io/SettingsIO.ts: header 0x13; byte 8 bit 0x20 selects Classic.
// GW-BX5600 captures in izivkov/gshock_api use the shorter 12-byte layout.
// Preserve every other setting and reserved byte from the actual watch reply.
namespace CasioWatchSettings {
constexpr size_t kBasicSize = 17;
constexpr size_t kBxBasicSize = 12;
constexpr uint8_t kBasicHeader = 0x13;
constexpr uint8_t kClassicMask = 0x20;
inline bool validBasicSize(size_t size) {
  return size == kBxBasicSize || size == kBasicSize;
}
inline bool buildFont(const uint8_t *reply, size_t size, uint8_t mode,
                      uint8_t *out, size_t capacity) {
  if (!reply || !out || !validBasicSize(size) || capacity < size ||
      reply[0] != kBasicHeader || (mode != 1 && mode != 2)) return false;
  uint8_t staged[kBasicSize];
  memcpy(staged, reply, size);
  staged[8] = mode == 2 ? staged[8] | kClassicMask : staged[8] & ~kClassicMask;
  memcpy(out, staged, size);
  return true;
}
} // namespace CasioWatchSettings
