# RadioClock V4.10

V4.10 corrects setup AP naming and startup reporting on Arduino-ESP32 3.3.12. It derives the SSID from the device AP MAC independently of the asynchronous AP-start event, checks driver/configuration/start/local-IP readiness, and retries failed startup after five seconds. Successful APs are left running; startup recovery does not write configuration or schedules.

With no saved credentials, connect to **RadioStation_XXXXXX**, password **12345678**, then open **http://192.168.4.1**. The setup network is visible on 2.4 GHz channel 1. If the device MAC cannot be read, the fallback is **RadioStation_Setup**. A serial error identifies failed startup rather than printing success. Actual broadcasting must still be confirmed on hardware.

Open `RadioClock_V4_10.ino` in Arduino IDE and keep all companion files beside it. Target **esp32:esp32@3.3.12**, NimBLE-Arduino **2.5.1**, ArduinoJson **6.21.5**, and 4 MB flash. Node32s **No OTA (Large APP)** or ESP32 Dev Module **Huge APP** supplies sufficient compile capacity; the sketch-local partition table remains the existing 3 MB app layout. The default application partition is too small.

V4.9's cold TCP/IP mailbox assertion fix remains. V4.8's AP-to-STA provisioning and NTP recovery remain. The approved eight-item sidebar, RF timing, Bluetooth delivery/font behavior, independent JJY/BT timezones, recurring schedules, LED control and existing persistence are unchanged. Bluetooth power saving is in Settings; Always Wait takes priority. Existing explicit Off settings remain Off.

The two requested runtime flash-write reductions remain queued in `docs/NEXT_VERSION.md`. Physical radio/watch operation and the earlier intermittent checksum/panic reports require device evidence; this version does not claim to diagnose those unrelated failures. See the root README and `docs/V4.10-review.md` for validation and source scope.
