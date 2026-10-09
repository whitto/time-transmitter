# Time Transmitter / RadioClock — session handoff

Updated on **10 October 2026**, using the user's **Australia/Brisbane** timezone.

## Read this first

**Latest status:** V4.15 build R1 implements all nine additional findings from the V4.14 R2 senior review, explicitly authorized by the user. The Watch page also shows the last four successful or failed attempts per selected watch in fixed RAM only, and both uptime displays use hours/minutes/seconds. Source and release notes are published at tag `v4.15` (commit `ba3c30ac46cb975dcca909cd17cf7da2b4775d8d`). All 77 unittest cases across 23 groups and complete/additional browser checks passed afterward. Five direct source ZIPs match their tags byte-for-byte. The user requested no firmware compile, so the active ESP32 compile was stopped; no V4.15 board-build/ELF result is claimed. Read `docs/V4.15-review.md` for focused fault evidence and remaining physical limits. The nine items are marked implemented in `docs/NEXT_VERSION.md`; the original R2 report remains historical evidence.

Use active sketch `firmware/RadioClock_V4_15/RadioClock_V4_15.ino` and all three included libraries: NimBLE-Arduino 2.5.1-radioclock.1, RadioCrashDumpGate 1.0.0 and RadioBoundedWebServer 3.3.12-radioclock.1. Normal upgrades retain saved settings. Initial/reacquired NTP trust now needs three consistent replies spanning at least 30 seconds; RF/BT stay off while provisional. Legacy configuration upgrades to a versioned CRC record only on an actual settings/binding save, with no boot/idle migration write. Preserve the eight-item sidebar, RF packet timing and watch protocol/font/battery/LED/history behavior.

Future workflow: fresh reliability review before publication; full changelog and source published first; full suite afterward; numbered revision if failures occur. No physical ESP32/watch is attached to the cloud. MeshCore messages are unrelated and must be ignored.

Read `AGENTS.md`, the root `README.md`, `docs/NEXT_VERSION.md`, `docs/V4.15-review.md` and `docs/LONG_RUN_RELIABILITY_REVIEW.md` before editing. Previous release reviews remain historical evidence. Preserve existing user changes, including README hardware/range details and the user's report that font changes now work.

## Repository and current release

- Repository: <https://github.com/whitto/time-transmitter>.
- Cloud checkout: `/workspace/time-transmitter`, branch `main`.
- Source release: **V4.15 build R1**, tag `v4.15`; resolve its published commit with `git rev-parse v4.15^{commit}`. Historical tags remain unchanged.
- Previous V4.11 release commit: `9edbdc2462b76757cabba9ca689e62689c6e93b8`.
- V4.15 release: <https://github.com/whitto/time-transmitter/releases/tag/v4.15>.
- V4.15 source ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/v4.15.zip>.
- Arduino sketch and all three libraries: <https://github.com/whitto/time-transmitter/archive/refs/tags/arduino-v4.15.zip>.
- RadioCrashDumpGate library ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/crash-gate-v1.0.0.zip>.
- Download this handoff: <https://raw.githubusercontent.com/whitto/time-transmitter/main/docs/SESSION_HANDOFF.md>.
- Previous V4.10 release commit: **`40f309b0ddfac04095ac58d6c821d9df4b26e67f`**.
- V4.10 release: <https://github.com/whitto/time-transmitter/releases/tag/v4.10>.
- V4.10 source ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/v4.10.zip>.
- Current active source: `firmware/RadioClock_V4_15/RadioClock_V4_15.ino`; previous version folders/reviews are retained unchanged.
- Keep the complete 15-file sketch folder (twelve headers) and all three source libraries: `libraries/RadioCrashDumpGate`, the patched `libraries/NimBLE-Arduino`, and `libraries/RadioBoundedWebServer`. Keep the five crash-gate files including its linker metadata/marker. Replace plain/duplicate NimBLE 2.5.1 with **2.5.1-radioclock.1**; the firmware rejects an unpatched build. The Arduino archive includes all four complete folders and no compiled binaries. Patched-library ZIP: <https://github.com/whitto/time-transmitter/archive/refs/tags/nimble-v2.5.1-radio-r2.zip>. Check `git status` and actual diffs before editing; later work may have progressed.
- UI source: `ui/radioclock.html`; approved layout reference: `ui/v32_ui_mockup.png`.
- The previous V4.11 ZIP was downloaded and verified against all 162 tracked release files. Repeat that verification for each published version, including the Arduino-source-only ZIP.

