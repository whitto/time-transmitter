# RadioClock V3.2.2 Codex handoff

## Goal
Continue the ESP32 RadioClock project from the included V3.2.1 baseline and V3.2.2 work-in-progress. Produce a reviewed, compile-tested V3.2.2 for a classic ESP32 Node32 using Arduino-ESP32 3.3.11 and NimBLE-Arduino 2.5.1 or newer.

## Important package state
- `baseline/RadioClock_V3_2_1_Casio_BLE_Reliability.ino` is the last packaged V3.2.1 baseline.
- `firmware/RadioClock_V3_2_2_Casio_BLE_Reliability.ino` contains partial V3.2.2 backend work and MUST be treated as WIP/unverified.
- `ui/v322_ui_latest_WIP.html` is newer than the WIP `.ino`; it has NOT necessarily been embedded into the firmware yet.
- `diff/V3_2_1_to_V3_2_2_WIP.diff` shows current firmware changes.
- Do not assume the V3.2.2 WIP compiles. Compile and fix it before flashing.

## Hardware/software
- Board: classic ESP32 Node32 / ESP32 Dev Module class board.
- Arduino-ESP32 core: 3.3.11.
- NimBLE-Arduino: 2.5.1 or newer.
- Sketch-local `partitions.csv` is included for a 4 MB flash / large application layout.
- LF time transmitter supports JJY plus other formats. RF timing integrity has priority over UI responsiveness.

## Required V3.2.2 behavior

### 1. Manual Sync busy/retry behavior
The old `/api/bluetooth-sync` returned `Bluetooth is already busy` when any of `btBleBusy`, `btWindowActive`, or `btManualSyncRequested` was set.

Fix this so manual sync is restartable/idempotent:
- A second manual Sync should safely restart/cancel-and-restart the manual wait window when appropriate.
- Do not interfere with an actual pairing transaction or an RF transmission.
- Expose specific states such as `Waiting for watch`, `Scanning`, `Connecting`, `Syncing`, `Automatic sync window active`, and `Pairing` instead of a generic busy state.
- If a manual attempt fails and the scanner resumes, another manual Sync should be able to retry cleanly.

### 2. Explicit Pair Watch
Retain the V3.2.1 Pair Watch action in Settings.
- Pairing is separate from Sync Now.
- During pairing, temporarily ignore the old learned BLE address so a replacement watch can be found.
- Do NOT erase the existing binding before success.
- Only commit the newly discovered watch address/name after a successful BLE connection and time write.
- A failed pairing attempt must leave the prior binding intact.

### 3. Bluetooth help card
Expand the existing `How manual sync works` card to show:
- Paired watch name/address/profile if one exists.
- Clear `No watch paired` state otherwise.
- Whether `Always wait for watch sync requests` is enabled.
- Pairing instructions.
- Manual sync instructions.

GW-BX5600 / module 3578 user workflow to display:
- Pair/connect mode: from the watch, hold C for at least 3 seconds until the Bluetooth symbol and `CONNECT WITH A PHONE` flash.
- Manual time correction: from Timekeeping Mode press D once to request immediate time correction.

### 4. Always Wait mode
Add a persistent option: `Always wait for watch sync requests`.
- When enabled and RF is idle, continuously scan/listen for the already-paired selected watch.
- Never silently learn or bind an unknown watch in Always Wait mode.
- Pairing remains explicit.
- After a completed or failed sync, return to listening while Always Wait remains enabled.
- Always Wait must not create a false `Bluetooth busy` condition for ordinary UI actions.

### 5. HARD RF / BLE mutual exclusion
Bluetooth and the LF transmitter must NEVER overlap. RF has priority.

Before any scheduled JJY/WWVB/DCF77/MSF/BPC/etc transmission begins:
1. stop BLE scan,
2. safely terminate/defer any BLE connection,
3. fully stop/deinitialize the BLE controller when required,
4. verify Bluetooth is off,
5. only then allow the LF carrier to start.

While RF is active:
- BLE remains disabled.

After RF ends:
- reinitialize BLE,
- restore Always Wait/manual/scheduled BLE behavior as appropriate.

If BLE shutdown fails:
- keep RF OFF rather than allow overlap,
- report a clear diagnostic error.

The implementation may delay the first RF edge while waiting for confirmed BLE shutdown; never run both simultaneously.

### 6. Fix observed Guru Meditation crash
See `OBSERVED_CRASH.txt`.

Likely lifecycle defect:
- Existing code can call disconnect and delete/free the NimBLEClient too aggressively during/near disconnect/GATT callback processing.
- Do not delete or invalidate the client from inside or immediately adjacent to a disconnect callback.
- Prefer deferred cleanup and/or safely retaining/reusing the client object.
- No missing characteristic, failed connect, timeout, or disconnect may crash the ESP32.

### 7. Correct GW-BX5600 GATT write capabilities
Known UUIDs:
- SP_REQUEST: `26eb002e-b012-49a8-b1f8-394fb2032b0f`
- SP_DATA: `26eb002f-b012-49a8-b1f8-394fb2032b0f`
- SET/TIME: `26eb002d-b012-49a8-b1f8-394fb2032b0f`

Important behavior:
- SP_REQUEST uses Write Without Response.
- SP_DATA uses Write With Response.
- Do not reject SP_REQUEST simply because `canWrite()` is false if `canWriteNoResponse()` is true.
- Do not use one write helper that forces `response=true` for every characteristic.
- Use the correct NimBLE write mode per characteristic.

Improve diagnostics to report each characteristic independently, e.g.:
```
BT: SP_REQUEST found [write-no-response]
BT: SP_DATA found [write-response]
BT: TIME found [write-response]
BT: protocol characteristics OK
```
If a required characteristic/property is missing, say exactly which one.

### 8. Wi-Fi control
Add a Settings control labelled clearly like:
`Disable Wi-Fi sleep / Keep Wi-Fi always on`

Map this to the existing Wi-Fi power mode only:
- enabled => `WIFI_POWER_ALWAYS_ON`
- disabled => scheduled/power-save mode

Do not create a second independent Wi-Fi power flag.

### 9. Preserve existing strengths
Do not regress:
- hardware-timed LF carrier generation,
- wall-clock/second-boundary alignment,
- RF scheduler,
- NTP confidence/holdover logic,
- schedule validation,
- gzip-compressed embedded web UI,
- large-app partition setup,
- NimBLE footprint savings.

## Required senior review before release
- Compile with Arduino-ESP32 3.3.11 if possible.
- Verify NimBLE-Arduino 2.5.1+ API usage.
- Audit every NimBLE callback/client lifetime path.
- Check task/core/thread races.
- Prove RF/BLE mutual exclusion in all entry/exit/error paths.
- Ensure BLE cannot be reinitialized while RF is active.
- Ensure scheduled RF cannot start while BLE is active.
- Verify manual retry, pairing, scheduled sync, Always Wait, and RF preemption.
- Verify all API routes used by `v322_ui_latest_WIP.html` exist.
- Embed the final UI as gzip only after browser/API validation.
- Validate decompression round-trip and JavaScript syntax.
- Update filename, top-of-file filename comment, UI version text, and `FIRMWARE_VERSION` consistently to V3.2.2.
- Report flash/RAM usage from a real compile if available.

## Do not do
- Do not weaken RF/BLE mutual exclusion to improve sync convenience.
- Do not silently pair unknown watches in Always Wait mode.
- Do not erase a good watch binding before replacement pairing succeeds.
- Do not claim compile/runtime validation unless actually performed.
