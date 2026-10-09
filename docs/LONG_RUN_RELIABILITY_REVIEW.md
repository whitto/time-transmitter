# Long-run embedded reliability review — V4.14

Reviewed on **9 October 2026** against `firmware/RadioClock_V4_14/RadioClock_V4_14.ino`, its helpers and the pinned ESP32/NimBLE/ArduinoJson dependencies. This is a read-only review: the new crash-dump option is the release feature, and the findings below are **not fixed in V4.14**. Changes to existing working functions require the user's prior approval.

No steadily growing application memory leak was demonstrated. Fixed buffers, one retained BLE client and bounded schedules/profiles are useful safeguards, but they do not establish years of unattended reliability. The review found concrete recovery/data-loss paths and a separate dependency shutdown defect that should be addressed before relying on unattended operation.

## Priority findings with evidence

### P1 — schedule saving can replace valid data with an empty array under low memory

[`saveSchedules()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4106) allocates an 8 KB ArduinoJson document without checking capacity, overflow or the resulting number of array entries. Its serialized byte-count check can still succeed when failed allocation produces `[]`.

A fixture extracted the actual function and used the pinned ArduinoJson library with an allocator forced to return `nullptr`. It replaced a previously valid schedule file with `[]`, returned success and queued a configuration save. This demonstrates the failure path under injected allocation failure; it does not show that the user's running device has hit it. See the retained [schedule allocation-failure reproduction](build-evidence/v4.14/schedule-oom-reproduction.cpp).

Recommended approved follow-up: validate allocation/capacity, overflow and serialized entry count **before touching/replacing storage**; retain the old file and return an explicit failure. Include allocation/partial-document cases in regressions.

### P1 — pinned NimBLE can invalidate a host timer before queued events finish

Both [`shutdownBluetoothForRadio()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L2638) and [`shutdownIdleBluetooth()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L3463) call `NimBLEDevice::deinit(false)`. Pinned NimBLE-Arduino 2.5.1 contains `ble_npl_callout_deinit(&ble_hs_timer)` in its host-stop path (`ble_hs.c:479`). If that timer event is already queued, early deinitialization can clear the callback before the host dispatches it.

This is the separate upstream [issue 1184](https://github.com/h2zero/NimBLE-Arduino/issues/1184), not the scan-cleanup race corrected in V4.11. Upstream removed the premature deinitialization in [`e0c8f5a558893197ae60c3606ded01a2dbb88863`](https://github.com/h2zero/NimBLE-Arduino/commit/e0c8f5a558893197ae60c3606ded01a2dbb88863); final cleanup already occurs after host shutdown. The installed 2.5.1 source still has the affected code. This establishes a dependency defect, not that it caused any particular historical user panic.

Recommended approved follow-up: pin a reproducible upstream correction or an appropriate released dependency, then validate repeated real controller deinit/reinit, RF handoff and cancellation. Preserve checked RF/BLE exclusion; disabling RF-priority shutdown would change required behavior.

### P1 — router recovery does not automatically recover the transmitter after AP fallback

[`checkWiFiConnection()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4354) switches to setup AP after a 30-second connection timeout. Once `ap_mode` is set and no explicit credential attempt is pending, that function immediately returns. [`startAPMode()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4404) stops SNTP, pauses RF and selects AP-only Wi-Fi.

The source therefore has no automatic retry of saved station credentials after this fallback. Restoring the router alone does not restore normal Wi-Fi/NTP/RF operation; setup access remains available for user recovery. This is a confirmed recovery policy gap, not a failure of the working AP-start helper.

Recommended approved follow-up: retain setup access while retrying stored station credentials with bounded backoff, then resume SNTP/RF only after successful recovery. Test router outages longer than the timeout and repeated loss/recovery without user interaction.

### P1 — power-save NTP windows do not guarantee continued clock trust

[`shouldWifiBeOnForSchedule()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4245) gives periodic NTP access only to full-time TX or an empty LF schedule list. Ordinary schedules wake Wi-Fi before their start times. With the default `{0,1440}` all-day LF schedule, BT slots Off and no independent access window, the available pre-start NTP period is only **23:50–00:00 daily**.

[`clockConfidence()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L491) rejects holdover beyond six hours or earlier if estimated error exceeds 0.90 seconds; [`applyCurrentSchedule()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4131) then stops RF. The fixed six-hour wake used in full-time mode also ignores a shorter adaptive interval: at the initial 50 ppm bound, trust expires after about **3 hours 53 minutes**. The extracted-function [NTP power-policy reproduction](build-evidence/v4.14/ntp-power-policy-reproduction.cpp) confirmed ten wake minutes per day for the all-day schedule and trust expiry before the next wake. Other BT/access windows can mask this policy gap.

