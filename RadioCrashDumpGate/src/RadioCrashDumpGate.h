#pragma once

namespace RadioCrashDumpGate {
// Set this only after the saved configuration has been loaded or committed.
// Before application configuration is loaded, crash dump storage remains Off.
void setEnabled(bool enabled);
bool enabled();
// Reports whether the compiled ESP-IDF backend supports flash core dumps.
bool available();
}  // namespace RadioCrashDumpGate
