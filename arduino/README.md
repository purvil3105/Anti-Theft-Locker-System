# Smart Anti-Theft Locker Alarm: firmware for the real ESP32

`LockerAlarm.ino` is version 3 of the firmware: real-time MQTT through EMQX Cloud, PIN-protected arm/disarm from the web dashboard, and email alerts through ThingSpeak.

**Full step-by-step instructions are in [`../ARDUINO_SETUP.md`](../ARDUINO_SETUP.md):** Arduino IDE and library install, the settings to fill in, uploading, wiring, sensor placement inside the box and calibration.

## Quick reference

| | |
|---|---|
| Board | **ESP32 Dev Module** (esp32 by Espressif Systems) |
| Extra library | **PubSubClient** by Nick O'Leary (Library Manager) |
| Serial Monitor | 115200 baud, line ending **Newline** |
| Serial commands | `ARM <pin>`, `DISARM <pin>`, `STATUS` |
| Arm | BOOT button, web dashboard, or Serial (10 s exit delay) |
| Disarm | Web dashboard or Serial, **with the PIN** (BOOT doesn't disarm unless `BOOT_CAN_DISARM` is `true`) |

## How the code is organised

| Part | Runs on | Does |
|---|---|---|
| `loop()` | Core 1 | BOOT button, Serial commands, dashboard commands, sensors, state machine, buzzer and LEDs |
| `netTask` | Core 0 | MQTT: connects to EMQX (TLS 8883), receives `locker/cmd`, publishes the retained `locker/state` at once on any change (and every 30 s), `locker/ack`, and `locker/status` (`online`, with `offline` as the last will) |
| `httpTask` | Core 0 | HTTPS to ThingSpeak: the email alert (Alerts API), and history uploads (every 60 s, or after a change) |

## Serial Monitor messages

| Prefix | Meaning |
|---|---|
| `[STATUS]` | Event text; also sent to the dashboard (`msg` in `locker/state`) |
| `[STATE]` | State change; `*** ALARM ***` for Tamper or Theft |
| `[VIB]` / `[TILT]` | Sensor events |
| `[MQTT]` | Broker connection and received commands (the PIN is never printed) |
| `[CMD]` | Result of a command: armed, disarmed, wrong PIN, locked out, invalid |
| `[EMAIL]` | Email alert accepted, rate-limited (retry in 5 min) or failed (retry in 30 s) |
| `[HISTORY]` | ThingSpeak history upload and its entry number |
| `[NET]` | Wi-Fi lost or reconnected |
| `[CONFIG]` | A setting is still `YOUR_...`, so that feature is off |

## Differences from the Wokwi sketch

| | Wokwi (`../wokwi/sketch.ino`) | Real board (this sketch) |
|---|---|---|
| Wi-Fi | `Wokwi-GUEST`, channel 6 | Your SSID and password |
| Buzzer | `tone()` for Wokwi's passive buzzer | `digitalWrite` for the lab's active buzzer module |
| Vibration input | `INPUT_PULLDOWN` (push-button) | `INPUT` (the SW-420 drives the line) |
| Tilt | Must stay tilted 20 ms | Must stay tilted 150 ms (ignores ball-switch chatter) |

Everything else is identical.
