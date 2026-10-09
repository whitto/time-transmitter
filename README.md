# Time Transmitter — RadioClock V4.14 build R2

Time Transmitter is an ESP32 project for synchronizing "atomic" watches (Casio and Citizen tested) with a esp32 generating a local LF time signal.  Additionally I've added   Casio Bluetooth time delivery for some supported watches (tested on a Gshock Square GW-BX5600) this project combines an NTP-disciplined clock, configurable transmission schedules, a browser settings/dashboard UI, and hardware-timed carrier generation.
The transmitter uses a simple GPIO pin a pair of 330 ohm resistors (in parallel) to reduce the GPIO current (thats all I had) and a coil of about 150 turns of copper magnet wire around a 10mm ferrite rod of about 120mm length (from Jaycar)

The range of the transmitter (in JJY40E mode) is approximately 3-5 meters.  Which is much more than expected, and heaps for me (your results may vary)

Casio watch font changes (during sync) now work for those who don't want to use the Casio app to change GW-BX5600 fonts.
Casio GW-BX5600 battery level reporting (after a time sync) is experimental.

RF transmission has priority over Bluetooth: scanning and connections stop, the BLE host/controller shut down, and the JJY carrier starts only after shutdown is confirmed. Bluetooth stays off for the whole RF session, including reduced/zero-carrier envelope slots.

![RadioClock V4 dashboard preview](ui/v4_dashboard.png)

Dashboard preview with sample status data.

## Features

- V4 dashboard based on the supplied UI mockup: dark sidebar, skyline clock banner, five live status tiles, transmission controls, schedules, Casio watch card and shortcuts. The layout adapts to phones and includes a saved light/dark theme.
- LF carrier generation with ESP32 LEDC and a hardware timer; envelope timing follows the system clock.
- Scheduled or continuous transmission, overlapping-schedule rotation, timezone selection, and a configurable transmission offset.
- NTP synchronization, adaptive resynchronization, clock-confidence checks, and holdover handling.
- Watch-initiated Casio BLE time delivery: GW-BX5600 MIP, selected standard digital/hybrid models, and an experimental analogue time-only profile.
- Separate Pair Watch and Sync Now controls, persistent watch profiles, safe manual retries, and optional Always Wait listening for the paired watch.
- The Watch card has an independent Bluetooth time-zone selector and additive offset, defaulting to Australia/Brisbane. The JJY/LF station time remains controlled by its own timezone and transmission offset.
- The main dashboard shows both the JJY transmitted time and the current civil time that the next Bluetooth watch write will use.
- Successful Bluetooth syncs show their date and time in the overview hero, watch card, and quick status. Automatic watch-sync slots are interpreted in the selected Bluetooth timezone plus offset, while LF schedules stay in the JJY/station timezone.
- A saved blue ESP32 BT sync indicator toggle: Off keeps GPIO2 dark; On flashes only during an actual Bluetooth connection/time-sync transaction. Successful time delivery leaves it solid for 24 hours; a failed BT transaction clears it. LF transmission, Wi-Fi and idle watch listening never make it flash.
- Optional compact Bluetooth status retention: immediately save the first successful sync each Brisbane calendar day, including UTC success timestamps, watch identity and four diagnostic counters. Later events stay in RAM. No serial logs, font results, battery readings or error text are stored in this snapshot.
- Optional crash dumps in Settings, default Off: allow the ESP32 SDK to store a diagnostic dump only after a panic, independently of the daily BT status snapshot. Ordinary serial logs are not stored in flash.
- Per-watch GW-BX5600 font selection, applied and checked before the final time command while preserving the watch's 12- or 17-byte settings format; optional controller shutdown between scheduled watch windows.
- GW-BX5600 battery estimate read during Bluetooth sync, shown with the reading date/time on Watch (BLE) and on the overview’s top Watch card. Readings are per watch, kept in RAM and cleared on reboot.
- Bluetooth scan operations serialized with NimBLE callbacks, avoiding concurrent scan-completion cleanup and retaining active watch discovery.
- Diagnostics shows Always Wait state (including RF pause), the next JJY date/time in the station timezone, and reset/memory evidence for troubleshooting.
- A gzip-compressed web UI, LittleFS configuration storage, and Always on or Scheduled/power-save Wi-Fi, with an independent daily web-access window in Settings.
- Green Wi-Fi status for a connected station and red for setup AP mode; green radio status during active LF transmission and red while idle.
- Embedded reliability safeguards: checked atomic saves, preserved settings on mount failure, automatic router recovery, clock-trust NTP wakeups, bounded Bluetooth work, RF timing/progress monitoring and 64-bit uptime. Fault and headroom measurements stay in RAM.

