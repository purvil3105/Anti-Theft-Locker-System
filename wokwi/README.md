# Smart Anti-Theft Locker Alarm: Wokwi simulation

Wokwi simulates the ESP32 and gives it internet access through its virtual `Wokwi-GUEST` network. That is enough for the full system: MQTT to EMQX Cloud, arm/disarm from the web dashboard, and email alerts through ThingSpeak.

| File | Paste it into |
|---|---|
| `sketch.ino` | The **sketch.ino** tab (firmware version 3, adapted for the simulator) |
| `diagram.json` | The **diagram.json** tab (the circuit) |
| `libraries.txt` | The **libraries.txt** tab (adds **PubSubClient**) |

## Steps
1. Set up EMQX Cloud and ThingSpeak first ([`../SETUP.md`](../SETUP.md), sections 3–4).
2. Open https://wokwi.com/projects/new/esp32 and sign in, so you can save the project.
3. Paste the three files into their tabs.
4. In `sketch.ino`, fill in `MQTT_HOST`, `MQTT_USER` / `MQTT_PASS` (the **device** user), `ALERTS_KEY`, `TS_WRITE_KEY` and `OWNER_PIN`. Wi-Fi is already set to `Wokwi-GUEST`.
5. Click **▶ Start**. The Serial Monitor should show `connected` for Wi-Fi and `[MQTT] ... connected`.
6. Open the web dashboard with the **web** user. It should say **Locker online**.

**Don't share screenshots that show the settings block.** It contains your broker password and API keys; crop to the simulation and the Serial Monitor.

## How the real sensors are simulated

| Real part | Wokwi part | How to use it | GPIO |
|---|---|---|---|
| SW-420 vibration sensor | Yellow push-button | Each click is one vibration pulse. Keyboard: **V** | 27 |
| SW-520D tilt sensor | Slide switch | Left = level, right = tilted/lifted | 26 |
| On-board BOOT button | Blue push-button (GPIO 0 → GND) | Click to arm (it doesn't disarm). Keyboard: **B** | 0 |
| Active buzzer module | Wokwi buzzer (passive, driven with `tone()`) | Turn on audio in the simulator | 18 |
| Green / red LED | LEDs through 220 Ω resistors | | 19 / 21 |

Type `ARM 1234`, `DISARM 1234` or `STATUS` in the Serial Monitor input box. Stopping the simulation makes the broker publish the last will `offline`, and the dashboard shows **Locker offline** (test T10).

## Moving to the real hardware
Use `../arduino/LockerAlarm.ino` with the same settings plus your Wi-Fi. The differences are listed in [`../arduino/README.md`](../arduino/README.md#differences-from-the-wokwi-sketch). For wiring and sensor placement, see [`../ARDUINO_SETUP.md`](../ARDUINO_SETUP.md).
