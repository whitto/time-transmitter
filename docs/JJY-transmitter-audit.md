# JJY transmitter audit

The user reported that the watch showed signal level L1 instead of L3. I
compared the current V3.3 firmware and the available earlier RadioClock
source `RadioClock_JJY40_HWTimer_Validated_V2.6.ino` from the project's
`Old-versions` history. No V2.7 or V2.8 source was present in the accessible
repository branches or tags; an exact V2.7/V2.8 link would allow a direct
comparison of those files.

The carrier path is materially the same in the available predecessor and this
build:

| Setting | Earlier source | Current source |
| --- | --- | --- |
| Classic ESP32 output | GPIO26 | GPIO26 |
| LEDC resolution | 10 bit | 10 bit |
| Carrier clock | APB | APB |
| JJY East / West | 40 / 60 kHz | 40 / 60 kHz |
| Full carrier duty | 512 / 1024 (approximately 50%) | 512 / 1024 |
| JJY reduced state | 32 / 1024 | 32 / 1024 |
| Envelope timing | 0.2 / 0.5 / 0.8 s pulse widths | 0.2 / 0.5 / 0.8 s pulse widths |

The current code still generates the carrier in LEDC and drives the envelope
from the hardware-timer radio task. The BLE/RF arbiter adds an intentional
operational condition: while Bluetooth is scanning, connecting, or performing
a GATT exchange, the LF transmitter is not running; when an RF session is due,
Bluetooth is stopped and the controller is shut down before the carrier starts.
Therefore a signal-level test performed during the Bluetooth transaction can
show no valid JJY frame or a weak/intermittent reading. Test signal level only
while the UI/serial status reports the selected JJY station transmitting and
the Bluetooth workflow is idle.

JJY symbols include reduced-carrier intervals. `RF_DUTY_JJY_REDUCED` is the
existing 32/1024 PWM value, while marker/full intervals use 512/1024. The PWM
duty value is not a calibrated antenna-field-strength control: actual range is
set by the external driver, coil/antenna, tuning, supply voltage, wiring, and
receiver orientation. A watch's L1/L3 indication therefore cannot by itself
show that firmware TX power changed.

The V3.3 RF changes address scheduling, wall-clock phase, and safe
BLE/controller handoff. They do not lower the JJY carrier duty or frequency.
The build's RF acceptance still needs a physical test with Bluetooth idle,
the correct JJY station selected, the antenna/driver connected, and the watch
within the same orientation and distance used for the L3 comparison.