| Time-signal format | Carrier |
| --- | --- |
| JJY East / West | 40 / 60 kHz |
| WWVB | 60 kHz |
| DCF77 | 77.5 kHz |
| MSF | 60 kHz |
| BPC | 68.5 kHz |
| BSF legacy experimental mode | 77.5 kHz, carrier only |

This is a local receiver emulator. Formats follow the selected timezone/offset; they do not necessarily reproduce the official station's civil-time convention. The existing JJY implementation keeps ordinary frames through the real station's call-sign periods for local watch acquisition.

## Hardware

The validated build target is a classic ESP32 Node32 / ESP32 Dev Module with 4 MB flash. The sketch's classic ESP32 pin assignments are:

| Output | GPIO |
| --- | --- |
| LF carrier to transmitter circuit | 26 |
| Buzzer envelope feedback | 27 |
| RF envelope LED | 25 |
| Blue activity LED (optional) | 2 |

The firmware provides the carrier output for an external transmitter/antenna circuit. ESP32-C3 pin mappings are retained in the source, but that target has not been compile-tested for this version.

## Build

The build target is the classic ESP32 Node32 / ESP32 Dev Module with 4 MB flash, Arduino-ESP32 **3.3.12**, the included patched NimBLE-Arduino **2.5.1-radioclock.1**, and ArduinoJson **6.21.5**. The sketch-local partition table has a 3 MB application partition and LittleFS storage.

For Arduino IDE, install those board/library versions and open:

`firmware/RadioClock_V4_14/RadioClock_V4_14.ino`

Keep all eleven companion headers—`RadioBleArbiter.h`, `RadioBleScanControl.h`, `CasioBxProtocol.h`, `CasioWatchSettings.h`, `CasioWatchBattery.h`, `BtSyncLed.h`, `BtSyncHistory.h`, `RadioWifiAccessWindow.h`, `RadioConfigWriter.h`, `RadioJsonWriter.h` and `RadioReliability.h`—and `partitions.csv` beside the sketch. For **ESP32 Dev Module**, select **Flash Size: 4MB (32Mb)** and **Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**. The default 1.25 MB application partition is too small for this project. On **Node32s**, **No OTA (Large APP)** also provides sufficient compile capacity; the included sketch-local partition table supplies the project's 3 MB application layout. This layout has one application slot and does not support dual-slot OTA updates.

V4.14 also requires the included **RadioCrashDumpGate 1.0.0** source library. Copy `libraries/RadioCrashDumpGate` from the full repository, or `RadioCrashDumpGate` from the Arduino-only archive, into your Arduino sketchbook's `libraries` folder. Keep all five files, including `library.properties`, `src/` and its `esp32/README.md` marker, together and restart Arduino IDE. Alternatively, download [the library-only ZIP](https://github.com/whitto/time-transmitter/archive/refs/tags/crash-gate-v1.0.0.zip) and use **Sketch → Include Library → Add .ZIP Library**. This library supplies the linker option needed for the crash-dump toggle; a missing `RadioCrashDumpGate.h` means it has not been installed. Missing linker metadata deliberately fails the build. The cloud compile script installs the included library automatically. The Arduino-only ZIP contains the complete 14-file sketch folder, the five-file crash-gate library and the complete patched NimBLE source library; no compiled library archive is supplied. The helper targets classic ESP32; other MCU targets require their own linker metadata and validation.

