# Next version updates

## Required for the next and every future release

- [x] V4.15 R4: Update the changelog at the top of the active Arduino `.ino` sketch with the latest version/build, release date, features, fixes and dependency/installation changes. Keep newest notes first, preserve prior notes/original credits and check consistency with GitHub release notes before publication. Requested 10 October 2026 (Brisbane).

## V4.15 R4 — timing diagnostics correction

- [x] Correct the false timing counter caused by initial NTP setting, later clock corrections and deliberate scheduler pauses. Measure genuine continuous active-RF boundary misses/delays with bounded RAM-only state; retain actual RF timing/encoding, BLE protocols, settings and flash-write policy. Focused review/checks passed before source publication, followed by all 87 unittest cases and complete browser checks. Both source ZIPs verified; no further revision needed. No ESP32 compile requested.

## V4.15 — nine further reliability corrections

All nine findings below were queued on 9 October 2026 and then explicitly authorized for immediate correction. They are implemented in **V4.15 R1**. The original [post-publication review](V4.14-R2-post-publication-review.md) preserves the failure evidence; the [V4.15 review](V4.15-review.md) records corrections and focused checks. The pre-publication reliability review and focused checks passed; the published R1 then passed all 77 unittest cases and full/additional browser checks. No revision was required. The ESP32 compile was stopped at the user’s request, with no board-build result claimed. Five direct source ZIPs were verified. Physical acceptance remains outstanding.

- [x] **P1 — NTP reacquisition:** recover from an incorrect initial plausible time anchor using bounded consistent multi-reply/server-confirmed reacquisition, with RF/BT off until trust returns. Test the reproduced one-hour initial error and correct replies over prolonged uptime.
- [x] **P1 — BLE host/scan/controller recovery:** add a common bounded shutdown/progress deadline covering scanner and controller failures when no client exists. Test an unacknowledged scan-stop event and repeated deinit errors; retain RF exclusion throughout recovery.
- [x] **P1 — HTTP request bounds:** limit POST body bytes before allocation and enforce an absolute request deadline below the loop monitor threshold. Test slow/disrupted client transfers and retain responsive firmware supervision.
- [x] **P2 — Final BT clock trust:** cancel active transactions when confidence is lost and recheck immediately before sampling/sending TIME. Test holdover expiry and rejected NTP during battery/font/handshake work; unwind callbacks safely.
- [x] **P2 — Timer retry replenishment:** replace the lifetime-only three-attempt budget with bounded consecutive/rate-window recovery and replenish after a verified healthy period. Keep lifetime diagnostics separate and prevent rapid endless retry.
- [x] **P2 — Strict saved configuration:** validate every field into a complete candidate, reject permissive numeric conversions such as `630junk`, and add versioned content integrity checks with bounded legacy migration. Preserve corrupt files for explicit recovery; add no periodic flash writes.
- [x] **P2 — Checked Wi-Fi shutdown:** check disconnect/mode results, distinguish desired from observed power state, expose a RAM fault and recover with bounded retries if Off fails. Test connected-but-unserviced state and SDK return failures.
- [x] **P3 — Adaptive NTP recovery:** aggregate short-interval drift samples or conservatively update uncertainty when intervals fall below 300 seconds. Test a jitter-induced 210-second interval followed by many correct replies; retain conservative trust limits.
- [x] **P3 — SSID byte limit:** enforce the ESP32 station's 32-byte SSID maximum in UI/API before saving, including UTF-8 byte length. Test accepted/rejected boundaries and setup recovery.


- [x] **V4.15 — Watch RAM history:** show the latest four successful or failed BT attempts for the selected watch, newest first, with date/time and protocol. Four entries per profile use a fixed 224-byte store; clear on reboot/watch replacement, with no new flash writes or snapshot restore.
- [x] **V4.15 — Uptime formatting:** show hours, minutes and seconds consistently in Overview and Diagnostics.

