# RadioCrashDumpGate

This source library makes ESP32 flash crash dumps optional. Time Transmitter
loads its saved setting and calls `RadioCrashDumpGate::setEnabled()`. The default
is Off, including during early startup before saved configuration is loaded.

Install this folder as `RadioCrashDumpGate` in your Arduino sketchbook's
`libraries` folder, then restart Arduino IDE if it is already open. Keep
`library.properties` and `src/` together. The firmware must include
`<RadioCrashDumpGate.h>` so the Arduino builder loads this library and its
linker flag. The project's cloud compile script also supplies the library path.

`library.properties` uses `-Wl,--wrap=esp_core_dump_write`. Arduino's
`precompiled=true` mixed-library mode and the textual `src/esp32/README.md`
marker allow the builder to supply that flag while compiling the sources
normally. No compiled archives are included. Keep that target marker with the
library; a missing linker flag deliberately fails the build.

The wrapper stays in
IRAM and reads a lock-free 32-bit atomic flag in internal DRAM. Off returns
without calling the SDK crash dump writer. On delegates to the unchanged ESP-IDF
writer and uses the firmware's 64 KB coredump partition. This flag gates crash
dump storage only; the usual serial panic message and backtrace still run.

Switching Off does not erase a previously stored dump, and does not write a new
dump. Enabling does not create a dump immediately; it permits a future panic
to store one. Routine serial logging is not written to flash by this library.
The normal SDK check for an existing dump at startup only reads flash.

The supported release toolchain is Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5.
`available()` reports whether the compiled SDK has its flash coredump backend
enabled. The companion coredump partition must also be uploaded with the sketch.
Linker wrapping is checked in the release ELF: the SDK panic handler must call
`__wrap_esp_core_dump_write`, and its enabled branch must call the real writer.
Revalidate that call path when changing the board core or toolchain.
The provided linker-option directory targets classic ESP32. Other MCU targets
need equivalent target-directory metadata and their own linked validation.

There is no supported public runtime-disable API in the pinned ESP-IDF SDK.
Using this small library supplies the linker flag through the standard Arduino
library mechanism without modifying the installed board package or SDK.
