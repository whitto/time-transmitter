#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

// A bounded response builder: failure is sticky, and no partial response is
// sent. The HTTP loop owns the caller's buffer, never a callback or radio task.
class RadioJsonWriter {
 public:
  RadioJsonWriter(char* buffer, size_t capacity): buffer_(buffer), capacity_(capacity) {
    if (!buffer_ || !capacity_) ok_ = false;
    else buffer_[0] = '\0';
  }
  RadioJsonWriter& append(const char* text) {
    if (!text) { ok_ = false; return *this; }
    return bytes(text, std::strlen(text));
  }
  RadioJsonWriter& bytes(const char* text, size_t count) {
    if (!ok_) return *this;
    if (!text || count >= capacity_ - used_) { ok_ = false; return *this; }
    std::memcpy(buffer_ + used_, text, count); used_ += count;
    buffer_[used_] = '\0'; return *this;
  }
  RadioJsonWriter& quoted(const char* value) {
    append("\"");
    if (!value) { ok_ = false; return *this; }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); ok_ && *p; ++p) {
      switch (*p) {
        case '"': append("\\\""); break;
        case '\\': append("\\\\"); break;
        case '\b': append("\\b"); break;
        case '\f': append("\\f"); break;
        case '\n': append("\\n"); break;
        case '\r': append("\\r"); break;
        case '\t': append("\\t"); break;
        default:
          if (*p < 0x20) {
            char escape[7]; std::snprintf(escape, sizeof(escape), "\\u%04x", *p); append(escape);
          } else bytes(reinterpret_cast<const char*>(p), 1);
      }
    }
    return append("\"");
  }
  RadioJsonWriter& number(int64_t value) {
    char text[24]; const int n = std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(text)) { ok_ = false; return *this; }
    return bytes(text, static_cast<size_t>(n));
  }
  RadioJsonWriter& real(double value) {
    if (!std::isfinite(value)) return append("null");
    char text[48]; const int n = std::snprintf(text, sizeof(text), "%.6f", value);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(text)) { ok_ = false; return *this; }
    return bytes(text, static_cast<size_t>(n));
  }
  RadioJsonWriter& boolean(bool value) { return append(value ? "true" : "false"); }
  RadioJsonWriter& key(const char* name) { append(","); quoted(name); return append(":"); }
  bool ok() const { return ok_; }
  const char* data() const { return buffer_; }
  size_t size() const { return used_; }
 private:
  char* buffer_; size_t capacity_; size_t used_ = 0; bool ok_ = true;
};
