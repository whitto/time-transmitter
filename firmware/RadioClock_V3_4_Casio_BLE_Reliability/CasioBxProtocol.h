#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// GW-BX5600 SP_DATA framing, independently implemented from observed packets:
// https://github.com/izivkov/gshock_api/tree/ae817310dd9551d5e6e53ee61d89af4e8d954767/test_data
// A record starts with its little-endian payload length, then tag and slot.
// Only framing is interpreted; every other byte is retained from the watch.
namespace CasioBxProtocol {

static constexpr size_t kSettingsResponseSize = 101;
static constexpr size_t kSettingsWriteSize = 35;
static constexpr size_t kCitiesSize = 66;
static constexpr size_t kDstResponseSize = 28;
static constexpr size_t kDstWriteSize = 94;
static constexpr size_t kAlarmResponseSize = 133;
static constexpr uint16_t kMinimumMtu = kAlarmResponseSize + 3;

namespace detail {

inline bool recordMatches(const uint8_t* record, uint8_t payloadLength,
                          uint8_t tag, uint8_t slot) {
  return record[0] == payloadLength && record[1] == 0 &&
         record[2] == tag && record[3] == slot;
}

inline bool validCities(const uint8_t* cities, size_t size) {
  if (!cities || size != kCitiesSize) return false;
  for (uint8_t slot = 0; slot < 3; ++slot) {
    if (!recordMatches(cities + slot * 22, 20, 0x24, slot)) return false;
  }
  return true;
}

inline bool rangesOverlap(const uint8_t* first, size_t firstSize,
                          const uint8_t* second, size_t secondSize) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(first);
  const uintptr_t b = reinterpret_cast<uintptr_t>(second);
  return a <= b ? b - a < firstSize : a - b < secondSize;
}

}  // namespace detail

// Response 05 contains two 1D settings records followed by three 24 cities.
// The first write is 02 + the settings records only (35 bytes). Keep the 66
// city bytes for the subsequent DST write. The observed 1D slots are 0 and 2.
// The two output regions must be disjoint; either may alias the input.
// Failure leaves both outputs untouched.
inline bool splitSettings(const uint8_t* response, size_t size,
                          uint8_t* settingsOut, size_t settingsCapacity,
                          uint8_t* citiesOut, size_t citiesCapacity) {
  if (!response || !settingsOut || !citiesOut ||
      size != kSettingsResponseSize || settingsCapacity < kSettingsWriteSize ||
      citiesCapacity < kCitiesSize || response[0] != 0x05 ||
      !detail::recordMatches(response + 1, 15, 0x1d, 0) ||
      !detail::recordMatches(response + 18, 15, 0x1d, 2) ||
      !detail::validCities(response + kSettingsWriteSize, kCitiesSize) ||
      detail::rangesOverlap(settingsOut, kSettingsWriteSize, citiesOut, kCitiesSize)) {
    return false;
  }

  // Stage the entire response before changing either output, including when
  // a caller reuses its response buffer for one of the writes.
  uint8_t staged[kSettingsResponseSize];
  memcpy(staged, response, sizeof(staged));
  staged[0] = 0x02;
  memcpy(settingsOut, staged, kSettingsWriteSize);
  memcpy(citiesOut, staged + kSettingsWriteSize, kCitiesSize);
  return true;
}

// Response 03 supplies three 1E records. Write 06 + these records + the saved
// 24 city records (94 bytes), retaining the watch's city and DST configuration.
// Inputs may alias the output. Failure leaves the output untouched.
inline bool buildDst(const uint8_t* response, size_t size,
                     const uint8_t* cities, size_t citiesSize,
                     uint8_t* out, size_t capacity) {
  if (!response || !out || size != kDstResponseSize ||
      capacity < kDstWriteSize || response[0] != 0x03 ||
      !detail::validCities(cities, citiesSize)) {
    return false;
  }
  for (uint8_t slot = 0; slot < 3; ++slot) {
    if (!detail::recordMatches(response + 1 + slot * 9, 7, 0x1e, slot)) return false;
  }

  uint8_t staged[kDstWriteSize];
  staged[0] = 0x06;
  memcpy(staged + 1, response + 1, kDstResponseSize - 1);
  memcpy(staged + kDstResponseSize, cities, kCitiesSize);
  memcpy(out, staged, sizeof(staged));
  return true;
}

// The legacy "alarm" stage is actually six 1F city-name records. Its 133-byte
// response is written unchanged. Check all observed slots to reject duplicate
// or unrelated records instead of accepting any reply with header 06.
inline bool validAlarms(const uint8_t* response, size_t size) {
  if (!response || size != kAlarmResponseSize || response[0] != 0x06) return false;
  static const uint8_t slots[] = {0, 6, 1, 7, 2, 8};
  for (size_t index = 0; index < sizeof(slots); ++index) {
    if (!detail::recordMatches(response + 1 + index * 22, 20, 0x1f, slots[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace CasioBxProtocol
