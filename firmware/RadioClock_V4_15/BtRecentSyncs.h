#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Loop-owned, RAM-only recent attempts. This is separate from the optional
// once-per-day durable snapshot and must never be serialized to storage.
namespace BtRecentSyncs {
constexpr size_t ProfileCount = 4;
constexpr size_t Capacity = 4;
constexpr uint8_t ProtocolCount = 3;
constexpr uint32_t EarliestClockUtc = 1577836800U;

struct Entry {
  uint32_t utcEpoch = 0; // Zero means the clock was untrusted at failure time.
  uint8_t protocol = 0;
  bool successful = false;
};

class Store {
public:
  // Reconcile with the currently bound identity before rendering/recording.
  // A failed replacement pairing must not replace this binding or its history.
  bool bind(size_t profile, const char *address, uint8_t protocol) {
    if (profile >= ProfileCount) return false;
    Profile &state = profiles_[profile];
    if (!validAddress(address) || protocol >= ProtocolCount) {
      state = Profile{};
      return false;
    }
    if (!sameAddress(state.address, address) || state.protocol != protocol) {
      state = Profile{};
      memcpy(state.address, address, sizeof(state.address));
      state.protocol = protocol;
    }
    return true;
  }

  bool record(size_t profile, const char *attemptAddress, uint8_t protocol,
              uint32_t utcEpoch, bool successful, bool successesOnly = false) {
    if (profile >= ProfileCount || (successesOnly && !successful)) return false;
    Profile &state = profiles_[profile];
    if (!validAddress(attemptAddress) || protocol != state.protocol ||
        !sameAddress(state.address, attemptAddress)) return false;
    Entry &entry = state.entries[state.next];
    entry.utcEpoch = utcEpoch >= EarliestClockUtc ? utcEpoch : 0;
    entry.protocol = protocol;
    entry.successful = successful;
    state.next = static_cast<uint8_t>((state.next + 1) % Capacity);
    if (state.count < Capacity) ++state.count;
    return true;
  }

  size_t count(size_t profile) const {
    return profile < ProfileCount ? profiles_[profile].count : 0;
  }

  // Newest first. Pointer remains valid until the next loop-owned record/bind.
  const Entry *latest(size_t profile, size_t index) const {
    if (profile >= ProfileCount || index >= profiles_[profile].count) return nullptr;
    const Profile &state = profiles_[profile];
    return &state.entries[(state.next + Capacity - 1 - index) % Capacity];
  }

private:
  struct Profile {
    Entry entries[Capacity]{};
    char address[18]{};
    uint8_t protocol = 0;
    uint8_t next = 0;
    uint8_t count = 0;
  } profiles_[ProfileCount]{};

  static uint8_t folded(char c) {
    return static_cast<uint8_t>(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
  }

  static bool validAddress(const char *address) {
    if (!address) return false;
    for (size_t i = 0; i < 17; ++i) {
      const uint8_t c = folded(address[i]);
      if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return false;
    }
    return address[17] == '\0';
  }

  static bool sameAddress(const char *a, const char *b) {
    if (!validAddress(a) || !validAddress(b)) return false;
    for (size_t i = 0; i < 17; ++i) if (folded(a[i]) != folded(b[i])) return false;
    return true;
  }
};
} // namespace BtRecentSyncs
