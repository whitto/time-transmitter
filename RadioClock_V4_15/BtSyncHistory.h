#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// One bounded status snapshot, separate from saved device configuration.
// This contains no log stream and uses no heap or Arduino-specific types.
namespace BtSyncHistory {
constexpr uint32_t Magic = 0x42544831U;  // BTH1
constexpr uint32_t Schema = 1U;
constexpr int64_t SecondsPerDay = 86400;
constexpr int64_t BrisbaneOffsetSeconds = 10 * 60 * 60;
constexpr int64_t EarliestClockUtc = 1577836800;  // 2020-01-01 UTC
constexpr int64_t LatestClockUtc = UINT32_MAX;
constexpr size_t ProtocolCount = 3;
constexpr size_t ProfileCount = 4;

// Initialize with Snapshot{} before filling fields. The version and exact size
// reject incompatible layouts; the CRC protects all bytes, including padding.
struct Snapshot {
  uint32_t magic;
  uint32_t schema;
  uint32_t size;
  uint32_t checksum;
  uint32_t generation;
  int64_t savedDay;
  int64_t successEpoch;
  uint8_t lastProfile;
  uint8_t lastProtocol;
  uint32_t connections;
  uint32_t ackedWrites;
  uint32_t notifications;
  uint32_t responseErrors;
  uint32_t protocolEpoch[ProtocolCount];
  uint32_t profileEpoch[ProfileCount];
  char profileAddress[ProfileCount][18];
  uint8_t profileProtocol[ProfileCount];
};

inline bool validEpoch(int64_t epoch) {
  return epoch >= EarliestClockUtc && epoch <= LatestClockUtc;
}

inline int64_t brisbaneDay(int64_t epoch) {
  return validEpoch(epoch) ? (epoch + BrisbaneOffsetSeconds) / SecondsPerDay : -1;
}

inline bool saveDue(bool enabled, int64_t now, int64_t lastSavedDay,
                    int64_t lastAttemptDay) {
  const int64_t day = brisbaneDay(now);
  // A backward clock correction cannot open a day already saved/attempted.
  return enabled && day >= 0 && day > lastSavedDay && day > lastAttemptDay;
}

inline uint32_t crc32(const Snapshot &snapshot) {
  const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&snapshot);
  uint32_t crc = UINT32_MAX;
  constexpr size_t checksumOffset = offsetof(Snapshot, checksum);
  for (size_t i = 0; i < sizeof(Snapshot); ++i) {
    const uint8_t byte = i >= checksumOffset && i < checksumOffset + sizeof(snapshot.checksum)
                          ? 0 : bytes[i];
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

inline void seal(Snapshot &snapshot) {
  snapshot.magic = Magic;
  snapshot.schema = Schema;
  snapshot.size = sizeof(Snapshot);
  snapshot.checksum = 0;
  snapshot.checksum = crc32(snapshot);
}

template<size_t N>
inline bool terminated(const char (&text)[N]) {
  return memchr(text, '\0', N) != nullptr;
}

inline bool hexDigit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
         (value >= 'a' && value <= 'f');
}

inline bool addressValid(const char (&address)[18], bool emptyAllowed = false) {
  if (!terminated(address)) return false;
  if (address[0] == '\0') return emptyAllowed;
  if (address[17] != '\0') return false;
  for (size_t i = 0; i < 17; ++i) {
    if (i % 3 == 2 ? address[i] != ':' : !hexDigit(address[i])) return false;
  }
  return true;
}

inline bool valid(const Snapshot &snapshot) {
  if (snapshot.magic != Magic || snapshot.schema != Schema ||
      snapshot.size != sizeof(Snapshot) || snapshot.checksum != crc32(snapshot) ||
      !validEpoch(snapshot.successEpoch) ||
      snapshot.savedDay != brisbaneDay(snapshot.successEpoch) ||
      snapshot.lastProfile >= ProfileCount || snapshot.lastProtocol >= ProtocolCount)
    return false;
  if (snapshot.profileEpoch[snapshot.lastProfile] != snapshot.successEpoch ||
      snapshot.protocolEpoch[snapshot.lastProtocol] != snapshot.successEpoch ||
      snapshot.profileProtocol[snapshot.lastProfile] != snapshot.lastProtocol) return false;
  for (size_t i = 0; i < ProtocolCount; ++i)
    if (snapshot.protocolEpoch[i] && !validEpoch(snapshot.protocolEpoch[i])) return false;
  for (size_t i = 0; i < ProfileCount; ++i) {
    if (snapshot.profileProtocol[i] >= ProtocolCount ||
        !addressValid(snapshot.profileAddress[i], !snapshot.profileEpoch[i]) ||
        (snapshot.profileEpoch[i] && !validEpoch(snapshot.profileEpoch[i]))) return false;
  }
  return true;
}
}  // namespace BtSyncHistory