V4.14 R2 also requires the included **patched NimBLE-Arduino 2.5.1-radioclock.1**. Replace the ordinary Library Manager 2.5.1 installation in your Arduino sketchbook's `libraries` folder with the complete `libraries/NimBLE-Arduino` folder from this repository, or the `NimBLE-Arduino` folder from the Arduino-only ZIP, and restart the IDE. You can instead install [the patched library ZIP](https://github.com/whitto/time-transmitter/archive/refs/tags/nimble-v2.5.1-radio-r2.zip) using **Sketch → Include Library → Add .ZIP Library**. Remove duplicate unpatched NimBLE installations so Arduino selects the included version. The firmware deliberately rejects an unpatched library at compile time. Its source manifest, patch, README and original licenses document the pinned changes; the cloud compile script installs and verifies it automatically.

In the Codex Linux cloud workspace:

```bash
cd /workspace/time-transmitter
bash scripts/install-toolchain.sh
bash scripts/compile.sh
bash scripts/test.sh
```

The installer keeps the pinned tools and packages in `/workspace/.radioclock-tools`, checks the official CLI checksum, and retains Arduino package signature/checksum and TLS verification. It uses injected proxy configuration when present. A later task can reuse the installed snapshot. Cloud tasks already have isolated checkouts; use this checkout without creating a Git worktree.

With no saved Wi-Fi credentials, connect to the visible 2.4 GHz setup network **RadioStation_XXXXXX** (device-specific suffix), password **12345678**, and open **http://192.168.4.1**. V4.10 uses the device AP MAC independently of startup events, instead of naming the network from a not-yet-initialized netif. AP success is logged only after the driver, AP-start event, configuration and local IP are ready. Failed initialization logs its stage and retries after five seconds; configuration is not erased. If the device MAC cannot be read, the fallback SSID is **RadioStation_Setup**. Driver readiness does not prove reception on a phone; if the AP remains absent, capture the new serial error and check another nearby 2.4 GHz client.

Build outputs are in `/workspace/.radioclock-tools/output/RadioClock_V4_14`. Override `RADIOCLOCK_TOOLS_DIR` to move tool storage or `RADIOCLOCK_BUILD_JOBS` to change the default two compiler jobs. `RADIOCLOCK_FQBN` is available for another compatible target, which needs its own validation.

## Watch controls

Select the watch profile and protocol in Settings. Use **Pair Watch** for first pairing or replacement. The existing watch address, name, and protocol are replaced only after the new watch receives an acknowledged time write. **Sync Now** restarts a manual wait window safely and requires an already paired watch.

For the GW-BX5600, module 3578:

- Pair/connect: hold **C for at least 3 seconds** until the Bluetooth symbol and **CONNECT WITH A PHONE** flash.
- Manual time correction: from **Timekeeping Mode, press D once**.

The **BX1** build corrects the SP settings/city record framing and logs the exact
handshake stage, negotiated MTU, and ATT error. It preserves the watch's existing
settings and sends complete packets with their required write modes. See
[the handshake evidence and device check](docs/GW-BX5600-SP-handshake.md).

**Always wait for watch sync requests** listens only for the selected paired watch while RF is idle. JJY/LF has absolute priority: BLE is shut down during RF and resumes afterward. If RF covers an entire automatic Bluetooth window, that watch attempt cannot be received. It never learns an unknown watch. Watches that rotate their BLE address may require explicit pairing again; the binding filter remains strict.

The Watch card's **Bluetooth time zone** and **Bluetooth time offset** apply only to BLE watch writes and automatic Bluetooth sync slots. They default to Australia/Brisbane and zero additional offset, and support the same fixed/DST-aware zones shown by the main clock. JJY and the other LF encoders continue using the main **Time zone** and **Transmission offset** settings. The dashboard's JJY card reports the active radio time and the calculated Bluetooth watch time together, and the overview records the date and time of the last successful Bluetooth write.

