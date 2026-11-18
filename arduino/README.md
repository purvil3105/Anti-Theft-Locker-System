# Smart Anti-Theft Locker Alarm: real ESP32 with the Arduino IDE

The sketch is `LockerAlarm.ino`. When you open it, the Arduino IDE asks to move it into a folder named `LockerAlarm`; click **OK**.
The ThingSpeak channel is the same one you made for Wokwi (see `../wokwi/README.md`, section 1).

## 1. One-time Arduino IDE setup
1. Install the Arduino IDE 2.x from https://www.arduino.cc/en/software.
2. **File → Preferences → Additional boards manager URLs**, add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif Systems**.
4. Plug in the ESP32 with the micro-USB cable. If no COM port appears, install the USB driver for your board's chip (CP210x or CH340).

No extra libraries are needed. `WiFi.h` and `HTTPClient.h` come with the ESP32 package.

## 2. Upload
1. Open `LockerAlarm.ino`.
2. Edit the three lines at the top:
   ```cpp
   const char* WIFI_SSID  = "YOUR_WIFI_NAME";
   const char* WIFI_PASS  = "YOUR_WIFI_PASSWORD";
   const char* TS_API_KEY = "...";   // your ThingSpeak Write API key
   ```
   The ESP32 only works on **2.4 GHz** Wi-Fi. A phone hotspot works well; on iPhone, turn on "Maximise Compatibility".
3. **Tools → Board → ESP32 Dev Module**, and **Tools → Port →** your COM port.
4. Click **Upload**. If it stalls at `Connecting.....`, hold the **BOOT** button until the upload starts.
5. Open **Tools → Serial Monitor** at **115200** baud and press the **EN** (reset) button.

You should see:
```
=== Smart Anti-Theft Locker Alarm ===
Sensor levels at start: VIB=0  TILT=0 (box should be level)
Connecting to Wi-Fi "MyHotspot"....... connected, IP 192.168.43.120
Press BOOT to arm (10 s exit delay).
[CLOUD] state=0 armed=0 vib=0 tilt=0 alarms=0 -> HTTP 200, entry 1
```

## 3. Calibrate the sensors (report setup step 9)

| Check | What to do |
|---|---|
| Vibration sensitivity | While **disarmed**, tap the box. Each tap prints `[VIB] pulse (ignored: not armed)`. Turn the SW-420 potentiometer until a firm tap prints one line and nothing prints with the box at rest. |
| Tilt direction | Tilt the box. It should print `[TILT] tilted / lifted`, then `[TILT] level` when you put it back. If it reads the other way round, change `#define TILT_ACTIVE HIGH` to `LOW`. |
| Buzzer | It should beep once at power-on (self-test). If it sounds all the time and goes quiet during an alarm, your module is low-level trigger: change `#define BUZZ_ON HIGH` to `LOW`. |
| Vibration polarity | If `VIB` shows `1` at rest and pulses don't print, change `#define VIB_ACTIVE HIGH` to `LOW`. |

## 4. Test cases (report Table 7)

| ID | Action | Expected on the board | Expected on ThingSpeak |
|---|---|---|---|
| T1 | Power on | Self-test beep, "connected" | First entry within about 20 s |
| T2 | Press BOOT | Green blinks 10 s, then stays on | Field 2 = 1, Field 1 = 1 |
| T3 | Press BOOT again | All off | Fields 1 and 2 = 0 |
| T4 | Armed: one light tap | `[VIB] 1 of 3`, no alarm | Field 1 stays 1 |
| T5 | Armed: tap 3+ times within 2 s | Red LED, beeping | Field 1 = 2, Field 5 + 1 |
| T6 | Armed: lift or tilt the box | Red LED, continuous buzzer | Field 1 = 3, Field 4 = 1 |
| T7 | Tilt during a tamper alarm | `[STATE] TAMPER -> THEFT` | Field 1 goes from 2 to 3 |
| T8 | Stop all disturbance | Alarm continues until BOOT | Field 1 stays 2 or 3 |
| T9 | Two alarms within 10 s | Second upload waits for the 15 s gap | No `entry 0` lines |
| T10 | Turn off the hotspot during an alarm | Alarm keeps going, `[NET] Wi-Fi lost` | Uploads resume after `[NET] Wi-Fi connected` |

## Differences from the Wokwi sketch

| | Wokwi (`../wokwi/sketch.ino`) | Real board (this sketch) |
|---|---|---|
| Wi-Fi | `Wokwi-GUEST`, channel 6 | Your SSID and password |
| Wi-Fi at boot | Waits forever | Waits up to 15 s, then the alarm runs offline and keeps retrying |
| Buzzer | `tone()` for Wokwi's passive buzzer | `digitalWrite` for the lab's active buzzer module |
| Vibration input | `INPUT_PULLDOWN` (push-button) | `INPUT` (the SW-420 drives the line) |
| Tilt | Read instantly | Must stay tilted 150 ms, which ignores the ball switch chattering |
| Extras | | Power-on self-test beep, sensor levels printed at start, vibration pulses printed while disarmed for tuning |

## Troubleshooting

| Symptom | Fix |
|---|---|
| Board keeps resetting or won't start | Don't hold BOOT while powering on or resetting. GPIO 0 low at reset puts the ESP32 in download mode. |
| `Connecting.....` never finishes during upload | Hold BOOT while the upload starts, or try another USB cable (some cables are charge-only). |
| Wi-Fi never connects | Use 2.4 GHz, check the SSID and password spelling (case-sensitive), keep the hotspot close. |
| `HTTP 200, entry 0` | Wrong Write API key, or two uploads less than 15 s apart. |
| `HTTP -1` | No internet on the hotspot, or the request timed out. The next upload retries. |
| Alarm goes off as soon as the exit delay ends | The box is tilted, or `TILT_ACTIVE` is the wrong way round. Check the `[TILT]` lines. |
| Too many false tamper alarms | Turn the SW-420 potentiometer to make it less sensitive. |
