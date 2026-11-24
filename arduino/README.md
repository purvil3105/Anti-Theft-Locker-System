# Smart Anti-Theft Locker Alarm: real ESP32 with the Arduino IDE

The sketch is `LockerAlarm.ino` (version 2: web dashboard, PIN-protected arm / disarm, email alerts).
When you open it, the Arduino IDE asks to move it into a folder named `LockerAlarm`; click **OK**.
Before uploading, set up ThingSpeak as described in [`../SETUP.md`](../SETUP.md), section 3. It gives you the keys this sketch needs.

## 1. One-time Arduino IDE setup
1. Install the Arduino IDE 2.x from https://www.arduino.cc/en/software.
2. **File → Preferences → Additional boards manager URLs**, add:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif Systems**.
4. Plug in the ESP32 with the micro-USB cable. If no COM port appears, install the USB driver for your board's chip (CP210x or CH340).

No extra libraries are needed. `WiFi.h`, `WiFiClientSecure.h` and `HTTPClient.h` come with the ESP32 package.

## 2. Upload
1. Open `LockerAlarm.ino`.
2. Edit the settings block at the top:
   ```cpp
   const char* WIFI_SSID    = "YOUR_WIFI_NAME";
   const char* WIFI_PASS    = "YOUR_WIFI_PASSWORD";
   const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";     // channel Write API key
   const char* TB_ID        = "YOUR_TALKBACK_ID";       // TalkBack ID
   const char* TB_KEY       = "YOUR_TALKBACK_API_KEY";  // TalkBack API key
   const char* ALERTS_KEY   = "YOUR_ALERTS_API_KEY";    // Account > My Profile > Alerts API Key
   const char* OWNER_PIN    = "1234";                   // your 4-digit PIN (same as on the dashboard)
   #define BOOT_CAN_DISARM false                        // true = BOOT also disarms (offline lab tests)
   ```
   The ESP32 only works on **2.4 GHz** Wi-Fi. A phone hotspot works well; on iPhone, turn on "Maximise Compatibility".
3. **Tools → Board → ESP32 Dev Module**, and **Tools → Port →** your COM port.
4. Click **Upload**. If it stalls at `Connecting.....`, hold the **BOOT** button until the upload starts.
5. Open **Tools → Serial Monitor** at **115200** baud, set the line ending to **Newline**, and press the **EN** (reset) button.

You should see:
```
=== Smart Anti-Theft Locker Alarm v2 (web dashboard + email alerts) ===
Sensor levels at start: VIB=0  TILT=0 (box should be level)
Connecting to Wi-Fi "MyHotspot"....... connected, IP 192.168.43.120
Arm: press BOOT or use the web dashboard (10 s exit delay).
Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.
[CLOUD] state=0 armed=0 vib=0 tilt=0 alarms=0 cmd=0/#0 mail=0 -> HTTP 200, entry 1
```

## 3. How to arm and disarm

| Action | How |
|---|---|
| Arm | BOOT button, the web dashboard (PIN), or Serial `ARM <pin>`. A 10 s exit delay follows. |
| Disarm | Web dashboard (PIN), or Serial `DISARM <pin>`. BOOT does **not** disarm unless `BOOT_CAN_DISARM` is `true`. |
| Status | Serial `STATUS` |

Five wrong PINs (from the dashboard or Serial) block all commands for 5 minutes.

## 4. Calibrate the sensors (report setup step 9)

| Check | What to do |
|---|---|
| Vibration sensitivity | While **disarmed**, tap the box. Each tap prints `[VIB] pulse (ignored: not armed)`. Turn the SW-420 potentiometer until a firm tap prints one line and nothing prints with the box at rest. |
| Tilt direction | Tilt the box. It should print `[TILT] tilted / lifted`, then `[TILT] level` when you put it back. If it reads the other way round, change `#define TILT_ACTIVE HIGH` to `LOW`. |
| Buzzer | It should beep once at power-on (self-test). If it sounds all the time and goes quiet during an alarm, your module is low-level trigger: change `#define BUZZ_ON HIGH` to `LOW`. |
| Vibration polarity | If `VIB` shows `1` at rest and pulses don't print, change `#define VIB_ACTIVE HIGH` to `LOW`. |

## 5. Serial Monitor messages

| Prefix | Meaning |
|---|---|
| `[STATUS]` | Event text; also sent to ThingSpeak and shown in the dashboard's Activity list |
| `[STATE]` | State change, `*** ALARM ***` for Tamper or Theft |
| `[TALKBACK]` | A dashboard command arrived (the PIN is never printed) |
| `[CMD]` | Result of a command: armed, disarmed, wrong PIN, locked out, expired, invalid |
| `[EMAIL]` | Email alert accepted, rate-limited (retry in 5 min) or failed (retry in 30 s) |
| `[CLOUD]` | Channel upload and its entry number |
| `[NET]` | Wi-Fi lost or reconnected |

## 6. Test cases
The full list (T1 to T18, including the dashboard and email tests) is in [`../SETUP.md`](../SETUP.md#6-test-plan).

## Differences from the Wokwi sketch

| | Wokwi (`../wokwi/sketch.ino`) | Real board (this sketch) |
|---|---|---|
| Wi-Fi | `Wokwi-GUEST`, channel 6 | Your SSID and password |
| Buzzer | `tone()` for Wokwi's passive buzzer | `digitalWrite` for the lab's active buzzer module |
| Vibration input | `INPUT_PULLDOWN` (push-button) | `INPUT` (the SW-420 drives the line) |
| Tilt | Must stay tilted 20 ms | Must stay tilted 150 ms, which ignores the ball switch chattering |

Everything else is identical: the dashboard, PIN, TalkBack and email code.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Board keeps resetting or won't start | Don't hold BOOT while powering on or resetting. GPIO 0 low at reset puts the ESP32 in download mode. |
| `Connecting.....` never finishes during upload | Hold BOOT while the upload starts, or try another USB cable (some cables are charge-only). |
| Wi-Fi never connects | Use 2.4 GHz, check the SSID and password spelling (case-sensitive), keep the hotspot close. |
| `HTTP 200, entry 0` | Wrong Write API key, or two uploads less than 15 s apart. |
| `HTTP -1` | No internet on the hotspot, or the request timed out. The next upload retries. |
| `[TALKBACK] poll failed, HTTP 401` | Wrong `TB_ID` or `TB_KEY`. |
| `[EMAIL] failed, HTTP 401` | Wrong `ALERTS_KEY`, or it was regenerated. |
| Alarm goes off as soon as the exit delay ends | The box is tilted, or `TILT_ACTIVE` is the wrong way round. Check the `[TILT]` lines. |
| Too many false tamper alarms | Turn the SW-420 potentiometer to make it less sensitive. |

More problems and fixes: [`../SETUP.md`](../SETUP.md#7-troubleshooting).
