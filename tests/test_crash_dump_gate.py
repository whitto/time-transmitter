#!/usr/bin/env python3
"""Compile and exercise the actual runtime panic gate against bounded SDK stubs."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBRARY = ROOT / "libraries" / "RadioCrashDumpGate"

DRIVER = r'''
#include "RadioCrashDumpGate.h"
#include "esp_core_dump.h"
#include <cassert>
#include <cstring>
#include <cstdio>

extern "C" void __wrap_esp_core_dump_write(panic_info_t *info);
#if SUPPORTS_FLASH
static unsigned realCalls = 0;
static panic_info_t *lastInfo = nullptr;
extern "C" void __real_esp_core_dump_write(panic_info_t *info) {
  ++realCalls; lastInfo = info;
}
#endif

int main(int argc, char **argv) {
  using namespace RadioCrashDumpGate;
  assert(argc == 2);
  panic_info_t info{};
  // Every invocation is a new process: a saved On choice must be explicitly
  // restored by configuration loading, never inferred by the panic gate.
  assert(!enabled());
  assert(available() == bool(SUPPORTS_FLASH));
  __wrap_esp_core_dump_write(&info);
#if SUPPORTS_FLASH
  assert(realCalls == 0);
#endif
  if (std::strcmp(argv[1], "early-boot") == 0) {
    std::puts("Early boot remains Off before saved configuration is restored");
    return 0;
  }
  assert(std::strcmp(argv[1], "toggles") == 0);
  setEnabled(true);
  assert(enabled() == bool(SUPPORTS_FLASH));
  __wrap_esp_core_dump_write(&info);
#if SUPPORTS_FLASH
  assert(realCalls == 1 && lastInfo == &info);
#endif
  setEnabled(false);
  assert(!enabled());
  __wrap_esp_core_dump_write(&info);
#if SUPPORTS_FLASH
  assert(realCalls == 1);
  setEnabled(true);
  __wrap_esp_core_dump_write(nullptr);
  assert(realCalls == 2 && lastInfo == nullptr);  // Pass through without altering SDK input.
  setEnabled(false);
  for (unsigned i = 0; i < 10000; ++i) __wrap_esp_core_dump_write(&info);
  assert(realCalls == 2);  // Disabled calls cannot reach any SDK dump operation.
#endif
  std::puts("Actual gate: default Off, enabled delegation and Off suppression passed");
}
'''


class CrashDumpGateTest(unittest.TestCase):
    def compile_and_run(self, flash_enabled, module_enabled, sanitizers=False):
        self.assertIsNotNone(shutil.which("g++"), "g++ is required")
        with tempfile.TemporaryDirectory(prefix="radioclock-crash-gate-") as tmp:
            directory = Path(tmp)
            (directory / "sdkconfig.h").write_text(
                f"#define CONFIG_ESP_COREDUMP_ENABLE {int(module_enabled)}\n"
                f"#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH {int(flash_enabled)}\n")
            (directory / "esp_attr.h").write_text(
                '#define IRAM_ATTR __attribute__((section(".iram_gate")))\n'
                '#define DRAM_ATTR __attribute__((section(".dram_gate")))\n')
            (directory / "esp_core_dump.h").write_text(
                "#pragma once\nstruct panic_info_t { const void *frame; int core; };\n")
            driver, binary = directory / "driver.cpp", directory / "test"
            driver.write_text(DRIVER)
            command = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                       f"-DSUPPORTS_FLASH={int(flash_enabled and module_enabled)}",
                       "-I", str(directory), "-I", str(LIBRARY / "src"),
                       str(LIBRARY / "src" / "RadioCrashDumpGate.cpp"), str(driver),
                       "-o", str(binary)]
            if sanitizers:
                command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
            subprocess.run(command, check=True)
            environment = dict(os.environ)
            if sanitizers:
                environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
                environment["UBSAN_OPTIONS"] = "halt_on_error=1"
            for case in ("toggles", "early-boot"):
                subprocess.run([str(binary), case], check=True, env=environment)

    def test_actual_supported_gate(self):
        self.compile_and_run(True, True)
        properties = (LIBRARY / "library.properties").read_text()
        self.assertIn("ldflags=-Wl,--wrap=esp_core_dump_write\n", properties)
        self.assertIn("precompiled=true\n", properties)
        self.assertTrue((LIBRARY / "src" / "esp32" / "README.md").is_file())

    def test_unsupported_backends_fail_closed(self):
        # No __real writer is defined in these tests, proving an unsupported
        # backend neither enables nor tries to link/call the SDK dump writer.
        self.compile_and_run(False, True)
        self.compile_and_run(True, False)

    def test_actual_gate_sanitizers(self):
        self.compile_and_run(True, True, sanitizers=True)

    def test_linker_wrap_and_missing_flag_failure(self):
        with tempfile.TemporaryDirectory(prefix="radioclock-crash-link-") as tmp:
            directory = Path(tmp)
            (directory / "sdkconfig.h").write_text(
                "#define CONFIG_ESP_COREDUMP_ENABLE 1\n"
                "#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1\n")
            (directory / "esp_attr.h").write_text("#define IRAM_ATTR\n#define DRAM_ATTR\n")
            (directory / "esp_core_dump.h").write_text(
                "#pragma once\nstruct panic_info_t { int core; };\n")
            (directory / "sdk_panic.cpp").write_text('''
#include "esp_core_dump.h"
extern "C" void esp_core_dump_write(panic_info_t *);
void simulateSdkPanic(panic_info_t *info) { esp_core_dump_write(info); }
''')
            (directory / "driver.cpp").write_text('''
#include "RadioCrashDumpGate.h"
#include "esp_core_dump.h"
#include <cassert>
static unsigned writes = 0;
extern "C" void esp_core_dump_write(panic_info_t *) { ++writes; }
void simulateSdkPanic(panic_info_t *);
int main() {
  panic_info_t info{};
  RadioCrashDumpGate::setEnabled(false); simulateSdkPanic(&info); assert(writes == 0);
  RadioCrashDumpGate::setEnabled(true); simulateSdkPanic(&info); assert(writes == 1);
  RadioCrashDumpGate::setEnabled(false); simulateSdkPanic(&info); assert(writes == 1);
}
''')
            command = ["g++", "-std=c++17", "-O2", "-ffunction-sections",
                       "-fdata-sections", "-Wl,--gc-sections", "-I", str(directory),
                       "-I", str(LIBRARY / "src"),
                       str(LIBRARY / "src" / "RadioCrashDumpGate.cpp"),
                       str(directory / "sdk_panic.cpp"), str(directory / "driver.cpp"),
                       "-o", str(directory / "test")]
            missing_flag = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(missing_flag.returncode, 0)
            self.assertIn("__real_esp_core_dump_write", missing_flag.stderr)
            subprocess.run(command + ["-Wl,--wrap=esp_core_dump_write"], check=True)
            subprocess.run([str(directory / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
