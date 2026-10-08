# Time Transmitter / RadioClock — session handoff

Prepared on **8 October 2026**, using the user's **Australia/Brisbane** timezone.

## Read this first

This is maintenance of the user's own ESP32 watch transmitter. The latest request is to read the watch battery during Bluetooth sync and show it on Watch (BLE) and the overview’s top Watch card, followed by a versioned source release and a direct Arduino-source ZIP. The earlier panic/font corrections must be preserved. They also requested this Markdown handoff and links to the project in the main-page header and About section.

**Status at handoff: V4.12 adds GW-BX5600 battery estimates during sync and both requested UI displays. It retains V4.11’s scan fix, font correction and project links. Cloud validation is recorded in `docs/V4.12-review.md`; physical ESP32/watch acceptance remains outstanding.** A successful compile or host test does not establish physical device/watch behavior.

Read `AGENTS.md`, the root `README.md`, `docs/NEXT_VERSION.md`, and `docs/V4.12-review.md` before editing. Preserve existing user changes.

## Repository and current release

- Repository: <https://github.com/whitto/time-transmitter>.
- Cloud checkout: `/workspace/time-transmitter`, branch `main`.
- Current release: **V4.12**, tag `v4.12`; resolve its commit with `git rev-parse v4.12^{commit}`.
- Previous V4.11 release commit: `9edbdc2462b76757cabba9ca689e62689c6e93b8`.
- V4.12 release: <https://github.com/whitto/time-transmitter/releases/tag/v4.12>.
- V4.12 source ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/v4.12.zip>.
- Arduino files only: <https://github.com/whitto/time-transmitter/releases/download/v4.12/RadioClock_V4_12_Arduino_Source.zip>.
- Download this handoff: <https://raw.githubusercontent.com/whitto/time-transmitter/v4.12/docs/SESSION_HANDOFF.md>.
- Previous V4.10 release commit: **`40f309b0ddfac04095ac58d6c821d9df4b26e67f`**.
- V4.10 release: <https://github.com/whitto/time-transmitter/releases/tag/v4.10>.
- V4.10 source ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/v4.10.zip>.
- Current active source: `firmware/RadioClock_V4_12/RadioClock_V4_12.ino`; V4.11 and V4.10 is retained for comparison.
- The current version contains `CasioWatchBattery.h`, per-profile battery status in RAM, `RadioBleScanControl.h`, the corrected font helper, tests, project links and this handoff. Check `git status` and actual diffs before editing; later work may have progressed.
- UI source: `ui/radioclock.html`; approved layout reference: `ui/v32_ui_mockup.png`.
- The previous V4.11 ZIP was downloaded and verified against all 162 tracked release files. Repeat that verification for each published version, including the Arduino-source-only ZIP.

## V4.12 battery feature

The identified GW-BX5600 uses GET **0x28** on `26eb002c` with **Write Without Response**, replying on TIME/all-features `26eb002d`. Observed replies are exactly nine bytes. Byte 1 is the calibrated raw battery level: **14–24 maps to 0–100% in 10% steps**, clamped at either end. Actual BX captures are `281418000000020000` (60%, D-button session) and `281821000000020000` (100%, official app). The standard Battery Service is not required or assumed.

The optional read runs before SP step 1 and final TIME; its reply wait is limited to 1.5 seconds including the 300 ms notification assembly gap. Discovery/subscription remain existing guarded GATT operations. A battery-only failure does not block time delivery. Readings commit only after successful TIME, after a replacement binding is applied. They are per profile, tied to the address and validated protocol, and kept in **RAM only**. A failed read retains the same watch’s last known sample with a failure status; a replacement cannot inherit that sample. Reboot clears readings.

Each `bt_profiles` API object adds nullable `battery_percent`, `battery_read_at` in Bluetooth civil time, and `battery_status`. The Watch page and top Watch tile follow the selected profile. A real 0% remains distinct from unavailable. Other model protocols show unavailable rather than using the wrong calibration.

Source evidence and build/test results are in `docs/V4.12-review.md`. Test on the device by pressing D once from Timekeeping with RF idle; check the `BT: Watch N battery estimate …` serial line, the two UI displays, reading time, time delivery, font behavior, replacement/profile isolation and restart clearing.

## Hardware and dependencies

The user uses a classic **Node32 / Node32s ESP32**, Arduino IDE **2.3.4**, a connected transmitter coil, **40 MHz flash**, and **No OTA (Large APP)**. They previously had **Erase All Flash Before Sketch Upload** enabled. Their exact flash-size/mode settings should be taken from the current build or confirmed if needed; do not recommend unnecessary erases.

The user explicitly confirmed **Arduino-ESP32 3.3.12** and **NimBLE-Arduino 2.5.1**. The cloud toolchain also pins **ArduinoJson 6.21.5** and Arduino CLI **1.3.1**. The target is 4 MB flash, with the supplied sketch-local `partitions.csv` defining a 3 MB application partition. The default 1.25 MB partition is too small.

