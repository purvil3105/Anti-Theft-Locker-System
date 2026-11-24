# Smart Anti-Theft Locker Alarm: Wokwi simulation + ThingSpeak

Use **Wokwi**, not Tinkercad. Tinkercad Circuits has no ESP32 and no Wi-Fi, so it cannot reach
ThingSpeak. Wokwi simulates the ESP32 and gives it internet access through its virtual `Wokwi-GUEST` network.
That is enough for the full version 2 system: cloud uploads, arm / disarm from the web dashboard, and email alerts.

| File | What it is |
|---|---|
| `sketch.ino` | ESP32 firmware version 2 (the report's Appendix A, adapted for the simulator) |
| `diagram.json` | Wokwi circuit diagram (unchanged from version 1) |

---

## 1. ThingSpeak cloud setup (do this first, you need the keys)

Follow [`../SETUP.md`](../SETUP.md), section 3. It creates:
- the channel **Smart Locker Alarm** with 8 fields (Write and Read API keys),
- the **TalkBack** `Locker Commands` (TalkBack ID and API key) for dashboard commands,
- the **Alerts API key** for email alerts (sent to the ThingSpeak account's email, 2023kuec2054@iiitkota.ac.in).

---

## 2. Wokwi simulation setup

1. Open https://wokwi.com/projects/new/esp32 and sign in so you can save the project.
2. In the **sketch.ino** tab, delete the default code and paste in `sketch.ino`.
3. Fill in the settings block (the Wi-Fi lines are already set for Wokwi):
   ```cpp
   const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";
   const char* TB_ID        = "YOUR_TALKBACK_ID";
   const char* TB_KEY       = "YOUR_TALKBACK_API_KEY";
   const char* ALERTS_KEY   = "YOUR_ALERTS_API_KEY";
   const char* OWNER_PIN    = "1234";
   ```
4. Open the **diagram.json** tab, select all, and paste in `diagram.json`. The circuit appears on the right.
5. No libraries are needed. `WiFi.h`, `WiFiClientSecure.h` and `HTTPClient.h` come with the ESP32 core.
6. Click the green **▶ Start** button. The Serial Monitor should show:
   ```
   === Smart Anti-Theft Locker Alarm v2 (web dashboard + email alerts) ===
   Connecting to Wi-Fi "Wokwi-GUEST".... connected, IP 10.13.37.2
   Arm: press BOOT or use the web dashboard (10 s exit delay).
   Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.
   [CLOUD] state=0 armed=0 vib=0 tilt=0 alarms=0 cmd=0/#0 mail=0 -> HTTP 200, entry 1
   ```
7. Click **Save** (top left) so the project gets its own URL that you can share.
8. Open the web dashboard (`../dashboard/index.html`, see `../SETUP.md` section 5) and enter the same channel, TalkBack and PIN. It controls the simulated locker exactly like the real one.

You can also type `ARM 1234`, `DISARM 1234` or `STATUS` in Wokwi's Serial Monitor input box.

### How the real sensors are simulated

Wokwi has no SW-420 or SW-520D model, so each sensor is replaced by a part that produces the same digital signal:

| Real part | Wokwi part | How to use it | GPIO |
|---|---|---|---|
| SW-420 vibration sensor | Yellow push-button | Each click is one vibration pulse (HIGH). Keyboard: **V** | 27 |
| SW-520D tilt sensor | Slide switch | Left = level (LOW), right = tilted/lifted (HIGH) | 26 |
| On-board BOOT button | Blue push-button (wired GPIO 0 → GND, same as on the board) | Click to arm. It does not disarm (use the dashboard or Serial) unless `BOOT_CAN_DISARM` is `true`. Keyboard: **B** | 0 |
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

## 3. Test plan in the simulator

| ID | Do this in Wokwi | Expected |
|---|---|---|
| T1 | Start the simulation | "connected" printed, first ThingSpeak entry within about 20 s, dashboard "Locker online" |
| T2 | Click **BOOT** once | Green LED blinks for 10 s, then stays on. Field 2 = 1 |
| T3 | Dashboard: **Disarm** + PIN | Everything off. Fields 1 and 2 = 0. Dashboard "Locker disarmed." |
| T4 | Armed: click **VIBRATION** once | `[VIB] 1 of 3`, no alarm, state stays 1 |
| T5 | Armed: click **VIBRATION** 3 times quickly (< 2 s) | Red LED, beeping buzzer, Field 1 = 2, Field 5 goes up, email sent |
| T6 | Armed: slide **TILT** to the right | Red LED, continuous tone, Field 1 = 3, Field 4 = 1 |
| T7 | During a tamper alarm, slide TILT right | `[STATE] TAMPER -> THEFT`, dashboard alert changes to Theft |
| T8 | After an alarm, stop all input; click BOOT | Alarm continues; BOOT prints `Already armed. Disarm with the PIN…` |
| T9 | Trigger two alarms within 10 s | Second upload waits for the 15 s gap, no `entry 0` |
| T10 | Not possible in Wokwi (Wi-Fi cannot be switched off) | Test on the real hardware |
| T12 | Dashboard: **Arm** + PIN | `[TALKBACK] command #… received`, then "Locker armed." on the dashboard |
| T14 | Type `DISARM 0000` five times | `commands locked for 5 min` |
| T15 / T16 | Trigger theft with the dashboard open | Red banner and siren on the dashboard; email in the owner's inbox |

The other new tests (T11, T13, T17, T18) are in [`../SETUP.md`](../SETUP.md#6-test-plan).

**Tip:** disarm and slide TILT back to the left before arming again. If TILT is still to the right when the 10 s exit delay ends, the system goes straight to THEFT.

---

## 4. Moving back to the real hardware

Use the ready-made hardware sketch in `../arduino/LockerAlarm.ino` (setup steps in `../arduino/README.md`), with the same keys and PIN.
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
| `[TALKBACK] poll failed, HTTP 401` | Wrong `TB_ID` or `TB_KEY` |
| Stuck on `Connecting to Wi-Fi....` | SSID must be exactly `Wokwi-GUEST` with an empty password |
| No sound | Turn on audio in Wokwi (speaker icon), or raise `volume` on the buzzer in diagram.json |
| One click counts as 2 vibrations | Button bounce. Add `"bounce": "0"` to `btnVib` attrs |