- [x] V4.8: Clarify the successful Bluetooth sync wording in **Watch (BLE) → Last watch result**. Show **“Time sync delivered”** after an acknowledged time write. Keep the technical explanation that the watch's resulting time/display has not been independently verified in Diagnostics. Preserve distinct failure messages and the successful sync date/time. This is a UI wording change; Bluetooth protocol and delivery checks stay unchanged.

- [x] V4.13: replace routine per-sync configuration history writes with the newly approved optional compact daily snapshot. Save the first successful TIME delivery each Brisbane calendar day immediately; later successes/failures stay in RAM. Retain only UTC success timestamps/profile identity, daily quota/validation metadata and four BT diagnostic counters. No log files, battery readings, font results or error text are persisted. Off disables writes/restore and invalidates the previous snapshot; re-enabling/rebooting does not reset the saved daily quota. Keep changed watch bindings and required settings as configuration. Frozen legacy configuration fields prevent RAM history leaking into unrelated setting saves. This supersedes the earlier RAM-only proposal when the history toggle is On; Off remains RAM only. Host, sanitizer and compile checks pass; device acceptance remains outstanding.

- [x] V4.14 R2: stop automatic background config-save retries. A failed flash save must report failure and retain the previous saved configuration; retry only after an explicit user save/retry action. Successfully acknowledged configuration changes must still survive power restarts. R2 latches a RAM fault, consumes the pending attempt and prevents rollback/debounce paths from restarting recurring writes; checked explicit saves clear the fault.

- [x] V4.11: Correct the GW-BX5600 font transaction. Support observed 12-byte packets and existing 17-byte packets while preserving original length and unrelated bytes. Apply and check the optional font before final TIME, then sample the current time. Report font errors separately. Host tests and captured-packet checks pass; actual watch display and manual D-button session acceptance still need device verification.

- [x] V4.11: Correct the reproduced BLE scan-cleanup race. Run both scan start and stop on the NimBLE host queue, retain commands across acknowledgement timeouts, and require safe scan shutdown before connecting or handing control to RF. Disable the auxiliary scan-response timer while retaining active scans and one-second completion callbacks. Native host tests, sanitizer checks and the Node32s compile pass.

- [ ] Verify V4.11 on the physical ESP32/watch: prolonged Always Wait, repeated manual syncs, font Off/Standard/Classic, automatic slots and RF/controller shutdown/restart. The latest V4.10 LoadProhibited trace closely matches the reproduced null waiting-head fault; the earlier corrupted InstructionFetchError trace remains unattributed. Retain the exact user-build ELF if another panic occurs. See `docs/SESSION_HANDOFF.md` and `docs/V4.11-review.md` for evidence and limits.

