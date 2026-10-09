#pragma once

#include <stddef.h>
#include <stdint.h>

// Project APIs use small URL-encoded forms or JSON; uploads are unsupported.
// These are fixed limits, not build flags that another library can disable.
#define RADIO_HTTP_BOUNDS_VERSION 1
static constexpr size_t RADIO_HTTP_MAX_BODY_BYTES = 4096;
static constexpr size_t RADIO_HTTP_MAX_HEADER_BYTES = 4096;
static constexpr uint32_t RADIO_HTTP_REQUEST_BUDGET_MS = 10000;
static constexpr uint32_t RADIO_HTTP_HEADER_BUDGET_MS = 5000;

enum class RadioHttpLengthResult { Ok, Invalid, TooLarge };

inline RadioHttpLengthResult radioHttpContentLength(const char *text, size_t length, size_t &result) {
  if (!text || !length) return RadioHttpLengthResult::Invalid;
  size_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char byte = static_cast<unsigned char>(text[i]);
    if (byte < '0' || byte > '9') return RadioHttpLengthResult::Invalid;
    const size_t digit = byte - '0';
    // Compare before arithmetic: no integer overflow or allocation on huge input.
    if (value > (RADIO_HTTP_MAX_BODY_BYTES - digit) / 10)
      return RadioHttpLengthResult::TooLarge;
    value = value * 10 + digit;
  }
  result = value;
  return RadioHttpLengthResult::Ok;
}

inline bool radioHttpBudgetExpired(uint32_t now, uint32_t start, uint32_t budget) {
  return static_cast<uint32_t>(now - start) >= budget;
}
