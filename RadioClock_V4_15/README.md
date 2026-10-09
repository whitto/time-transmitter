# RadioClock V4.15 build R1 — Arduino source

Open `RadioClock_V4_15.ino` in Arduino IDE **2.3.4**. Keep this complete **15-file folder** together: the sketch, twelve headers, `partitions.csv` and this README. The headers are `RadioBleArbiter.h`, `RadioBleScanControl.h`, `CasioBxProtocol.h`, `CasioWatchSettings.h`, `CasioWatchBattery.h`, `BtSyncLed.h`, `BtSyncHistory.h`, `BtRecentSyncs.h`, `RadioWifiAccessWindow.h`, `RadioConfigWriter.h`, `RadioJsonWriter.h` and `RadioReliability.h`. The UI is embedded; no separate UI upload is needed.

Install **esp32 by Espressif Systems 3.3.12** and **ArduinoJson 6.21.5**. For a classic 4 MB Node32 / Node32s, choose **Node32s**, **40 MHz** flash and **No OTA (Large APP)**. ESP32 Dev Module with **4MB / Huge APP** is also supported. The supplied partition table defines the project's 3 MB application layout; the default small partition is insufficient. Routine uploads do not require erasing all flash.

Install all three included source libraries into your Arduino sketchbook's `libraries` folder and restart the IDE:

- **NimBLE-Arduino 2.5.1-radioclock.1**: replace the ordinary Library Manager 2.5.1 installation with the complete `NimBLE-Arduino` folder. Remove duplicate unpatched installations. This pinned version includes the upstream host-timer shutdown correction and checked initialization allocations; the firmware rejects an unpatched build. Alternatively install <https://github.com/whitto/time-transmitter/archive/refs/tags/nimble-v2.5.1-radio-r2.zip> using **Sketch → Include Library → Add .ZIP Library**. Keep the source manifest, patch and original licenses.
- **RadioCrashDumpGate 1.0.0**: keep all five files, including `library.properties`, `src/` and `src/esp32/README.md`. Alternatively install <https://github.com/whitto/time-transmitter/archive/refs/tags/crash-gate-v1.0.0.zip> with **Add .ZIP Library**. Its linker metadata is required; missing metadata deliberately fails compilation. The helper targets classic ESP32; other targets need separate validation.

- **RadioBoundedWebServer 3.3.12-radioclock.1**: install the complete separately named `RadioBoundedWebServer` folder. It contains the guarded server source and provenance manifest; the standard ESP32 board-core WebServer remains installed. It preserves dashboard text forms/JSON while bounding request size/duration; file uploads and transfer-encoded bodies are unsupported.

The full repository stores these under `libraries/`. The Arduino-only ZIP places all three library folders beside the sketch. Cloud compile scripts install and verify all three automatically. These packages contain source, not compiled binaries.

V4.15 retains the prior approved long-run reliability corrections: bounded and verified configuration/schedule saves, no recurring failed-save retries, preserved storage on mount failure, automatic router recovery, nonblocking network startup, NTP wakeups before clock-trust deadlines, bounded Bluetooth transactions, safe callback teardown, RF command ownership and timing-progress safeguards. Uptime uses the 64-bit monotonic clock; drift confidence uses conservative absolute measurements. Diagnostics exposes RAM-only faults, progress and stack headroom; its full refresh runs only when needed.

V4.15 fixes all nine further review findings: three-reply/30-second NTP acquisition and reacquisition, bounded no-client BLE shutdown progress, checked HTTP body/header sizes and absolute deadlines, final BT clock-confidence checks, timer retry replenishment after 30 minutes of qualified health, strict versioned/CRC saved settings, checked Wi-Fi shutdown with RAM faults/backoff, drift aggregation across short NTP intervals, and 32-byte UTF-8 SSID limits. The RF encoder and watch packet formats remain intact. Initial or corrected time is provisional until the NTP confirmation window completes.