## V4.14 crash-dump feature and reliability review

**Settings → Crash dumps → Save crash dumps to flash** defaults Off and saves on toggle change. Off blocks new SDK dumps; On permits a future panic dump in the restored 64 KB core-dump partition. Off does not delete an existing dump and On does not create one immediately. Enabled dumps have no once-per-day quota; they are independent of daily BT status snapshots. Serial panic/backtrace output remains available. The gate is Off during early startup before saved configuration is loaded; unsupported compiled flash backends show Unavailable.

Install **RadioCrashDumpGate 1.0.0** into the Arduino sketchbook's `libraries` folder, or install its library-only ZIP using **Sketch → Include Library → Add .ZIP Library**. Keep all five files including its textual `src/esp32/README.md` marker. Arduino's mixed-library metadata supplies `--wrap=esp_core_dump_write` while compiling source normally; no `.a` or `.so` is shipped. A retained `__real_` reference makes missing linker metadata fail the build. Cloud scripts install the included helper. Classic ESP32 is validated; other MCU targets require their own linker metadata and call-path proof. Recheck actual release ELF dispatch and IRAM/DRAM placement after toolchain changes.

The user approved all original review recommendations for R2. `docs/LONG_RUN_RELIABILITY_REVIEW.md` now maps findings to fixes and tests; `docs/V4.14-R2-review.md` records release validation. Required fixes are not left pending approval. Retain the earlier R1 reproductions as historical evidence rather than treating their intentionally unsafe snapshots as current passing tests. No confirmed growing application RAM leak was demonstrated, but physical soak and SDK/watch behavior remain unproven.

R2 uses bounded/readback-checked saves, no recurring failed-config retries, bounded boot parsing and non-destructive filesystem recovery. A mount-fault AP provides a specifically confirmed settings erase/restart; mounted save faults only permit explicit retry. Router outages retain AP access while station retries; independent NTP wakes follow trust deadlines. RF uses cancelled command generations/pause ownership, checked timer progress and RAM-only monitoring. Unrecoverable stalls can force RF off and trigger a controlled restart; there are no periodic maintenance reboots.

## Approved V4.13 scope and behavior

The user required asking before changing working functions and then **approved these specific changes**: LED handling, BT result/history saving, configuration/API handlers, Wi-Fi power management and UI rendering. Keep BT packet protocols, LF generation/timing and working Wi-Fi/AP/NTP connection helpers unchanged. No additional generic approval is needed within this scope. Ask before unrelated fixes or fundamental layout changes.

The blue GPIO2 LED flashes only during an actual BT connection/time-sync transaction. With the master LED setting On, successful TIME delivery leaves it solid for 24 hours from the UTC success timestamp; an actual failed BT transaction clears it. Waiting for a watch, a no-watch window or RF-deferred work does not clear a previous success. Optional font/battery failure does not negate successful TIME delivery. LF transmission does not flash it, but a solid success indication may continue during RF. Existing master Off choices remain Off.

**Save BT sync status to flash** is a saved Watch-card toggle, default On. The user explicitly accepted saving the **first successful sync each Brisbane calendar day immediately**, keeping later events in RAM, and restoring that first snapshot after reboot. A later failure can therefore be forgotten after reboot. The snapshot stores only UTC success timestamps, watch/profile address and protocol identity, saved-day quota/validation metadata, and four diagnostic counters: connection attempts, acknowledged writes, notifications and response errors. There is no logfile, serial-output storage, font result, battery reading or verbose error text. Profile completion is restored only for the matching binding. Routine syncs no longer save the config file merely to record history; changed bindings still save as required configuration. Frozen legacy fields prevent current RAM history leaking into unrelated settings writes.