The watch is a **Casio GW-BX5600, module 3578**, using the GW-BX5600 MIP protocol. Manual time correction is triggered by pressing **D once from Timekeeping Mode**; pairing/full connection uses a long press of **C**.

## What already works and must be preserved

- JJY/LF transmission, its independent timezone/offset, carrier generation, envelope timing, and recurring RF schedules.
- Bluetooth time delivery to the GW-BX5600. The user reports success on the watch and a correctly changed time.
- Separate Bluetooth timezone/offset, defaulting to Brisbane. Automatic Bluetooth slots use the Bluetooth civil time; LF slots use the station timezone.
- Four repeating automatic Bluetooth times, per-slot persistent switches, and Always Wait listening for the bound watch when RF is idle. Existing saved Off settings remain Off.
- RF priority: BLE scanning/connections stop and the controller must be confirmed off before LF begins. BLE stays off during the entire RF session.
- Saved configuration, watch binding, Wi-Fi credentials, LED setting and UI options across reboots.
- Fixed left sidebar with **Overview, Radio, Watch (BLE), Schedules, Network, Settings, Diagnostics, About**. No fundamental layout changes are authorized.
- Separate reporting of time delivery and font result; Diagnostics explains the limits of independent watch verification.

The recent Wi-Fi AP problem was resolved by the user: **“ok i fixed the wifi issue and its working again.”** They did not identify the exact hardware/upload change. Do not alter the working Wi-Fi/NTP path as part of this fix without evidence. V4.10's checked AP startup and earlier cold-network/SNTP fixes remain.

## Panic investigation: evidence and limits

The user first reported roughly minute-spaced reboots without UI interaction. Continuous Always Wait scanning remains active even with automatic Bluetooth schedules Off. A brief stable interval and one successful Bluetooth sync were followed by another panic, so disabling automatic slots did not prove the defect fixed.

The **13:14:13 restart was an intentional reset/power cycle**, explicitly confirmed by the user; its reset reason 1 is not evidence of an automatic panic.

### Earlier panic, 12:58

```text
Guru Meditation Error: Core 0 panic'ed (InstructionFetchError)
PC: 0x3ffe7658
A0: 0x801282c2
A8: 0x8011f1a9
EXCVADDR: 0x3ffe7658
Backtrace: 0x3ffe7655:0x3ffd8d30 |<-CORRUPTED
ELF file SHA256: 919c068de
```

The instruction address lies in RAM and the backtrace is explicitly corrupted. Do not claim that this trace names a particular C++ function or that every possible memory-corruption cause has been fixed.

### Latest panic, 13:23:40

```text
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
PC: 0x400ee979
A0: 0x800eea05
A2: 0x3ffd6df0
A3: 0xffffffff
A8: 0x00000000
EXCVADDR: 0x0000000c
Backtrace: 0x400ee976:0x3ffd6aa0 0x400eea02:0x3ffd6ac0 0x400eeff9:0x3ffd6ae0 0x400f3ec5:0x3ffd6b20 0x400f400d:0x3ffd6b40 0x400f469a:0x3ffd6bb0 0x400f8b0b:0x3ffd6bd0 0x401014cd:0x3ffd6bf0 0x400ed6df:0x3ffd6c10 0x40096fad:0x3ffd6c30
ELF file SHA256: 919c068de
```

The next boot logged **Reset reason: 4**, confirming a panic reset. This latest backtrace has a strong code-shape/relative-address match to NimBLE scan response handling: `resetWaitingTimer`, `removeWaitingDevice`, and scan completion. It is more useful than the corrupted first trace, but it is still not an exact decode against the user's ELF.

The retained cloud V4.10 ELF is `/workspace/.radioclock-tools/output/RadioClock_V4_10/RadioClock_V4_10.ino.elf`, SHA256 **`d7c3231853c526769add0fffdc648a6f2b8db44a72c53698e0ca356fe85f1295`**, which differs from the user's **`919c068de`**. Do not map the user's absolute addresses to unrelated cloud functions and present them as definitive. If exact decoding is necessary, retain or obtain the ELF from the user's specific build, with matching hash and dependency versions.

### Confirmed source defects and V4.11 approach

V4.10 restarted one-second active scans from `loopTask` when `isScanning()` becomes false. NimBLE can mark scanning inactive before its host task finishes the scan-completion callback and pending advertised-device cleanup. Restarting and clearing results from the other task in that interval can free a device still in use by the host callback.

A native AddressSanitizer fixture using the installed NimBLE scan-completion/start/cleanup bodies reproduced the use-after-free. Additional native reproduction reaches a null pending-list head and a read at offset **0x0c**, consistent with the latest exception address. This confirms defects in the source; exact attribution of all reported crashes still requires device acceptance.

