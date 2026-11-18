# Smart Anti-Theft Locker Alarm: Wokwi simulation + ThingSpeak

Use **Wokwi**, not Tinkercad. Tinkercad Circuits has no ESP32 and no Wi-Fi, so it cannot reach
ThingSpeak. Wokwi simulates the ESP32 and gives it internet access through its virtual `Wokwi-GUEST` network.

| File | What it is |
|---|---|
| `sketch.ino` | ESP32 firmware (the report's Appendix A, adapted for the simulator) |
| `diagram.json` | Wokwi circuit diagram |

---

## 1. ThingSpeak cloud setup (do this first, you need the API key)

### 1.1 Account and channel
1. Go to https://thingspeak.mathworks.com and sign in, or create a free MathWorks account.
2. **Channels → My Channels → New Channel**, then fill in:

   | Setting | Value |
   |---|---|
   | Name | `Smart Locker Alarm` |
   | Field 1 | `System State` (tick the box) |
   | Field 2 | `Armed` |
   | Field 3 | `Vibration Events` |
   | Field 4 | `Tilt Detected` |
   | Field 5 | `Total Alarms` |

3. Click **Save Channel**.
4. Open the **API Keys** tab and copy the **Write API Key**. Paste it into `sketch.ino`:
   ```cpp
   const char* TS_API_KEY = "YOUR_WRITE_API_KEY";
   ```
5. To show the dashboard to others, open **Sharing** and choose **Share channel view with everyone**. The **Public View** tab then has its own link.

### 1.2 Dashboard widgets (Private View tab)
ThingSpeak creates one line chart per field automatically. Change them and add widgets like this:

| Field | Edit the chart (pencil icon) or **Add Widgets** | Settings |
|---|---|---|
| 1 State | Chart: Type **step**, Results **60**, Y-axis min 0 max 3 | Title "System state (0 Disarmed, 1 Armed, 2 Tamper, 3 Theft)" |
| 1 State | Widget **Numeric Display** | Name "Current state", Field 1 |
| 2 Armed | Widget **Lamp Indicator** | Field 2, condition `>= 1`, colour green |
| 3 Vibrations | Chart: Type **column** (bar), Results **60** | Title "Vibration events per upload" |
| 4 Tilt | Widget **Lamp Indicator** | Field 4, condition `>= 1`, colour red |
| 5 Total alarms | Widget **Numeric Display** | Field 5 |

Drag the widgets into three rows: status (Armed lamp, Tilt lamp, Current state), history (state chart and vibration chart), and summary (Total alarms).
To get the timestamped history as a file, use **Data Import / Export → Export recent data** (CSV).

---

## 2. Wokwi simulation setup

1. Open https://wokwi.com/projects/new/esp32 and sign in so you can save the project.
2. In the **sketch.ino** tab, delete the default code and paste in `sketch.ino`. Set your Write API key.
3. Open the **diagram.json** tab, select all, and paste in `diagram.json`. The circuit appears on the right.
4. No libraries are needed. `WiFi.h` and `HTTPClient.h` come with the ESP32 core.
5. Click the green **▶ Start** button. The Serial Monitor should show:
   ```
   === Smart Anti-Theft Locker Alarm ===
   Connecting to Wi-Fi.... connected, IP 10.13.37.2
   Press BOOT to arm (10 s exit delay).
   [CLOUD] state=0 armed=0 vib=0 tilt=0 alarms=0 -> HTTP 200, entry 1
   ```
6. Click **Save** (top left) so the project gets its own URL that you can share.

### How the real sensors are simulated

Wokwi has no SW-420 or SW-520D model, so each sensor is replaced by a part that produces the same digital signal:

| Real part | Wokwi part | How to use it | GPIO |
|---|---|---|---|
| SW-420 vibration sensor | Yellow push-button | Each click is one vibration pulse (HIGH). Keyboard: **V** | 27 |
| SW-520D tilt sensor | Slide switch | Left = level (LOW), right = tilted/lifted (HIGH) | 26 |
| On-board BOOT button | Blue push-button (wired GPIO 0 → GND, same as on the board) | Each click arms or disarms. Keyboard: **B** | 0 |
| Active buzzer module | Wokwi buzzer (passive, driven with `tone()`) | Sound: turn on audio in the simulator | 18 |
| Green / red LED | LEDs through 220 Ω resistors | | 19 / 21 |

To use the keyboard shortcuts, click on the diagram once so it has focus.

### Wiring (same as report Table 4)

```
VIBRATION button : one side -> 3V3,       other side -> GPIO 27   (internal pull-down)
TILT slide switch: pin 1 -> GND,  pin 2 (common) -> GPIO 26,  pin 3 -> 3V3
BOOT button      : one side -> GPIO 0,    other side -> GND       (internal pull-up)
Buzzer           : + -> GPIO 18,          - -> GND
Green LED        : GPIO 19 -> 220 Ω -> anode, cathode -> GND
Red LED          : GPIO 21 -> 220 Ω -> anode, cathode -> GND
```

---

## 3. Test plan in the simulator (report Table 7)

| ID | Do this in Wokwi | Expected |
|---|---|---|
| T1 | Start the simulation | "connected" printed, first ThingSpeak entry within about 20 s |
| T2 | Click **BOOT** once | Green LED blinks for 10 s, then stays on. Field 2 = 1 |
| T3 | Click **BOOT** again | Everything off. Fields 1 and 2 = 0 |
| T4 | Armed: click **VIBRATION** once | `[VIB] 1 of 3`, no alarm, state stays 1 |
| T5 | Armed: click **VIBRATION** 3 times quickly (< 2 s) | Red LED, beeping buzzer, Field 1 = 2, Field 5 goes up |
| T6 | Armed: slide **TILT** to the right | Red LED, continuous tone, Field 1 = 3, Field 4 = 1 |
| T7 | During a tamper alarm, slide TILT right | `[STATE] TAMPER -> THEFT` |
| T8 | After an alarm, stop all input (slide TILT back) | Alarm continues until you click BOOT |
| T9 | Trigger two alarms within 10 s | Second upload waits for the 15 s gap, no `entry 0` |
| T10 | Not possible in Wokwi (Wi-Fi cannot be switched off) | Test on the real hardware |

Each upload prints a `[CLOUD] ... HTTP 200, entry N` line. Open the ThingSpeak channel's **Private View** to watch the charts update.

**Tip:** disarm (BOOT) and slide TILT back to the left before arming again. If TILT is still to the right when the 10 s exit delay ends, the system goes straight to THEFT.

---

## 4. Moving back to the real hardware

Use the ready-made hardware sketch in `../arduino/LockerAlarm.ino` (setup steps in `../arduino/README.md`).
Or, to reuse this sketch, change four lines in `sketch.ino`:

```cpp
const char* WIFI_SSID = "your-hotspot";
const char* WIFI_PASS = "your-password";
WiFi.begin(WIFI_SSID, WIFI_PASS);            // drop the WIFI_CHANNEL argument
#define BUZZER_PASSIVE false                 // the lab buzzer module is active
```

Also check the tilt module's output level in the Serial Monitor (`[TILT]` lines). If it reads the opposite way round, change `TILT_ACTIVE` to `LOW`.
`INPUT_PULLDOWN` on GPIO 27 is harmless with the real SW-420 because its DO pin drives the line itself.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `HTTP 200, entry 0` | Wrong Write API key, or two updates less than 15 s apart |
| `HTTP -1` / `-11` | Simulator network hiccup. The next upload retries automatically |
| Stuck on `Connecting to Wi-Fi....` | SSID must be exactly `Wokwi-GUEST` with an empty password |
| No sound | Turn on audio in Wokwi (speaker icon), or raise `volume` on the buzzer in diagram.json |
| One click counts as 2 vibrations | Button bounce. Add `"bounce": "0"` to `btnVib` attrs |