History Off keeps current status in RAM but disables history writes/restoration across reboot. It invalidates the old saved snapshot for future re-enabling. Valid saved-day metadata still enforces the daily quota across reboot and toggle changes. The quota means one successful snapshot commit per Brisbane day; LittleFS can perform internal metadata operations for a commit. A failed save does not retry during the same uptime/day. Battery readings, font results and later diagnostic events remain RAM only. No history save occurs while idle or on a failed TIME transaction.

**Settings → Daily Wi-Fi access** saves a recurring start/end/timezone group, default disabled with 18:00–20:00 Australia/Brisbane selected. Enabling automatically selects Power-save. Start is inclusive, end exclusive; overnight windows are valid, equal times invalid. Disabling leaves the power mode unchanged; Network Always on overrides the window. The user approved retaining setup AP, five-minute startup/recovery access and existing short NTP wakeups outside the window. This timezone is independent of both LF and BT timezones.

Overview Wi-Fi is green for a station connection, red for setup AP (AP-plus-station uses red precedence). Radio is green while LF is active and red while idle. Keep all eight sidebar pages and compare rendered screenshots against `ui/v32_ui_mockup.png` before publication.

## V4.12 battery feature

The identified GW-BX5600 uses GET **0x28** on `26eb002c` with **Write Without Response**, replying on TIME/all-features `26eb002d`. Observed replies are exactly nine bytes. Byte 1 is the calibrated raw battery level: **14–24 maps to 0–100% in 10% steps**, clamped at either end. Actual BX captures are `281418000000020000` (60%, D-button session) and `281821000000020000` (100%, official app). The standard Battery Service is not required or assumed.

The optional read runs before SP step 1 and final TIME; its reply wait is limited to 1.5 seconds including the 300 ms notification assembly gap. Discovery/subscription remain existing guarded GATT operations. A battery-only failure does not block time delivery. Readings commit only after successful TIME, after a replacement binding is applied. They are per profile, tied to the address and validated protocol, and kept in **RAM only**. A failed read retains the same watch’s last known sample with a failure status; a replacement cannot inherit that sample. Reboot clears readings.

Each `bt_profiles` API object adds nullable `battery_percent`, `battery_read_at` in Bluetooth civil time, and `battery_status`. The Watch page and top Watch tile follow the selected profile. A real 0% remains distinct from unavailable. Other model protocols show unavailable rather than using the wrong calibration.

Source evidence and build/test results are in `docs/V4.12-review.md`. Test on the device by pressing D once from Timekeeping with RF idle; check the `BT: Watch N battery estimate …` serial line, the two UI displays, reading time, time delivery, font behavior, replacement/profile isolation and restart clearing.

## Hardware and dependencies

The user uses a classic **Node32 / Node32s ESP32**, Arduino IDE **2.3.4**, a connected transmitter coil, **40 MHz flash**, and **No OTA (Large APP)**. They previously had **Erase All Flash Before Sketch Upload** enabled. Their exact flash-size/mode settings should be taken from the current build or confirmed if needed; do not recommend unnecessary erases.

The user confirmed **Arduino-ESP32 3.3.12** and originally **NimBLE-Arduino 2.5.1**. R2 requires the included patched **2.5.1-radioclock.1** source fork, with the upstream host-timer fix and checked initialization allocations. The cloud toolchain also pins **ArduinoJson 6.21.5** and Arduino CLI **1.3.1**. The target is 4 MB flash, with the supplied sketch-local `partitions.csv` defining a 3 MB application partition. The default 1.25 MB partition is too small.

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

