# RadioClock V4.12 — Arduino source

Open `RadioClock_V4_12.ino` in Arduino IDE **2.3.4**. Keep this entire folder together: all five companion headers and `partitions.csv` must stay beside the sketch. No separate UI upload is needed; the web interface is embedded in the sketch.

Install **esp32 by Espressif Systems 3.3.12**, **NimBLE-Arduino 2.5.1** and **ArduinoJson 6.21.5**. For the user's classic 4 MB Node32 / Node32s board, choose **Node32s**, **40 MHz** flash and **No OTA (Large APP)**. ESP32 Dev Module with **4MB** and **Huge APP** is also supported. The supplied partition table defines the existing 3 MB application layout. The default small application partition is insufficient. Routine uploads do not require erasing all flash; erasing removes saved settings.

V4.12 reads the GW-BX5600 battery estimate during Bluetooth sync, before the SP/time handshake. It shows the selected watch's last estimate on **Watch (BLE)** and the overview's top **Watch** card. The percentage is calibrated in 10% steps. The battery reply wait is limited to 1.5 seconds; a battery-only failure does not prevent a time attempt. Invalid or missing readings show unavailable, never a fabricated 0%.

Battery readings are kept per watch in RAM, with the reading date/time in the Bluetooth timezone and offset. They clear on reboot, and a newly paired watch cannot inherit another watch's estimate. Other watch protocols have no validated battery calibration and show unavailable. This feature adds no flash writes.

The V4.11 host-serialized scan correction and optional 12-/17-byte font transaction remain. Font work and readback precede the freshly sampled final time. Working Wi-Fi/NTP, RF timing, timezones, recurring schedules, saved configuration, LED control and the eight-item sidebar are preserved. Physical ESP32/watch acceptance remains required.

With no saved Wi-Fi credentials, connect to **RadioStation_XXXXXX**, password **12345678**, and open **http://192.168.4.1**.

Project, full documentation and original source credits: <https://github.com/whitto/time-transmitter>. Original RadioClock code credits **tarohs/nisejjy** (2021) and **5Breeze/ClockWaveXmitter** (2025), retained in the sketch. Casio battery mapping and packet examples were checked against **izivkov/gshock-smart-sync-webapp** and **izivkov/gshock_api**; the helper is independently implemented.
