#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace RadioConfigFormat {
// Longer than the station SDK's 32-byte SSID limit, so no valid legacy SSID
// can be mistaken for a version marker (including RADIOCLOCK_CONFIG_V1).
constexpr char MAGIC[] = "RADIOCLOCK_CONFIG_V1_CHECKSUMMED_SETTINGS";
static_assert(sizeof(MAGIC)-1 > 32 && sizeof(MAGIC)-1 <= 64,
              "A config version marker must not collide with a valid SSID");
constexpr size_t FIELD_COUNT = 68;
inline uint32_t crc32(const char *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i=0;i<length;++i) {
    crc ^= static_cast<uint8_t>(bytes[i]);
    for (unsigned bit=0;bit<8;++bit)
      crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
  }
  return ~crc;
}
}

// A complete configuration record is built without heap allocation. Each
// append is checked before storage is touched; failed appends cannot shorten
// an otherwise apparently valid record. The caller owns the bounded buffer.
class RadioConfigWriter {
 public:
  RadioConfigWriter(char *buffer, size_t capacity)
      : buffer_(buffer), capacity_(capacity) {
    if (capacity_) buffer_[0] = '\0';
  }

  bool line(const char *value, bool removeLineBreaks = false) {
    if (!ok_ || !value) return fail();
    for (const char *p = value; *p; ++p) {
      if (*p == '\r' || *p == '\n') {
        if (removeLineBreaks) continue;
        return fail();
      }
      if (!append(*p)) return false;
    }
    if (!append('\n')) return false;
    ++lines_;
    return true;
  }

  bool number(int32_t value) {
    char digits[16];
    const int length = snprintf(digits, sizeof(digits), "%ld", (long)value);
    return length > 0 && (size_t)length < sizeof(digits) ? line(digits) : fail();
  }

  bool unsignedNumber(uint32_t value) {
    char digits[16];
    const int length = snprintf(digits, sizeof(digits), "%lu", (unsigned long)value);
    return length > 0 && (size_t)length < sizeof(digits) ? line(digits) : fail();
  }

  bool complete(size_t expectedLines) const {
    return ok_ && lines_ == expectedLines && length_ > 0 &&
           buffer_[length_ - 1] == '\n';
  }
  size_t size() const { return length_; }

  // The checksum covers the version marker and every payload byte. A missing
  // trailer is never treated as a legacy record once the marker is present.
  bool seal(size_t fields) {
    if (!complete(fields + 1)) return fail();
    char trailer[15];
    const int count = snprintf(trailer, sizeof(trailer), "CRC32:%08lX",
                              (unsigned long)RadioConfigFormat::crc32(buffer_, length_));
    return count == 14 && line(trailer) && complete(fields + 2);
  }

 private:
  bool fail() { ok_ = false; return false; }
  bool append(char value) {
    if (!ok_ || length_ + 1 >= capacity_) return fail();
    buffer_[length_++] = value;
    buffer_[length_] = '\0';
    return true;
  }
  char *buffer_;
  size_t capacity_, length_ = 0, lines_ = 0;
  bool ok_ = true;
};