The V4.11 implementation in `RadioBleScanControl.h` sends **all scan start/stop operations through the NimBLE host event queue**, serializing them with scan callbacks. This covers manual retry/pairing, Always Wait, scheduled windows, cancellation and RF shutdown. A false `isScanning()` result alone must never be treated as completion of host cleanup. Timed-out commands retain their event/state until the host acknowledges them; RF remains blocked rather than resetting an event that is still queued.

It also sets **`setScanResponseTimeout(0)`** to avoid the affected independent pending-response timer path. Keep active scanning and its existing one-second scan-completion fallback so watch requests without a separate scan response are still delivered. Do not switch casually to passive scanning or drop the bound-watch filter. Require coordinator quiescence before host deinit, release its event **only after NimBLE has stopped the host**, and initialize fresh event state after reinit.

Temporary native investigation fixtures are `/workspace/panic-diagnostics/reproduce_scan_restart.py` with `scan_restart_asan.log`, and `/workspace/panic-diagnostics/reproduce_scan_null_head.py` with `scan_null_head_asan.log`. Their success means they reproduced the expected failures in the **old baseline**; it is not a passing result for the corrected firmware. The null-head reproduction matches the relevant `l32i` read from a null register plus offset 12 in the latest exception, while the ELF mismatch still limits definitive decoding.

There is a **separate upstream NimBLE 2.5.1 host-timer deinit issue**: <https://github.com/h2zero/NimBLE-Arduino/issues/1184>. The reported upstream correction is not part of the pinned 2.5.1 release or this project fix. Do not claim the scan correction applies that dependency fix or proves every deinit/reinit failure solved. Check the current upstream status before deciding whether it needs a separate future dependency update.

## Font correction included in this version

The user originally deferred this to prioritize the panic, then explicitly requested: **“add the font change to this version too.”** Include it in V4.11; do not leave it deferred based on the earlier request.

The user selected **Apply watch font during BT sync: On** and **Standard (new font)**, yet manual time correction updates the time without changing the font.

Two confirmed defects were identified:

1. The V4.10 helper accepted only **17-byte** basic-settings packets. Real GW-BX5600 captures contain **12-byte** replies and writes. Accept the observed 12- and 17-byte formats, preserve the original packet length and every unrelated byte, and change only **byte 8, bit 0x20** (Classic set; Standard clear).
2. V4.10 ran the optional font transaction **after the final TIME command**. Lower-right-button time-sync sessions can disconnect immediately after TIME is acknowledged. In the user's log, time delivery was reported at **13:18:19.083**, disconnection at **13:18:19.156** (73 ms later, reason 531 = HCI 0x13, remote user terminated connection). No font request/write appeared.

V4.11 keeps the existing working Casio handshake and reads/updates/reads back the optional font **before final TIME**, then calculates the fresh current time and delivers it last. When font changes are Off, preserve the time-only wire sequence. A font error must be reported separately and must not unnecessarily prevent successful time delivery.

Reference implementations and real captures already reviewed:

- <https://github.com/izivkov/gshock-smart-sync-webapp>, commit `5b3ff83dbae43a7a526232e5f35de98425ca7ac8`, reference for font bit and characteristic mapping.
- <https://github.com/izivkov/gshock_api>, commit `ae817310dd9551d5e6e53ee61d89af4e8d954767`, real GW-BX5600 captures.
- Local retained checkouts: `/tmp/gshock-web-v46` and `/tmp/gshock-api-v46`; they are optional research copies, not build dependencies.
- Captures include `btsnoop_hci_bx.log`, `btsnoop_hci-bx2.log`, and `btsnoop_hci_bx3_official_app.log`.
- Actual 12-byte example: Classic `130501000100000020000000`; Standard `130501000100000000000000`.
- The repository's earlier 17-byte mocked fixture alone did not cover the watch's 12-byte reply. New tests must exercise actual packet lengths and post-TIME disconnection behavior.

Readback agreement is stronger than a write acknowledgement, but the physical watch display still needs checking. A manual time-sync session may impose additional settings permissions; do not claim that moved ordering alone proves the device accepted a font change.

## Flash changes: still queued, not part of this release

The user wants **no logs written to flash** and only required settings persisted across reboots. They were mainly concerned about writes while the ESP sits running, rather than explicit configuration changes.

Two changes were approved **for a later version, queued only**:

1. Keep routine Bluetooth sync history/date/protocol completion information in RAM, preserving persistent watch bindings when pairing changes configuration.
2. Remove recurring background retries of failed configuration saves; retry only after an explicit user save/retry, retain the previously saved configuration on failure, and acknowledge success only after a durable write.

