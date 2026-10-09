#!/usr/bin/env python3
"""Parse native bounded responses and check failure/stress behavior under ASan."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'firmware/RadioClock_V4_15'
CPP = r'''
#include "RadioJsonWriter.h"
#include <cassert>
#include <limits>
#include <cstdlib>
#include <new>
static bool denyAllocation = false;
void* operator new(std::size_t n) {
  if (denyAllocation) std::abort();
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
int main() {
  char buffer[8192];
  denyAllocation = true;
  for (int i = 0; i < 20000; ++i) {
    RadioJsonWriter json(buffer, sizeof(buffer));
    json.append("{\"watch\":").quoted("BNE \\\"\n\t\r\b\f\x01\x1f é 🕒");
    json.key("uptime").number(6307200000LL);
    json.key("minimum").number(std::numeric_limits<int64_t>::min());
    json.key("maximum").number(std::numeric_limits<int64_t>::max());
    json.key("error").real(0.125);
    json.key("nan").real(std::numeric_limits<double>::quiet_NaN());
    json.key("ok").boolean(true).append("}");
    assert(json.ok() && json.size() == std::strlen(json.data()));
    if (i == 19999) std::puts(json.data());
  }
  struct { char before[4]; char body[8]; char after[4]; } guarded = {"abc", {}, "xyz"};
  RadioJsonWriter tooSmall(guarded.body, sizeof(guarded.body));
  tooSmall.append("1234567"); assert(tooSmall.ok());
  tooSmall.append("8"); assert(!tooSmall.ok());
  tooSmall.append("anything"); assert(!tooSmall.ok());
  assert(std::strcmp(guarded.before, "abc") == 0 && std::strcmp(guarded.after, "xyz") == 0);
  assert(std::strcmp(guarded.body, "1234567") == 0);
  RadioJsonWriter zero(nullptr, 0); zero.quoted("x"); assert(!zero.ok());
  RadioJsonWriter nullValue(buffer, sizeof(buffer)); nullValue.quoted(nullptr); assert(!nullValue.ok());
  RadioJsonWriter huge(buffer, sizeof(buffer)); huge.real(std::numeric_limits<double>::max()); assert(!huge.ok());
  char source[9000]; std::memset(source, 'x', sizeof(source)); source[8999] = 0;
  RadioJsonWriter oversized(buffer, sizeof(buffer)); oversized.quoted(source); assert(!oversized.ok());
}
'''

class JsonWriterTest(unittest.TestCase):
    def test_bounded_response_roundtrip_and_no_allocation_stress(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / 'json.cpp', Path(tmp) / 'json'
            src.write_text(CPP)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                            '-I', str(HEADER), str(src), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            decoded = json.loads(result.stdout)
            self.assertEqual(decoded['watch'], 'BNE \\"\n\t\r\b\f\x01\x1f é 🕒')
            self.assertEqual(decoded['uptime'], 6307200000)
            self.assertEqual(decoded['minimum'], -(1 << 63))
            self.assertEqual(decoded['maximum'], (1 << 63)-1)
            self.assertEqual(decoded['error'], 0.125)
            self.assertIsNone(decoded['nan'])
            self.assertTrue(decoded['ok'])

if __name__ == '__main__': unittest.main()
