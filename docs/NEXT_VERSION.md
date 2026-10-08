# Next version updates

- [x] V4.8: Clarify the successful Bluetooth sync wording in **Watch (BLE) → Last watch result**. Show **“Time sync delivered”** after an acknowledged time write. Keep the technical explanation that the watch's resulting time/display has not been independently verified in Diagnostics. Preserve distinct failure messages and the successful sync date/time. This is a UI wording change; Bluetooth protocol and delivery checks stay unchanged.

- [ ] Keep Bluetooth sync history in RAM only. Routine successful automatic/manual BT syncs must not write flash merely to record the last successful date/time or watch/protocol completion dates. Show that information during the current uptime; reset it after reboot. Preserve recurring schedule behavior. Persist newly changed watch bindings as configuration when pairing changes them, along with the other required settings.

- [ ] Stop automatic background config-save retries. A failed flash save must report failure and retain the previous saved configuration; retry only after an explicit user save/retry action. Successfully acknowledged configuration changes must still survive power restarts. Remove the recurring three-second retry path and unnecessary background config rewrites.
