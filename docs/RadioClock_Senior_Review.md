# RadioClock V3.1 / V3.2 senior review

## Release assessment

V3.1 is the reliability and footprint release. V3.2 is deliberately based on the same executable firmware core and replaces only version/UI content, so visual changes do not introduce a second RF/BLE code branch.

## Defects corrected in the V3.1 core

- Overlapping schedule rotation could retain an index that became out of range after the active set shrank, selecting a stale schedule until the next rotation interval.
- ESP32-C3/single-core startup previously assumed core 1 task pinning; startup now selects `xTaskCreate` on unicore builds and checks task creation before enabling the timer.
- Radio pause acknowledgement did not guarantee complete quiescence. Paused radioTask now performs no schedule/frame work and forces RF, LED and buzzer inactive.
- Repeated NTP setup used `configTime(0,0,...)`, which changes the process TZ to UTC before the selected TZ was re-applied. Scheduled NTP wakeups could therefore race frame generation. NTP now pauses RF and uses `configTzTime()` with the selected POSIX rule.
- A frame regenerated mid-100 ms envelope slot could wait until the next slot before updating duty. A regenerated frame now calls the envelope update immediately.
- Stored unsupported timezone strings could bypass HTTP validation and cause display/config disagreement with the POSIX fallback. Stored timezone values are now validated too.
- BLE discovery handoff was made safer by publishing fixed-size callback data instead of changing shared Arduino `String` state from the NimBLE host task.
- BLE response assembly has explicit synchronization, overflow handling and cancellation.
- LEDC frequency-change failure now detaches stale channel state so a future station change can recover.
- Two generated diagnostics contained a literal `\\n`; corrected to real newline escapes.
- Misleading timer comments were corrected: hardware notifications are wakeups and can be coalesced; wall-clock phase is the RF timebase.

## Footprint changes

- Bluedroid BLE client API was replaced by NimBLE-Arduino 2.5.1+, which is specifically intended to reduce resource usage compared with the original ESP32 BLE stack.
- V3.1 browser UI: 26,422 bytes raw HTML -> 8,120 bytes gzip in flash (69.3% smaller asset).
- V3.2 browser UI: 36,842 bytes raw HTML -> 10,872 bytes gzip in flash (70.5% smaller asset than raw).
- A 4 MB 3 MB-app partition table is included in the Arduino-ready ZIPs for Node32 board definitions that lack a Partition Scheme menu.

## Standards / intentional emulator behavior

This remains a local receiver emulator rather than a literal recreation of each national broadcast. In particular, the selected local timezone/offset model is preserved across formats for the user's use case. The real JJY :15/:45 call-sign interruption is documented but intentionally not inserted into normal watch-sync frames, because doing so can reduce local receiver acquisition reliability.

## Verification performed

- Version/header/file-name consistency.
- C++ delimiter/lexical structure check with comments and strings removed.
- No duplicate ordinary function definitions found by source scan.
- No obsolete Bluedroid callback APIs or Arduino-ESP32 2.x timer/LEDC APIs remain.
- Embedded gzip byte arrays decompress successfully to complete HTML.
- Extracted JavaScript passes `node --check`.
- Every static `$('<id>')` reference in the UI resolves to an HTML element.
- Every UI `/api/...` endpoint is present in the firmware route table.
- V3.2 executable core normalizes identically to V3.1; only the version/UI content differs.

## Remaining verification boundary

No Arduino-ESP32 compiler/toolchain or physical Node32/watch hardware is available in the preparation environment. Therefore this review does not claim a successful Arduino compile, final linked flash byte count, RF waveform measurement, or live Casio BLE test. Those are the next hardware acceptance tests after upload.
