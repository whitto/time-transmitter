# RadioClock V4.11

V4.11 corrects concurrent Bluetooth scan start/stop and callback cleanup, and updates the optional GW-BX5600 font transaction. Cloud tests and the Node32s compile passed; see the root `docs/V4.11-review.md` for actual results and hardware limits.

`RadioBleScanControl.h` sends both scan start and stop operations through the NimBLE host event queue. It retains pending command state on timeout, blocks RF until shutdown is confirmed, and releases its event only after the host stops. Active scanning remains enabled; the independent scan-response timer is disabled while the one-second scan-completion fallback retains watch discovery.

The font helper supports the observed 12- and 17-byte settings packets, preserving their original length and unrelated bytes. It reads/changes/checks the optional font before final TIME, then calculates current time and sends it last. Font Off preserves the time-only sequence; font errors are reported separately from time delivery. A real watch display check is still required.

Open `RadioClock_V4_11.ino` in Arduino IDE and keep **all companion headers**, including `RadioBleScanControl.h`, `RadioBleArbiter.h`, `CasioBxProtocol.h`, `CasioWatchSettings.h`, and `partitions.csv` beside it. Target **esp32:esp32@3.3.12**, NimBLE-Arduino **2.5.1**, ArduinoJson **6.21.5**, and 4 MB flash. Node32s **No OTA (Large APP)** or ESP32 Dev Module **Huge APP** supplies sufficient compile capacity; the sketch-local partition table retains the existing 3 MB app layout. The default application partition is too small. The user's Node32s flash frequency is 40 MHz.

Wi-Fi/NTP behavior, LF carrier/envelope timing, independent JJY/BT timezones, recurring schedules, saved settings, LED control and the approved eight-item sidebar are preserved. RF has priority over BLE. Existing saved Off settings stay Off. Bluetooth power saving remains in Settings; Always Wait takes priority.

The user resolved the earlier missing Wi-Fi AP separately. With no saved credentials, the inherited setup network is **RadioStation_XXXXXX**, password **12345678**, at **http://192.168.4.1**. V4.10's AP readiness checks and V4.9's cold TCP/IP guard remain.

The two runtime flash-write reductions remain queued in `docs/NEXT_VERSION.md`; no log-file writes are added. The header and About page also link to the project on GitHub. This version contains source only. Read the root README, `docs/V4.11-review.md`, and `docs/SESSION_HANDOFF.md` for release status, evidence, remaining device tests and original credits.