Automatic watch slots save when their switches or settings change. Each enabled time repeats every day until you disable it, even if the watch has already synchronized at another time that day. New or unset configurations default all four automatic slots (00:30, 06:30, 12:30 and 18:30 in the Bluetooth timezone) and Always Wait to On; existing saved Off choices stay Off. Saving an LF transmission schedule does not change those switches. Settings writes finish before the UI reports success and survive power restarts; a storage error is reported instead of a successful save. RF still takes priority during overlapping windows.

In **Settings**, **Enable blue ESP32 BT sync indicator** controls the onboard GPIO2 indicator. Off keeps it dark. On flashes only while an actual Bluetooth connection/time-sync transaction is in progress, including blocking watch writes; passive listening does not count. Successful TIME delivery leaves it solid for the following 24 hours, measured from the UTC success timestamp. An actual failed BT transaction turns it off; a missing watch or an RF-deferred attempt does not erase a previous success. Optional battery/font errors do not turn a successful TIME delivery into a failed sync. JJY/other LF transmission never makes this LED flash, although an existing solid success indication continues during RF. The setting saves on toggle change and survives power restarts; existing Off choices remain Off. This control is for the classic ESP32's GPIO2 LED; the existing C3 mapping shares its LED pin with the RF envelope indicator, which this control leaves under RF ownership.

In **Watch (BLE) → Last watch result**, **Save BT sync status to flash** enables one compact daily status snapshot, defaulting to On. The first successful TIME delivery each **Australia/Brisbane calendar day** is saved immediately, regardless of the separately selected Bluetooth timezone. The snapshot contains UTC success timestamps and watch/profile identity, the saved-day quota and validation metadata, and the Bluetooth connection/acknowledged-write/notification/response-error counters. It contains no event log, serial output, font result, battery reading or error text. Routine syncs no longer save the configuration file merely to record history; pairing still saves a changed watch binding as required configuration.

Later successes and failures update RAM only. After reboot, the first saved daily snapshot restores the successful date/time and diagnostic counters; profile completion data is restored only for the matching watch address/protocol. A later RAM-only failure can therefore be forgotten after reboot, and the LED can resume the saved success indication if that snapshot is still within 24 hours. Turning history Off keeps current data in RAM, disables restoration after reboot and invalidates the old snapshot for future re-enabling. Toggling or rebooting does not reset an already saved day's quota. No history is written during idle running or failed syncs. LittleFS may perform internal metadata writes when committing a snapshot; the limit is one successfully committed status snapshot per day, rather than one physical flash operation.

In **Settings → Crash dumps**, **Save crash dumps to flash** defaults to Off and saves on toggle change. Off blocks new SDK panic dumps; On permits a future panic to store a diagnostic dump in the dedicated 64 KB partition. Enabling does not immediately create a dump. Disabling does not erase an existing dump. Crash dumps are separate from the daily BT status snapshot and have no once-per-day limit when enabled. There are no continuous flash logs, and normal serial panic/backtrace output remains available. The gate starts Off before saved configuration is loaded. If the compiled ESP32 core has no supported flash dump backend, the control shows Unavailable.

V4.14 restores the optional core-dump partition omitted in V4.13; application and LittleFS addresses/sizes stay unchanged. The required RadioCrashDumpGate library controls whether the SDK writer runs. Normal uploads do not require erasing saved settings.

In V4.14 R2, a failed configuration save retains the previous file, reports an error and stops background retries. Retry with an explicit user save after correcting the fault. Schedule saves reject incomplete or invalid JSON before opening a temporary file. Both saves verify the full temporary record before replacing the previous one. No periodic configuration rewrite or flash log is added.

LittleFS mount failures preserve the filesystem and keep a setup AP available with RF off. Settings shows storage recovery only for an actual mount fault. **Erase saved settings and restart** requires a destructive confirmation; it erases Wi-Fi credentials, configuration, schedules and BT history. A normal save fault does not authorize formatting. A freshly erased board may need this explicit first-time filesystem initialization. Routine firmware updates should retain settings and do not require erasing all flash.