Recommended approved follow-up: add a bounded, trust-driven NTP wake independent of user/LF/BT schedules, early enough for the adaptive interval/error budget. Preserve intentional RF shutdown when NTP remains unavailable and retain bounded connection/reply deadlines and retry backoff.

### P1 — a timed-out pause can leave RF disabled after a rejected request

[`radioSetPaused()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L438) queues a pause and waits two seconds. A timeout returns false without cancelling the queued command. Schedule/timezone handlers then return HTTP 503 without arranging a matching resume. If the radio task later processes that delayed pause, RF remains paused until another operation resumes it.

The [late-pause reproduction](build-evidence/v4.14/late-pause-reproduction.cpp) used the actual pause function and command-processing block with injected delay: timeout, rejected caller, late pause and continued paused state. This demonstrates a failure path, not an observed device incident.

Recommended approved follow-up: use command generations/ownership and a bounded cancel/resume policy, preserving intentional AP pauses. Check resume acknowledgements. The NTP configurator already recognizes late pause ownership; general configuration handlers need equivalent handling.

### P1 — timer allocation failure does not prevent reported RF startup

[`starttimer()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L1133) returns after failed `timerBegin()` allocation, but setup still requests radio refresh and reports startup. One notification can initialize the carrier while no timer remains to advance its envelope; the radio task can then wait indefinitely for notifications. The source failure path is confirmed; it was not reproduced on physical hardware.

Recommended approved follow-up: return/check timer startup success, keep RF positively off on failure, expose a latched RAM-only fault and allow bounded retry. Monitor timing progress independently so a stopped timer/task fails safely.

### P2 — failed configuration writes retry indefinitely every three seconds

The [`loop()` retry branch](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L1115) calls [`writeConfigNow()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4000) whenever `configDirty` remains set beyond the three-second debounce. Storage failures set it again. A persistent short-write/rename/storage fault can repeatedly reopen/rewrite the temporary file. This is the previously queued flash-wear issue; it does not affect the separate daily BT history writer's no-automatic-retry policy.

Recommended approved follow-up: stop repeated background retries, keep a RAM-only failure state, retain the previous saved configuration and retry following explicit user save/retry. Do not acknowledge a failed save as durable.

### P2 — config serialization has unchecked allocation paths

[`writeConfigNow()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4000) ignores its `String::reserve(700)` result and subsequent append results. ESP32 String growth failure can leave an incomplete blob; the file byte-count comparison then compares against that already incomplete value. Atomic rename protects against partial file writes, but not against an incomplete in-memory serialization.

This is a confirmed missing check with a plausible corruption path, rather than a reproduced complete configuration-loss event. Recommended approved follow-up: checked bounded serialization, complete-format validation and readback before replacement, including injected allocation failure.

### P2 — failed filesystem mounting may format required saved settings

[`initFilesystem()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L3590) calls `LittleFS.begin(true)`, permitting automatic formatting after a mount failure. This can erase credentials/configuration/history after a filesystem fault or incompatible/corrupt storage. The code policy is confirmed; a spontaneous filesystem fault was not reproduced in this review.

Recommended approved follow-up: enter an accessible fault/recovery state without automatically formatting, and make destructive formatting an explicit user reset action. Preserve the user's settings and avoid unnecessary erase-on-upload advice.

## Memory and responsiveness gaps

The [`status API`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L5169) reserves 512 bytes but builds a much larger response using many temporary Strings and unchecked allocations. [`jsonQuoted()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4506) allocates a JSON document per quoted field. The UI requests status every second and, roughly every five seconds, requests diagnostics, status, config and schedules even on other pages. These temporary allocations are released; allocation churn is **not evidence of a memory leak**. It does increase sensitivity to low heap/fragmentation and can yield malformed responses when unchecked growth fails.

Recommended approved follow-up: bounded/pre-sized checked serialization, lighter common status data and on-demand/slower diagnostic refresh. Keep useful overview data without issuing the full diagnostic bundle on every page.

[`ntpstart()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L2192) performs bounded connection/NTP waits on `loopTask`, totalling up to approximately 30 seconds. During scheduled Wi-Fi wakeup, HTTP servicing and BT watch processing wait; independent RF/LED tasks continue. A nonblocking network state machine would improve responsiveness while preserving the tested connection/AP/NTP helpers.

[`writeBt()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L2866) waits for ATT completion with `BLE_NPL_TIME_FOREVER`. Normal NimBLE procedures have a 30-second timeout and existing cancellation/preemption guards, but there is no application-wide BT transaction deadline. A future deadline should request disconnect and allow callbacks to unwind safely; returning early while stack-local `NimBLETaskData` remains referenced would introduce a lifetime defect.