// Bounded, allocation-free candidate. The caller's bytes are split only after
// the complete version/checksum/structure has been verified. Semantic checks
// precede application of any candidate field to live configuration.
// bytes[length] must be a writable terminator supplied by the bounded caller;
// this also permits legacy records without a final newline.
class RadioConfigRecord {
 public:
  bool parse(char *bytes, size_t length) {
    count_ = cursor_ = 0; versioned_ = false;
    if (!bytes || !length) return false;
    const size_t markerLength = strlen(RadioConfigFormat::MAGIC);
    size_t begin = 0, end = length;
    if (length > markerLength && !memcmp(bytes, RadioConfigFormat::MAGIC, markerLength) &&
        bytes[markerLength] == '\n') {
      versioned_ = true; begin = markerLength + 1;
      if (length < begin + 15 || memcmp(bytes + length - 15, "CRC32:", 6) ||
          bytes[length - 1] != '\n') return false;
      uint32_t stored = 0;
      for (size_t i=length-9;i<length-1;++i) {
        const char c = bytes[i];
        const int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
        if (digit < 0) return false;
        stored = (stored << 4) | static_cast<uint32_t>(digit);
      }
      end = length - 15;
      if (stored != RadioConfigFormat::crc32(bytes, end)) return false;
    }
    for (size_t offset=begin;offset<end;) {
      if (count_ == RadioConfigFormat::FIELD_COUNT) return false;
      const size_t start = offset;
      while (offset < end && bytes[offset] != '\n') {
        if (!bytes[offset] || (static_cast<uint8_t>(bytes[offset]) < 32 && bytes[offset] != '\r') ||
            static_cast<uint8_t>(bytes[offset]) == 127 || offset-start >= 64) return false;
        ++offset;
      }
      size_t lineEnd = offset;
      if (lineEnd > start && bytes[lineEnd-1] == '\r') --lineEnd;
      for (size_t i=start;i<lineEnd;++i) if (bytes[i] == '\r') return false;
      bytes[lineEnd] = '\0';
      fields_[count_++] = bytes + start;
      if (offset < end) ++offset;
    }
    return count_ >= 3 && (!versioned_ || count_ == RadioConfigFormat::FIELD_COUNT);
  }
  size_t count() const { return count_; }
  bool versioned() const { return versioned_; }
  void restoreVersionedNewlines() {
    // Used only for the writer's canonical LF record after semantic checking;
    // the complete CRC-checked payload can then be written without a copy.
    if (!versioned_) return;
    for (size_t i=0;i<count_;++i)
      const_cast<char *>(fields_[i])[strlen(fields_[i])] = '\n';
  }
  const char *text(size_t index) const { return index < count_ ? fields_[index] : ""; }
  const char *next() { return text(cursor_++); }
  bool present(size_t index) const { return index < count_; }
  static bool signedInteger(const char *text, int32_t minimum, int32_t maximum, int32_t &value) {
    if (!text || !*text) return false;
    bool negative = *text == '-';
    if (negative) ++text;
    if (!*text) return false;
    uint32_t magnitude = 0;
    const uint32_t limit = negative ? UINT32_C(2147483648) : UINT32_C(2147483647);
    for (;*text;++text) {
      if (*text < '0' || *text > '9') return false;
      const uint32_t digit = static_cast<uint32_t>(*text - '0');
      if (magnitude > (limit-digit)/10) return false;
      magnitude = magnitude*10 + digit;
    }
    const int64_t parsed = negative ? -static_cast<int64_t>(magnitude) : magnitude;
    if (parsed < minimum || parsed > maximum) return false;
    value = static_cast<int32_t>(parsed); return true;
  }
  static bool unsignedInteger(const char *text, uint32_t &value) {
    if (!text || !*text) return false;
    uint32_t parsed = 0;
    for (;*text;++text) {
      if (*text < '0' || *text > '9') return false;
      const uint32_t digit = static_cast<uint32_t>(*text-'0');
      if (parsed > (UINT32_MAX-digit)/10) return false;
      parsed = parsed*10 + digit;
    }
    value = parsed; return true;
  }
  int32_t number(size_t index, int32_t fallback) const {
    int32_t value;
    return signedInteger(text(index), INT32_MIN, INT32_MAX, value) ? value : fallback;
  }
  bool flag(size_t index, bool fallback) const {
    const char *s = text(index);
    if (!strcmp(s,"1") || equalsWord(s,"true")) return true;
    if (!strcmp(s,"0") || equalsWord(s,"false")) return false;
    return fallback;
  }
  static bool boolean(const char *text) {
    return !strcmp(text,"0") || !strcmp(text,"1") || equalsWord(text,"true") || equalsWord(text,"false");
  }
 private:
  static bool equalsWord(const char *a, const char *b) {
    for (;*a && *b;++a,++b) {
      const char c = *a >= 'A' && *a <= 'Z' ? *a + ('a'-'A') : *a;
      if (c != *b) return false;
    }
    return !*a && !*b;
  }
  const char *fields_[RadioConfigFormat::FIELD_COUNT]{};
  size_t count_ = 0, cursor_ = 0;
  bool versioned_ = false;
};