Station recovery keeps setup access available while retrying saved credentials with bounded backoff. Scheduled/power-save Wi-Fi also wakes ahead of adaptive NTP deadlines independently of radio, watch and web-access schedules. Untrusted time keeps LF transmission off. Startup access expires once and does not reopen after `millis()` rollover. Diagnostics adds RAM-only timing faults, task progress and stack headroom; unrecoverable stalls can trigger a controlled restart after forcing RF off. There are no periodic maintenance reboots.

In **Watch (BLE)**, select the GW-BX5600 profile, enable **Apply watch font during BT sync**, then select **Standard (new font)** or **Classic**. The choice saves for that profile and applies during the next Bluetooth sync with this ESP32. Off keeps the current font untouched. V4.11 reads the watch's 12- or 17-byte settings packet, preserves its length and unrelated bytes, changes only the font bit and reads it back before the final time command. It calculates the current time after the font transaction so the delivered time includes that elapsed work. A font error is reported separately from a successful time delivery. A phone-only sync cannot use an ESP32 setting; the watch must connect to RadioClock.

During each **GW-BX5600** Bluetooth sync, V4.12 requests the watch’s battery condition before the required time handshake. It shows a battery **estimate in 10% steps** for the selected watch profile, plus the last reading date/time in the Bluetooth timezone and offset. A missing or malformed reply does not prevent a time attempt. The 1.5-second battery reply deadline preserves the existing 5-second SP/font reply deadlines. Readings stay in RAM and clear on reboot; a replacement watch cannot inherit the old watch’s reading. Other watch protocols show unavailable because their calibration has not been validated. This feature adds no flash writes. See [the battery protocol and validation](docs/V4.12-review.md).

In **Settings**, **Power down idle Bluetooth** turns the Bluetooth controller off outside scheduled listening windows and wakes for explicit Pair Watch/Sync Now requests. **Always Wait** takes priority; turn it off to obtain controller savings. For additional savings use the existing Wi-Fi power-save mode and disable the blue LED. This option preserves the CPU clock/RF scheduler and does not enable deep or full-chip light sleep. See [the power-saving assessment](docs/V4.6-power-saving.md).

**Disable Wi-Fi sleep / Keep Wi-Fi always on** uses the existing Wi-Fi power-mode enum: enabled is Always on (`0`); disabled is Scheduled/power-save (`1`). It also applies the corresponding Wi-Fi driver sleep setting. There is no independent persistent sleep flag.

In **Settings → Daily Wi-Fi access**, select start/end times and an independent timezone for daily web-interface access. The default is disabled, with **18:00–20:00 Australia/Brisbane** ready to select. Enabling the window saves automatically and selects Power-save; start/end/timezone edits save together using **Save Wi-Fi access schedule**. The window repeats daily, includes its start and excludes its end, and supports overnight periods. Equal start/end times are rejected. **Network → Always on** overrides the window. Disabling the window leaves the selected power mode unchanged. Setup AP, five-minute startup/recovery access and existing brief NTP wakeups remain available outside the window. The Wi-Fi timezone is independent of both BT sync time and the LF station timezone.

The overview's Wi-Fi tile is green for a normal station connection and red in setup AP mode, including AP-plus-station operation. The radio tile is green while the LF transmitter is active and red while idle.

## Editable UI and validation

The live source is `ui/radioclock.html`.

V4.7 added Diagnostics summaries for full-time Bluetooth listening and the next JJY window, coalesces overlapping diagnostic refreshes, and reports reset reason, largest heap block and loop-stack headroom. It targets Arduino-ESP32 3.3.12. Earlier intermittent bootloader checksum failures remain under hardware investigation; they are separate from the Wi-Fi/NTP setup bug. V4.6 added per-watch font changes during Bluetooth sync, idle Bluetooth controller power saving and the persisted blue LED activity control, and defaults unset Bluetooth schedules/Always Wait to On. A separate low-priority task flashes the indicator without changing radio timing or Bluetooth writes. The V4.5 clock, scheduling, timezone, persistence and editor fixes remain in place.