These remain unchecked in `docs/NEXT_VERSION.md`. Do not silently include them in unrelated updates. The current V4.10 log's `Config saved atomically` three seconds after a successful sync is evidence of the existing deferred save, not a serial-log file being written.

## Build and verification workflow

Use the existing checkout and retained toolchain. Do not create a worktree unless requested. Keep all active sketch companion headers, including `CasioWatchBattery.h` and `RadioBleScanControl.h`, and `partitions.csv` beside the `.ino` file.

```bash
cd /workspace/time-transmitter
# Only if the retained toolchain is missing:
bash scripts/install-toolchain.sh
# After editing the UI or its version label:
python3 scripts/embed-ui.py
bash scripts/test.sh
node scripts/test-ui-browser.cjs
RADIOCLOCK_FQBN='esp32:esp32:node32s:PartitionScheme=no_ota,FlashFreq=40' bash scripts/compile.sh
```

Tool storage: `/workspace/.radioclock-tools`. Build outputs: `/workspace/.radioclock-tools/output/RadioClock_V4_12`. Browser checks need Playwright/Chromium; use the existing installation and set `RADIOCLOCK_CHROMIUM` if required. The permanent coordinator regression is `tests/test_ble_scan_control.py`; keep it in the normal test runner. The investigation's native fixtures use the pinned installed NimBLE sources; temporary research files are not additional build dependencies.

Before release, retain test/compiler/image evidence, record hashes of tested source, review the scope against the previous release, and inspect the actual rendered page against the approved sidebar mockup. The new version must be consistent in the sketch path, firmware label, UI, scripts and README.

## Cloud validation completed

V4.12: all **31 Python host cases**, UI handler and browser checks, captured BX packet checks and concurrent RF/BLE arbiter stress passed. Battery tests include two identified GW-BX5600 captures, exact packet size/channel, fragmented/overflow/late responses, 1.5-second reply deadline, disconnection/RF cancellation, optional failures preserving time, TIME-success-only commit, replacement/profile isolation and no battery flash persistence. The UI tests distinguish 0% from null, retain date/time and previous readings on read failure, switch profiles and clear battery display on simulated restart.

Node32s/core 3.3.12 compile: **1,411,588 / 2,097,152 bytes (67%)** program storage; **62,272 / 327,680 bytes (19%)** global RAM. Firmware image **DIO / 40 MHz / 4 MB**, checksum/hash valid. Retained cloud ELF SHA256: `8e6461917ce730a9cabc53c0e07dd5532790b4494562d46a50705870f23201a0`.

Evidence and tested-source hashes: `docs/build-evidence/v4.12/`; full review: `docs/V4.12-review.md`. Actual overview/watch screenshots were inspected against the approved mockup; all eight sidebar labels and layout remain. No compiled binaries are published. The release includes a separate Arduino-source ZIP containing the complete active sketch folder.

The latest remote README edits (through commit `da0d31f`) were preserved. They report that the watch font changes now work; this is user-reported device behavior, not a cloud hardware test. Battery hardware acceptance and prolonged scan stability still need device confirmation.

## Device acceptance still needed

No physical ESP32 or Casio watch is attached to the cloud. Ask the user for results where necessary and keep that limit explicit.

- Leave **Always Wait On**, automatic BT slots Off initially, and observe at least 15 minutes, longer than the reported short stable interval. Record any full panic/backtrace and ELF hash.
- Repeat many manual watch time syncs, including cancellation/retry and disconnect cases. Confirm the correct Bluetooth civil time.
- Test font Off, Standard and Classic; verify the actual display and returned font result. Include both a short D-button manual correction and a full C-button connection where practical.
- Re-enable automatic slots and verify listening in the Bluetooth timezone.
- Exercise RF start/stop and Bluetooth power-saving off/on transitions. BLE must remain off during RF; scanning must resume safely afterward.
- Browse Diagnostics and navigate the UI while listening/syncing. Confirm NTP, independent JJY/BT timezones, configuration persistence after a power restart, and the unchanged eight-item sidebar.
- Do not erase flash or remove saved Wi-Fi settings merely to perform these checks.

## Release and communication requirements

- User authorization includes fixing both issues and pushing the next release. No extra generic approval step is needed; necessary device evidence is distinct from authorization.
- Bump the version for each new released change; the current release is **V4.12**. Preserve original author notices, credits and historical version folders.
- Publish **source only**; the user did not request compiled binaries.
- Provide change/fix descriptions and a **direct GitHub tag ZIP link after every release**. Download that ZIP and verify it matches the tested source before reporting success.
- V4.12 is the current release; preserve its verified source and do not claim a successful physical device test before the user reports one.
- The user has been frustrated by unrelated cybersecurity warning messages. Those UI/account messages cannot be controlled by this repository; continue ordinary firmware maintenance and report the concrete outcome without claiming to fix the chat platform.

