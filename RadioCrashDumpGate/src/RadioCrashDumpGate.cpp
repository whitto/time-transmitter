#include "RadioCrashDumpGate.h"

#include <atomic>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "sdkconfig.h"

extern "C" void __real_esp_core_dump_write(panic_info_t *info);

namespace {
// The pinned Xtensa compiler does not advertise all atomic operations as
// always lock-free. Only aligned 32-bit loads/stores are used here; release
// disassembly verifies that they use inline DRAM instructions, not helpers.
// No libatomic helper may be needed while handling a panic.
DRAM_ATTR std::atomic<uint32_t> crashDumpEnabled{0};
static_assert(sizeof(uint32_t) == 4 && sizeof(crashDumpEnabled) == 4 &&
                  alignof(decltype(crashDumpEnabled)) >= 4,
              "The panic gate requires one aligned 32-bit atomic word");
}  // namespace

namespace RadioCrashDumpGate {
bool available() {
#if CONFIG_ESP_COREDUMP_ENABLE && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  return true;
#else
  return false;
#endif
}

void setEnabled(bool enabled) {
#if CONFIG_ESP_COREDUMP_ENABLE && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  // GNU --wrap resolves this name to the real SDK writer. Retain the reference
  // even when the gate is Off so missing linker metadata fails the build,
  // instead of silently leaving the SDK panic path outside the gate.
  asm volatile("" : : "r"(&__real_esp_core_dump_write));
#endif
  crashDumpEnabled.store(enabled && available() ? 1U : 0U,
                         std::memory_order_relaxed);
}

bool enabled() {
  return crashDumpEnabled.load(std::memory_order_relaxed) != 0;
}
}  // namespace RadioCrashDumpGate

// library.properties supplies --wrap for both Arduino IDE and CLI builds.
// The SDK panic handler references this symbol from a separate archive object;
// Off therefore returns before the SDK writer can erase or write flash.
// Keep this path in IRAM and use no allocation, locks, logging or filesystem.
extern "C" void IRAM_ATTR __wrap_esp_core_dump_write(panic_info_t *info) {
#if CONFIG_ESP_COREDUMP_ENABLE && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  if (crashDumpEnabled.load(std::memory_order_relaxed) != 0)
    __real_esp_core_dump_write(info);
#else
  (void)info;
#endif
}
