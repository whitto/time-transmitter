# RadioClock V4.2 BLE Scheduler

This version fixes missed automatic Bluetooth watch-sync windows.

- Default automatic targets remain 00:30, 06:30, 12:30 and 18:30.
- Each target is eligible from 5 minutes before through 5 minutes after.
- Automatic scheduling uses the independent Bluetooth timezone, not the JJY/LF timezone.
- If JJY/LF temporarily pre-empts Bluetooth, the automatic slot may resume if the Bluetooth-time window is still open.
- Always Wait pauses completely while RF is active and resumes after RF ends.
- If RF occupies the entire ±5-minute Bluetooth window, that watch attempt cannot be received.
- The clock-confidence 0.20 s baseline uncertainty is unchanged in this release.

Open RadioClock_V4_2_BLE_Scheduler.ino in Arduino IDE. Keep all files in this folder together.
