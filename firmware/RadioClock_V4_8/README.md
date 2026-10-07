# RadioClock V4.8

This version adds per-watch font changes during BT sync, idle Bluetooth power saving and the saved blue activity LED switch. The V4.5 fixes, approved dashboard and eight-item left sidebar are preserved.

- In Watch (BLE), enable **Apply watch font during BT sync** for the selected GW-BX5600 profile and select **Standard (new font)** or **Classic**. Off keeps the watch font untouched. Changes apply on the next ESP32 Bluetooth time delivery and are read back from the watch; a font error does not discard successful time delivery.
- In Network, enable **Power down idle Bluetooth** to shut down the controller between scheduled windows. Manual Pair/Sync requests wake it. Always Wait keeps Bluetooth awake and takes priority over this preference. The CPU, clock and RF scheduler remain running.
- In Settings, the blue GPIO2 LED can stay off or flash during RF/BT activity, independently of Wi-Fi. The preference survives restart. The shared C3 envelope LED is excluded.
- New/unset configurations default all four automatic BT slots and Always Wait to On; existing saved Off choices are preserved.

- Default automatic targets remain 00:30, 06:30, 12:30 and 18:30.
- Each target is eligible from 5 minutes before through 5 minutes after.
- Automatic scheduling uses the independent Bluetooth timezone, not the JJY/LF timezone.
- Every enabled slot repeats daily until disabled, including later slots after a successful sync that day. Switches and slot edits save immediately and survive power restarts; LF schedule saves preserve them.
- Settings APIs acknowledge success only after flash persistence; failed slot writes keep the prior setting.
- If JJY/LF temporarily pre-empts Bluetooth, the automatic slot may resume if the Bluetooth-time window is still open.
- Always Wait pauses completely while RF is active and resumes after RF ends.
- If RF occupies the entire ±5-minute Bluetooth window, that watch attempt cannot be received.
- Bluetooth slot edits reset that slot's attempt state; disabling it stops its active window.
- Sydney Bluetooth daylight-saving changes occur at the correct UTC instant.
- NTP uses its adaptive interval without restarting after every reply, and retains a conservative drift estimate until a valid sample exists. The 0.20 s baseline uncertainty remains part of the clock-confidence calculation.
- LF schedule drafts survive row changes, and an end time of 00:00 represents the end of the day.
- Saved Bluetooth delivery timestamps and status survive a reboot.
- BPC sends distinct 00/20/40-second block codes and parity.

Open `RadioClock_V4_8.ino` in Arduino IDE. Keep all files in this folder together. For a 4 MB ESP32 Dev Module select **Huge APP (3MB No OTA/1MB SPIFFS)**; the default application partition is too small. See the repository README for pinned board/library versions and the complete release checks.

V4.7 added listen/next-JJY Diagnostics summaries and reset/heap/stack evidence. Overlapping diagnostics refreshes remain coalesced. V4.8 fixes NTP startup after Wi-Fi setup/reconnection, retains the setup AP during credential attempts, and preserves recovery access. Bluetooth power saving is now in Settings; acknowledged watch results show Time sync delivered. Validated target: esp32:esp32@3.3.12. Reported physical restarts remain undiagnosed without serial/reset evidence.
