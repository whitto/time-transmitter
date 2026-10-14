# Bluetooth power-save failure investigation

Recorded 14 October 2026; device evidence updated through 15 October (Australia/Brisbane). Device: Time Transmitter **V4.15 R4**, GW-BX5600. Repository main is V4.16 R1; the reviewed power-save/startup/shutdown functions are identical in these two versions. The V4.16 schedule changes do not establish a fix for this incident.

## Observed device behavior

1. The transmitter ran for about a day with successful scheduled Bluetooth and LF synchronization, with Always Wait Off.
2. The last reported successful BT delivery was **2026-10-13 18:30:38** in BT civil time. The user enabled Wi-Fi and BT power saving after that success.
3. Subsequent scheduled synchronization did not succeed. Manual Sync Now and Always Wait also failed.
4. Turning only BT power saving Off did not restore manual synchronization. Turning Wi-Fi to Always on as well did not restore it without a reboot. The user reported no other active watch client.
5. A reboot with both power-saving options Off restored synchronization. This established the working baseline; it does not identify which transition or retained state failed.
6. From that working state, the user enabled **BT power saving alone**, with Wi-Fi Always on and Always Wait Off, waited for the idle period and successfully completed Sync Now plus the watch's manual sync. One isolated BT shutdown/restart works on the device. It does not establish long-run or combined-transition reliability.
7. With **Wi-Fi power saving On**, **BT power saving Off** and **Always Wait Off**, the scheduled **12:30 Brisbane** sync succeeded on 14 October. Raw Diagnostics records delivery at **12:30:39**. The saved 07:00–10:00 access window was no longer active. No trace explicitly showing the earlier driver Off transition was supplied, so this confirms the scheduled configuration worked without establishing every Wi-Fi transition.
8. The next morning's snapshot records a successful delivery at **2026-10-15 00:30:36**, followed by a window that ended with "no watch connection". The user explicitly clarified that **BT power saving was Off and Wi-Fi power saving was On**. Although testing both together had been proposed, this capture is another Wi-Fi-only power-save observation. The snapshot does not identify the expired window's time or establish whether the watch advertised then; do not attribute a failed 06:30 slot or a successful 18:30 slot from this data alone.

The [successful scheduled-sync snapshot](build-evidence/power-save-investigation/scheduled-sync-2026-10-14-1230.json) records V4.15 R4, uptime 3h 45m 43s, successful TIME delivery, four accumulated connection attempts/16 acknowledged writes/16 notifications/zero response errors, no RF/Wi-Fi/storage faults, free heap 78,784 bytes and largest block 69,620 bytes. Clock confidence was Synchronized with an NTP age of 339 seconds and estimated error of 0.327 seconds. The earlier 08:46:24 saved BT snapshot is consistent with the first-success-per-day quota; the latest 12:30:39 result is in RAM.

The [15 October morning snapshot](build-evidence/power-save-investigation/diagnostics-2026-10-15-morning.json) records uptime **24h 47m 29s**, six accumulated connection attempts/24 acknowledged writes/24 notifications/zero response errors, free heap **81,852 bytes**, largest block **73,716 bytes** and the same minimum free heap of 53,448 bytes. The higher current heap values do not establish leak freedom, but these two captures show no heap decline. Clock confidence remains Synchronized (NTP age 387 seconds, estimated error 0.275 seconds), with no reported RF/Wi-Fi/storage faults or missed second boundaries. The controller is On and idle, consistent with BT power saving Off. The first success after midnight was saved at 00:30:36, consistent with the daily quota. "Last outcome: Successful" still describes the last actual transaction; "no watch connection" describes a later expired window, not a successful delivery or a proven controller fault.

During a failed manual attempt, raw Diagnostics reported:

- Controller On (`bt_controller_off=false`), `bt_state="Scanning"`, listening window Active.
- No RF request, no radio pause, station -1, no RF fault or command timeouts.
- Clock Synchronized, accepted NTP reply one second old, no rejected replies or reacquisition.
- Free heap 77,924 bytes, largest block 65,524 bytes; minimum free heap 48,884 bytes.
- Connection attempts remained **16**, with **60 acknowledged writes, 60 notifications and zero reported response errors**. These are accumulated counters, not evidence of success during the failing window.
- Daily Wi-Fi access was active, 07:00–10:00 Australia/Brisbane. Wi-Fi being connected in that snapshot is consistent with the saved access schedule.

No serial capture or actual advertisement trace was available. BT status flash persistence was disabled; recent attempt history remains RAM-only. The earlier "Last outcome: Successful" refers to the last actual delivery, not an empty listening-window success.

## Source review and focused evidence

The manual request explicitly admits a Bluetooth wake. Activating a window requires initialized BLE. Manual `Scanning` reflects `NimBLEScan::isScanning()`, which queries the active SDK discovery procedure. These observations exclude a current initialization-admission block or unopened window in the captured manual attempt. They do not prove RF packets reached the controller or passed the watch filters.

Idle and RF shutdown use acknowledged scanner/client teardown, retained NimBLE objects, confirmed controller-Off and a recreated host scan event. Startup reapplies active scan settings. The pinned core reapplies Wi-Fi sleep mode at STA startup and enables coexistence/full-scan support. The normal NTP pause/resume path does not leave an RF request set when schedules are idle. No specific persistent failure in these paths was demonstrated by source inspection.

Focused actual-helper ASan/UBSan probes, run outside the repository, established:

- With injected low internal heap, cold startup can be deferred while an already-running BLE stack would scan. This is a valid conditional admission path, **not the explanation for the captured healthy, initialized/scanning attempt**. Do not lower memory safeguards from this incident alone.
- A window with every scanner start failing ends as "no watch connection", hiding the earlier scan error. This is a confirmed reporting defect.
- 2,000 modeled daily wake/scan/delivery/shutdown cycles passed, including `millis()` wrap. These mocks validate application transitions, not the physical ESP32 radio/controller.
- A changed watch address is rejected by the binding guard with either power-save setting. No changed address was observed, and reboot recovery with saved bindings makes a permanent binding mismatch less likely. Do not loosen identity checks or automatically re-pair.

The vendor scanner also suppresses an advertisement before application callbacks when its matching retained client reports connected. Application quiescence checks connection handle and application flags, whereas vendor `isConnected()` uses its connection-status state. Normal disconnect clears both before the host barrier; **no normal stale-connected path was reproduced**. This is a diagnostic hypothesis, not an identified defect.

## Next evidence and change limits

The underlying fault remains open. The isolated BT-only manual restart and scheduled synchronization with Wi-Fi power saving alone succeeded, with further overnight delivery under the latter configuration. Combined transitions remain untested after the clean restart; successful cases do not rule out a cumulative failure. Keeping Wi-Fi power saving On and BT power saving Off is now a demonstrated scheduled configuration, rather than disabling both. For the agreed comparison, explicitly enable both power-saving options, keep Always Wait Off, and capture Diagnostics during or immediately after a scheduled watch attempt, before rebooting. A post-window "no watch connection" alone does not distinguish an absent watch advertisement from a scanner/filter/controller failure. Retain actual Off/On evidence if attributing the fault to a particular driver transition.

If more firmware instrumentation is needed, use bounded RAM counters/snapshots for controller initialization generation, scan starts/completions and completion reason, advertisements/Casio candidates/filter rejections, and vendor/client handle/connected/application-quiescent state. Retain scanner error evidence across window expiry. A fixed latest-candidate address/type snapshot can distinguish filtering from missing advertisements; it must not bypass pairing identity.

Keep the working BT packets, LF timing, schedules and persisted configuration unchanged until a specific correction is justified. No new flash logs, periodic reboot policy or speculative dependency upgrade is warranted. Any firmware change still needs a version bump, fresh embedded reliability review, accurate release notes and the user's publish-before-full-suite order.

**No firmware source, dependency or compiled image was changed for this investigation.**