BLE scanner creation also relies on dynamic allocation before an application-level low-memory admission check. Safe handling must account for the dependency's C++ allocation behavior; adding a null check alone may not prevent allocation abort.

## Long-uptime and progress monitoring

The existing boot-access comparison in [`updateWifiPowerManagement()`](../firmware/RadioClock_V4_14/RadioClock_V4_14.ino#L4337) is evaluated throughout uptime. After each complete 32-bit `millis()` cycle, about **49.71 days**, it reopens the five-minute boot-only access window. Diagnostics also reports `millis()/1000`, so displayed uptime returns to zero at that boundary. The [32-bit rollover reproduction](build-evidence/v4.14/rollover-reproduction.cpp) confirmed the access-window behavior; neither finding itself implies a reboot. Recommended follow-up: latch startup access as completed and use 64-bit monotonic uptime.

The pinned SDK task watchdog checks CPU0 idle with a five-second timeout; CPU1 idle is not checked, and application loop/radio tasks are not registered. A blocked loop or stopped radio notification stream can therefore leave a powered device without detecting lost application progress. `yield()` alone does not reliably allow lower-priority CPU1 idle to run. Recommended follow-up: add a small blocking idle interval and RAM-only task heartbeat/headroom monitoring, then introduce a progress watchdog after accommodating bounded long operations. **Simply adding a five-second loop watchdog now would cause resets**, because existing `ntpstart()` can legitimately take approximately 30 seconds.

Clock error confidence also averages signed ppm samples before taking the absolute value. Opposite-signed drift from temperature changes or NTP jitter can cancel that average towards zero while a recent drift magnitude remains substantial. This is an estimation risk, not a reproduced stability fault. Recommended follow-up: conservative absolute drift/uncertainty bounds, implausible-correction rejection and sign-reversal/temperature tests, with measurements kept in RAM.

## Boundedness and evidence that passed

- One retained BLE client, guarded reuse and refreshed service caches; no client destruction in disconnect callbacks.
- Static callback/event objects and fixed 512-byte notification assembly with overflow rejection.
- Scan results configured with `setMaxResults(0)` and cleared at each one-second scan completion; no accumulating results collection was found.
- Fixed storage limits of 24 LF schedules and four watch profiles; replaced status strings rather than an append-only application event log.
- JSON/request temporaries are released, routes register once and Wi-Fi credential persistence through the SDK is disabled.
- Diagnostics already rejects document overflow/output reservation failure.
- `tests/test_ble_scan_control.py`: nine native cases passed, including 100 modeled host restart cycles and ASan/UBSan with leak detection. This tests the production scan coordinator header, not the entire SDK or physical ESP32.
- Existing clock/timezone/encoder checks passed five cases, and the actual RF scheduler/handoff check passed one case. Timezone fixtures include leap years, DST and year 2100. Ordinary short elapsed/deadline checks generally use wrap-safe subtraction.
- RF patterns, schedule arrays and pending serial snapshots are bounded. The 64-bit monotonic clock avoids ordinary multi-year timer overflow; the processed-second counter would take about 136 years to wrap at one increment per second.

## Soak testing before unattended deployment

Use a physical Node32s/GW-BX5600 and a reproducibly pinned build, initially for several days and then weeks. Capture measurements in RAM/serial or externally; do not add periodic flash logs. Sample free heap, minimum free heap, largest allocatable block and task stack high-water marks at matching points before/after BT sync, RF and Wi-Fi transitions. The current UI exposes loop-task headroom, but radio, LED and NimBLE-host stack minima also need measurement before claiming adequate margins.

Repeat manual/scheduled BT time syncs, optional battery/font work, disconnect/cancel/retry, controller shutdown/restart and RF handoff. Exercise router loss/recovery, unavailable NTP, Wi-Fi power windows, all-day/long RF schedules, date/DST changes and millis rollover. During separate fault tests, inject allocation and storage failures and verify retained configuration/schedules. Confirm the daily BT write quota and that crash storage stays Off unless explicitly enabled.

A stable free-heap value after equivalent cycles and a stable largest-block size help distinguish retained allocations from normal churn. Low stack margins, steadily shrinking largest blocks, resets, dropped network recovery or silent schedule loss require investigation with the exact build ELF. The existing native cycle/stress results are useful regressions, not a substitute for this physical soak or a guarantee of years of service.