- [x] V4.14 R2: address the separate NimBLE-Arduino host-timer shutdown issue [1184](https://github.com/h2zero/NimBLE-Arduino/issues/1184) against a suitable upstream dependency release. The correction is included in the source-pinned 2.5.1-radioclock.1 library, separately from the V4.11 scan fix. Checked allocation/initialization and deadline/teardown regressions retain controller-off/RF priority; physical repetition remains required.

- [x] V4.12: Add GW-BX5600 battery estimate retrieval during sync and show the selected watch’s reading on Watch (BLE) and the top overview Watch card. Captured vendor packets/model calibration and cloud regressions validate the implementation; readings stay in RAM, with profile/address isolation and an optional 1.5-second reply wait. Device acceptance remains outstanding.

- [x] V4.13: blue GPIO2 flashes only during an actual Bluetooth transaction, stays solid for 24 hours after successful TIME delivery, and clears after an actual failed transaction. LF, Wi-Fi and passive listening do not make it flash. Preserve the master LED Off choice; use the optional saved UTC success snapshot after reboot. Extracted-task and persistence checks pass.

- [x] V4.13: daily Wi-Fi web-access window in Settings with start/end and an independent timezone, default disabled with 18:00–20:00 Brisbane selected. Enabling selects Power-save; overnight windows are supported and equal start/end rejected. Preserve setup AP, startup/recovery access and brief NTP wakeups; Always on overrides the window. Colour Wi-Fi status green for station/red for AP and radio status green active/red idle without changing the sidebar. Timezone/DST, power-exception, durable-config and browser checks pass.

- [ ] Verify V4.13 on the physical Node32s/GW-BX5600: BT-only LED flashing, success hold/24-hour expiry/failure clearing/master Off, first daily history save and quota across reboot/toggle, RAM-only later events and battery readings, and daily Wi-Fi/NTP/AP access across ordinary and overnight windows. Do not erase saved settings for these checks.

- [x] V4.14 R1: add saved default-Off crash-dump storage toggle in Settings, separate from daily BT history. Restore the 64 KB core-dump slot without changing app/FS layout; use the source-only RadioCrashDumpGate library to gate the SDK writer. Off retains existing dumps; On permits future panic dumps without a daily quota. All 42 host cases, browser/native/config, Node32s compile, image and actual main-ELF gate checks pass. Physical crash storage acceptance remains outstanding.

- [ ] Verify V4.14 crash storage on a spare/test ESP32: default/saved Off/On, no new dump with Off, SDK storage with On, existing dump retained when disabled and normal serial backtraces. Install the complete helper library and use the matching ELF; do not erase settings unnecessarily.

- [x] V4.14 R1: complete the original long-run review. The user then explicitly approved all recommendations in V4.14; R2 implements them. The updated report maps findings to corrections/regressions and records remaining physical-test limits.

- [x] V4.14 R2 P1 reliability correction: reject schedule serialization allocation/overflow/entry-count failures before touching storage; retain the previous schedule and test allocation failure.
- [x] V4.14 R2 P1 reliability correction: pin/validate the separate upstream NimBLE host-timer shutdown correction, preserving RF priority and testing repeated modeled shutdown/restart with production helpers; real-device repetition remains required.
- [x] V4.14 R2 P1 reliability correction: recover station/NTP/RF automatically after router/AP fallback, with bounded retry while retaining setup access.
- [x] V4.14 R2 P1 reliability correction: wake NTP according to clock trust/adaptive deadlines independently of LF/BT/user access schedules, including all-day RF.
- [x] V4.14 R2 P1 reliability correction: handle delayed pause commands after timeout with explicit ownership/cancel/resume; keep RF off and expose a fault if timing initialization/progress fails.
- [x] V4.14 R2 P2 reliability correction: checked config/status serialization, safe filesystem mount recovery without automatic formatting, lighter/on-demand diagnostics, rollover-safe boot-access/uptime and measured task heartbeats/stack margins. Keep measurements in RAM/serial rather than flash logs. The progress monitor accommodates bounded BT work and nonblocking network startup; independent fault tests cover recovery thresholds.

- [x] V4.14 R2: bound boot configuration file/physical lines before String parsing; preserve malformed files, select safe defaults/AP and accept valid legacy/CRLF records. Native boot/storage fault tests and sanitizer checks cover these paths.
- [x] V4.14 R2: use nonblocking association/NTP startup, bounded Bluetooth transaction/deinit recovery, checked singleton allocations, conservative absolute drift uncertainty and pre-commit rejection of implausible NTP replies.
- [x] V4.14 R2: record the continuing requirement in `AGENTS.md`: complete an embedded reliability review after every code change/feature and before publication, with fault regressions and evidence.
- [ ] Physical R2 acceptance: prolonged Node32s/GW-BX5600 soak, real router/NTP outages, repeated RF/BLE/controller handoff, allocation/storage/timer faults, heap/largest-block/stack measurements and crash/history toggles. Use serial/RAM/external measurements; no flash logs or periodic maintenance reboots.
- [x] V4.14 R2: complete the additional post-publication senior review. Nine further issues are queued above with reproduction evidence. The 61-case complete suite, browser checks, Node32s compile and all source ZIP verifications passed; physical acceptance remains outstanding.
