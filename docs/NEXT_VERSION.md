# Next version updates

- [x] V4.8: Clarify the successful Bluetooth sync wording in **Watch (BLE) → Last watch result**. Show **“Time sync delivered”** after an acknowledged time write. Keep the technical explanation that the watch's resulting time/display has not been independently verified in Diagnostics. Preserve distinct failure messages and the successful sync date/time. This is a UI wording change; Bluetooth protocol and delivery checks stay unchanged.

- [ ] Keep Bluetooth sync history in RAM only. Routine successful automatic/manual BT syncs must not write flash merely to record the last successful date/time or watch/protocol completion dates. Show that information during the current uptime; reset it after reboot. Preserve recurring schedule behavior. Persist newly changed watch bindings as configuration when pairing changes them, along with the other required settings.

- [ ] Stop automatic background config-save retries. A failed flash save must report failure and retain the previous saved configuration; retry only after an explicit user save/retry action. Successfully acknowledged configuration changes must still survive power restarts. Remove the recurring three-second retry path and unnecessary background config rewrites.

- [x] V4.11: Correct the GW-BX5600 font transaction. Support observed 12-byte packets and existing 17-byte packets while preserving original length and unrelated bytes. Apply and check the optional font before final TIME, then sample the current time. Report font errors separately. Host tests and captured-packet checks pass; actual watch display and manual D-button session acceptance still need device verification.

- [x] V4.11: Correct the reproduced BLE scan-cleanup race. Run both scan start and stop on the NimBLE host queue, retain commands across acknowledgement timeouts, and require safe scan shutdown before connecting or handing control to RF. Disable the auxiliary scan-response timer while retaining active scans and one-second completion callbacks. Native host tests, sanitizer checks and the Node32s compile pass.

- [ ] Verify V4.11 on the physical ESP32/watch: prolonged Always Wait, repeated manual syncs, font Off/Standard/Classic, automatic slots and RF/controller shutdown/restart. The latest V4.10 LoadProhibited trace closely matches the reproduced null waiting-head fault; the earlier corrupted InstructionFetchError trace remains unattributed. Retain the exact user-build ELF if another panic occurs. See `docs/SESSION_HANDOFF.md` and `docs/V4.11-review.md` for evidence and limits.

- [ ] Assess the separate NimBLE-Arduino host-timer shutdown issue [1184](https://github.com/h2zero/NimBLE-Arduino/issues/1184) against a suitable upstream dependency release. The one-line upstream correction is not in pinned 2.5.1 and is not applied by the V4.11 scan fix. Preserve checked controller-off/RF priority and validate any future dependency change.