The recent Wi-Fi AP problem was resolved by the user: **“ok i fixed the wifi issue and its working again.”** They did not identify the exact hardware/upload change. V4.13's approved change adds a daily access condition to the existing power manager; keep working Wi-Fi/AP/NTP connection helpers intact. V4.10's checked AP startup and earlier cold-network/SNTP fixes remain.

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

The **separate upstream NimBLE host-timer deinit issue** (<https://github.com/h2zero/NimBLE-Arduino/issues/1184>) was absent from the ordinary 2.5.1 release and the V4.11 scan correction. R2 now includes its upstream correction in the verified 2.5.1-radioclock.1 source fork, separately from the scan coordinator. This establishes the implemented dependency correction and regressions, not definitive attribution of every historical panic or physical shutdown/restart acceptance.

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

## Flash policy and completed retry correction

The user wants **no logs written to flash**. Only required changed configuration/schedules and the optional minimal first-success-per-Brisbane-day BT snapshot are normally persisted. Later BT outcomes, battery/font results, faults, heartbeats and headroom remain RAM/serial. Enabled crash dumps are a separate explicit exception with no daily quota.

The earlier per-sync history/config writes were replaced in V4.13. R2 completes the queued failed-config retry correction: retain the previous saved file, latch a RAM fault, stop background writes and retry only following an explicit successful save. Config/schedule serialization and readback failures cannot acknowledge durability.

LittleFS mounts with automatic formatting disabled. Mount failures keep RF off and setup AP accessible; destructive reset requires the exact explicit confirmation and an acknowledged RF pause through the mount-fault AP. Corrupt boot config is bounded and preserved with safe defaults/AP; a mounted filesystem is not eligible for automatic/destructive fault reset. Normal uploads need not erase flash.

The optional 64 KB core-dump partition restored in R1 remains; app/LittleFS addresses and sizes are unchanged. RadioCrashDumpGate starts Off and permits SDK writing only when the saved toggle enables it. Disabling retains old dumps, and serial panic/backtrace output remains available.

## Build and verification workflow

Use the existing checkout and retained toolchain. Do not create a worktree unless requested. Keep all twelve active sketch companion headers and `partitions.csv` beside the `.ino`, and install the included RadioCrashDumpGate, patched NimBLE and RadioBoundedWebServer source libraries. Cloud compile scripts install it automatically.

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

Tool storage: `/workspace/.radioclock-tools`. Build outputs: `/workspace/.radioclock-tools/output/RadioClock_V4_15`. Browser checks need Playwright/Chromium; use the existing installation and set `RADIOCLOCK_CHROMIUM` if required. Keep existing scan/history/Wi-Fi-window regressions and V4.14's `tests/test_crash_dump_gate.py` in the normal runner. R1 reproduction sources under `docs/build-evidence/v4.14` preserve the old failures. Current fault regressions live in `tests/`; final R2 evidence belongs under `docs/build-evidence/v4.14-r2`. Run the reliability review before publication and the additionally requested full review afterward.

Before release, retain test/compiler/image evidence, record hashes of tested source, review the scope against the previous release, and inspect the actual rendered page against the approved sidebar mockup. The new version must be consistent in the sketch path, firmware label, UI, scripts and README.

## Cloud validation

R2 final suite counts, compile sizes, image/ELF proof, screenshots and independent review are recorded in `docs/V4.14-R2-review.md` and `docs/build-evidence/v4.14-r2/` before publication. Six storage reliability cases include actual saves/boot faults and sanitizers; radio, network, pinned-library, JSON and lifecycle cases cover the other fixes. Run the full final suite after all shared edits; do not substitute these component checks for the release result.

Historical V4.14 R1: **42 Python host cases**, UI handlers/browser checks, native BX packet checks and existing RF/BLE stress passed. Gate cases cover Off/On/unsupported dispatch, sanitizer checks and missing-linker rejection; the actual config test covers legacy Off/save/reboot/both directions under three storage failures. Node32s compile passed at **1,429,192 / 2,097,152 bytes (68%)** program storage and **62,536 / 327,680 bytes (19%)** global RAM. The sketch-local table retains the existing 3 MB app and 896 KB filesystem addresses/sizes with the restored 64 KB core-dump slot. Settings desktop/phone renderings were inspected against the mockup; the new card defaults Off and all eight sidebar items remain.

Actual main-ELF verification passed: SDK panic dispatch enters the IRAM wrapper/literal pool, reads the aligned DRAM word inline, returns with Off and calls only the real SDK writer with On. `scripts/validate-crash-gate-elf.py` is a mandatory post-compile check. Retained ELF SHA256: `54b81b3d961e538b588bb5a67e948731369359704b9a2259311e58ca3f9a1fa3`. Image checksum/hash are valid. Scope audit found 23 working functions and all eight companion headers byte-identical to V4.13. Evidence is retained under `docs/build-evidence/v4.14`; published archive comparison remains part of the release procedure.

Previous V4.13 baseline:

V4.13 passed all **37 Python host cases**, UI handler/browser checks, native BX capture/malformed-packet checks and 10,000-session concurrent RF/BLE arbiter stress without modeled overlap. Compact history codec and actual history integration fixtures passed AddressSanitizer/UndefinedBehaviorSanitizer. Tests cover BT-only LED behavior and 24-hour limits, history/quota/Off/On/reboot/corruption/failure handling, independent Wi-Fi window/DST conversion, existing access exceptions and durable grouped settings. Independent read-only review found no blocking issue and confirmed unchanged BT packet, RF and Wi-Fi/AP/NTP connection helpers.

Node32s/core 3.3.12 compile: **1,426,872 / 2,097,152 bytes (68%)** program storage; **62,536 / 327,680 bytes (19%)** global RAM, leaving 265,144 bytes. The CLI reports board No OTA capacity; the sketch-local partition table retains the existing 3 MB application layout. Root inspected actual dark/light desktop and phone screenshots against the mockup; all eight sidebar routes remain. See `docs/V4.13-review.md`. Published archive comparison remains part of the release procedure.

Previous V4.12 baseline:

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
- Confirm the blue LED flashes only during actual BT transactions, stays solid after success through idle/RF, clears after an actual failure, respects Off, and expires at 24 hours.
- With daily history On, confirm first-success save, later RAM-only events, first-snapshot restore after reboot, matching-binding completion data and retained day quota across reboot/Off/On. Confirm history Off and battery/font data remain RAM only.
- Test daily Wi-Fi access with a short ordinary window and overnight times. Confirm independent timezone, inclusive start/exclusive end, Always-on override and setup/startup/recovery/NTP exceptions.
- Verify crash-dump default Off, saved toggle across reboot, unsupported state where applicable and retained serial diagnostics. On a spare/test device, check controlled panic storage with Off/On using the exact release ELF; Off must leave an existing dump intact.
- Do not erase flash or remove saved Wi-Fi settings merely to perform these checks.

## Release and communication requirements

- User authorization includes all suggested reliability corrections and pushing the R2 source release. No extra generic approval step is needed; necessary device evidence is distinct from authorization.
- Bump the version for each new released change; this source release is **V4.14 build R2** with new immutable R2 tags. Preserve original author notices, credits and historical version folders.
- Publish **source only**; the user did not request compiled binaries.
- Provide change/fix descriptions and a **direct GitHub tag ZIP link after every release**. Download that ZIP and verify it matches the tested source before reporting success.
- Perform a fresh reliability review after every code/feature change and before publication; complete the separately requested additional full review after finishing R2. Preserve V4.12's verified source and do not claim a successful physical device test before the user reports one.
- The user has been frustrated by unrelated cybersecurity warning messages. Those UI/account messages cannot be controlled by this repository; continue ordinary firmware maintenance and report the concrete outcome without claiming to fix the chat platform.

