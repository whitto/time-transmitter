#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
