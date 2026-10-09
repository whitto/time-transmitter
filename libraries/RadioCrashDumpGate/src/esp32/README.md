This is the classic ESP32 source-only linker-option directory. No compiled
archive is shipped here. Arduino's mixed-library mode (`precompiled=true`)
compiles `RadioCrashDumpGate.cpp` normally and supplies `library.properties`
linker flags when this target directory exists and contains a file.

The pinned Arduino CLI 1.3.1 applies library linker flags only through this
path. Keep this file with the library. `setEnabled()` also retains a reference
to the GNU `__real_` symbol, so omitting the linker flag fails the build rather
than allowing unguarded SDK flash crash dumps.

The release validates classic ESP32 Node32 / ESP32 Dev Module. A different MCU
requires its own equivalent target directory and linked panic-path validation.