The main page follows `ui/v32_ui_mockup.png`, using self-contained SVG icons and a skyline illustration without external fonts, images or scripts. The fixed left sidebar keeps all eight labelled items at every screen width: Overview, Radio, Watch (BLE), Schedules, Network, Settings, Diagnostics and About. Each opens its own page. Radio and LF schedule controls use the same DOM elements as their overview cards, so unsaved edits survive navigation. Its controls call the existing APIs, and JJY/LF and Bluetooth time remain separately visible.

Before publishing, compare the rendered UI against the user's request and the approved mockup, then complete a fresh embedded reliability review. Check memory and allocation failures, task/callback lifetimes, deadlines and rollover, RF/BLE exclusion, network recovery, durable settings and flash-write frequency. Address regressions before publication. Ask before changing the fundamental layout or departing from that design. These requirements are recorded in `AGENTS.md`.

[Download the V4.14 R2 source ZIP](https://github.com/whitto/time-transmitter/archive/refs/tags/v4.14-r2.zip).

[Download only the Arduino source files](https://github.com/whitto/time-transmitter/archive/refs/tags/arduino-v4.14-r2.zip).

[V4.14 R2 release notes](https://github.com/whitto/time-transmitter/releases/tag/v4.14-r2).

[Download the session handoff Markdown](https://raw.githubusercontent.com/whitto/time-transmitter/v4.14-r2/docs/SESSION_HANDOFF.md).

After editing the UI, regenerate its embedded gzip asset and run checks:

```bash
python3 scripts/embed-ui.py
bash scripts/test.sh
bash scripts/compile.sh
```

The checks run the actual UI script against mocked APIs, compile extracted firmware functions against host mocks, stress the real atomic RF/BLE arbiter concurrently, and verify gzip, DOM references, API routes, versions, and partition bounds. They exercise state transitions and failures; they do not emulate the ESP32 radio or prove the watch display changed. With Playwright and Chromium installed, run `node scripts/test-ui-browser.cjs` for the real-browser schedule, toggle and pairing regressions; set `RADIOCLOCK_CHROMIUM` if the browser executable is elsewhere.

V4.14 R2 implements the user's approved long-run reliability changes: safe saves under low memory, no recurring failed-config retries, non-destructive filesystem recovery, the pinned NimBLE host-timer correction, bounded BT deadlines and allocation admission, nonblocking Wi-Fi/NTP recovery, trust-driven NTP wakes, ownership-safe RF pauses, checked timing startup/progress, rollover-safe uptime and conservative drift confidence. Common JSON responses use bounded storage and Diagnostics refreshes on demand. See [the R2 release review](docs/V4.14-R2-review.md) and [the updated reliability report](docs/LONG_RUN_RELIABILITY_REVIEW.md). V4.14 R1 introduced the optional crash-dump gate; its [original review](docs/V4.14-review.md) remains historical evidence. V4.13's approved LED/status/Wi-Fi behavior and compact daily snapshot remain; R2 also removes the previously queued recurring failed-config background writes. V4.11 serializes scan start/stop with the NimBLE host callbacks, disables the affected independent scan-response timer while retaining active scanning, and corrects the GW-BX5600 font packet length and transaction order. Its verification is recorded in `docs/V4.11-review.md`; physical watch/device acceptance is still required. V4.10 derives a stable setup SSID before AP-start events, checks AP readiness and retries failed starts without writing configuration. V4.9 fixes the V4.8 cold setup boot assertion by avoiding SNTP shutdown before its client/network startup. V4.8 introduced Wi-Fi/NTP recovery, Settings power controls and clearer watch delivery. See `docs/V4.12-review.md` for battery evidence, `docs/V4.11-review.md` for scan/font evidence and `docs/SESSION_HANDOFF.md` for current continuation instructions. `docs/V4.10-review.md` records the previous release checks. `docs/V4.5-review.md`, `docs/V4.2-review.md` and `docs/V3.5-review.md` record earlier changes and hardware acceptance steps. `baseline/`, `diff/`, `docs/CODEX_HANDOFF.md`, and `docs/RadioClock_Senior_Review.md` are historical handoff material. `ui/v321_ui_baseline.html` is the original UI, kept for comparison.

## Build status

V4.14 R2 validation and the required final reliability review are recorded in [docs/V4.14-R2-review.md](docs/V4.14-R2-review.md), with exact release build/test evidence captured before publication. Fault tests exercise allocation/storage failures, late RF commands, network outages, NTP trust, callback teardown, progress recovery and long uptime. The cloud compile also checks the actual ELF crash-gate call path. No physical ESP32/watch is attached to the cloud; host checks and a successful compile cannot establish years of hardware reliability. The user's README update reports that font changes now work.

## Credits and provenance

The supplied RadioClock source retains these original credits:

- **taroh (GitHub: tarohs)** — original [nisejjy](https://github.com/tarohs/nisejjy) software-radio code, copyright **2021**.
- **5Breeze** — [ClockWaveXmitter](https://github.com/5Breeze/ClockWaveXmitter) improvements, copyright **2025**; the source identifies web UI, Wi-Fi configuration, multi-station scheduling, and station rotation additions. Its README also credits nisejjy and requests source credit for redistribution, adaptation, or commercial use.
- **RadioClock V2.7 through V3.2.2** — the supplied firmware lineage and baseline used for this project's V3.5 continuation. The V3.2.1 source is preserved in `baseline/` for comparison.

Those copyright notices remain in the firmware headers. V4.14 preserves the V4.1 dashboard and V3.5 BLE lifecycle/write-mode corrections, transactional pairing, passive listening, RF/BLE arbitration and build tooling, with the scan/font corrections, battery reporting and approved LED/status/Wi-Fi changes described above.

The project also uses these libraries and platforms:

- [Arduino-ESP32](https://github.com/espressif/arduino-esp32) — Espressif's Arduino platform and its Wi-Fi, WebServer, LittleFS, timer, and LEDC integrations.
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — the NimBLE-based Bluetooth stack maintained by h2zero and contributors, building on Apache Mynewt NimBLE.
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing and serialization by Benoît Blanchon and contributors.
- [Arduino CLI](https://github.com/arduino/arduino-cli) — reproducible cloud builds and dependency installation.
- [izivkov/gshock-smart-sync-webapp](https://github.com/izivkov/gshock-smart-sync-webapp) — reference for the basic-settings font byte/bit, GW-BX5600 battery calibration and characteristic mapping. The font helper is independently implemented and preserves the other watch settings; the checked reference is commit `5b3ff83dbae43a7a526232e5f35de98425ca7ac8`.
- [izivkov/gshock_api](https://github.com/izivkov/gshock_api) — public GW-BX5600 Bluetooth captures used to check SP packet structure and battery-condition requests/replies. The BX1 packet helper was implemented independently from those observations.

The JJY implementation references [NICT's time-signal specification](https://jjy.nict.go.jp/jjy/trans/index-e.html). Firmware comments also acknowledge NIST (WWVB), PTB (DCF77), NPL (MSF), and BPC signal specifications. These are standards references, separate from source-code authorship.

Neither nisejjy nor ClockWaveXmitter currently declares a standard software license or includes a LICENSE file, and the uploaded handoff did not include a project-level license grant. This repository preserves the supplied author notices and does not invent a license for inherited code. Dependencies retain their own licenses, including Arduino-ESP32's [LGPL 2.1](https://github.com/espressif/arduino-esp32/blob/3.3.12/LICENSE.md) and NimBLE-Arduino's [Apache 2.0](https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/LICENSE).