Watch (BLE) displays the last four successful or failed BT attempts for the selected watch, newest first, with the attempt date/time and protocol. The fixed 224-byte history is RAM only, clears on reboot/watch replacement and is never restored from the daily snapshot. Overview and Diagnostics both display uptime in hours, minutes and seconds.

A failed configuration save retains the previous file and reports failure. Background retries stop; an explicit successful user save clears the RAM fault. Boot rejects oversized/malformed configuration without deleting it. A filesystem mount failure preserves settings, keeps RF off and provides a setup AP. **Settings → Storage recovery** permits an explicitly confirmed erase only in the mount-fault AP. This removes Wi-Fi/configuration/schedules/BT history and restarts; ordinary save errors do not authorize formatting. A freshly erased board may require this explicit initialization.

Station retries retain AP access and use bounded backoff. Scheduled Wi-Fi wakes for adaptive NTP deadlines independently of user/LF/BT windows. Invalid or implausible NTP replies are rejected before changing the clock, and untrusted time keeps LF off. The startup access window expires once. RF timer/task faults force the carrier off; bounded recovery and, for unrecoverable stalls, controlled restarts avoid leaving an active carrier or silent hung device. There are no periodic maintenance reboots or flash logs.

**Settings → Crash dumps → Save crash dumps to flash** remains saved and default Off. Off blocks new SDK dumps and retains old dumps. On permits a future panic dump in the dedicated 64 KB partition without a daily quota. The gate starts Off before settings load. Normal serial panic/backtrace output remains available. Application/LittleFS addresses and sizes are unchanged.

**Watch (BLE) → Save BT sync status to flash** remains On by default: save the first successful TIME delivery each Brisbane day immediately as a minimal validated snapshot, including timestamps/watch identity, quota metadata and four diagnostic counters. Later events stay in RAM. Off disables writes/restoration; toggling/rebooting does not reset a saved day's quota. No battery/font/error/serial log is persisted. Required changed settings/watch bindings still save normally.

The saved GPIO2 LED control remains: flash during actual BT sync only, solid for 24 hours after TIME success, off after an actual failed transaction. Passive listening, Wi-Fi and JJY/LF never make it flash. Master Off remains dark.

**Settings → Daily Wi-Fi access** retains independent times/timezone, default disabled with 18:00–20:00 Brisbane ready to select. Overnight windows work; equal endpoints are rejected. Always on overrides the window. Setup/recovery access and brief NTP wakeups remain exceptions. Wi-Fi tiles are green for station/red for AP; radio is green active/red idle. All eight sidebar pages remain.

GW-BX5600 optional font changes precede final TIME, with packet length/unrelated settings preserved and readback. Experimental battery estimates are read before the handshake, shown per watch with date/time, and kept in RAM; battery-only errors do not prevent TIME. Other protocols have no validated battery calibration. RF keeps priority over BLE. JJY and BT settings/timezones remain independent.

Focused checks and the required pre-publication reliability review are recorded in the full repository’s `docs/V4.15-review.md`. Source is published before the full suite, with results added to release notes afterward as requested. Physical Node32s/watch acceptance and a long soak remain required; cloud checks cannot guarantee years of uninterrupted service.

With no saved credentials, connect to **RadioStation_XXXXXX**, password **12345678**, and open **http://192.168.4.1**.

Project and credits: <https://github.com/whitto/time-transmitter>. Original RadioClock notices credit **tarohs/nisejjy** (2021) and **5Breeze/ClockWaveXmitter** (2025); they remain in the sketch. Casio packet/font/battery references include **izivkov/gshock-smart-sync-webapp** and **izivkov/gshock_api**. NimBLE source retains its authors' licenses and notices.

Full ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/v4.15.zip>. Arduino source and all three libraries: <https://github.com/whitto/time-transmitter/archive/refs/tags/arduino-v4.15.zip>.
