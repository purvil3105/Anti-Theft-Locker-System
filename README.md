# Smart Anti-Theft Locker Alarm System

ESP32-based IoT anti-theft alarm for lockers (IoT Laboratory project, ECE, IIIT Kota).
An SW-420 vibration sensor detects forcing (Tamper) and an SW-520D tilt sensor detects lifting (Theft).
A buzzer and LEDs give a local alarm, and the ESP32 reports in real time over **MQTT** through the **EMQX Cloud** broker.

**Features:**
- a **web dashboard** where the owner logs in with a **4-digit PIN** and **arms / disarms** the locker from anywhere;
- **instant alerts on the dashboard** (banner, siren, browser notification) when the locker reports Tamper or Theft, about 1 s after the event;
- an **email alert** to the owner (2023kuec2054@iiitkota.ac.in) through the ThingSpeak Alerts API;
- the **BOOT button and the dashboard stay in sync**: both change the same state on the ESP32, which publishes it at once;
- FreeRTOS network tasks on the ESP32's second core, so cloud traffic never pauses the alarm.

**Live dashboard:** https://mohitanuragi.github.io/smart-locker-dashboard/
(try it without hardware: https://mohitanuragi.github.io/smart-locker-dashboard/?demo, PIN 1234)

Start with **[`ARDUINO_SETUP.md`](ARDUINO_SETUP.md)** (Arduino IDE, code, wiring, fitting the sensors in the box) and **[`SETUP.md`](SETUP.md)** (EMQX Cloud, ThingSpeak and the dashboard).

| Folder / file | Contents |
|---|---|
| [`ARDUINO_SETUP.md`](ARDUINO_SETUP.md) | Step-by-step build: Arduino IDE and libraries, code settings, circuit connection, sensor positions inside the box, calibration |
| [`SETUP.md`](SETUP.md) | Assumptions, MQTT topics, EMQX Cloud setup, ThingSpeak (email + history), dashboard setup, test plan |
| [`dashboard/`](dashboard) | Owner web dashboard (static HTML/JS, MQTT over secure WebSocket). Open `dashboard/index.html?demo` to try it without hardware (PIN 1234) |
| [`arduino/`](arduino) | Firmware for the real ESP32 (Arduino IDE) |
| [`wokwi/`](wokwi) | Wokwi simulation: sketch, `diagram.json` circuit, `libraries.txt` |
| `results/` | Screenshots from testing (dashboard, Wokwi Serial Monitor) |
| `Smart_Anti-Theft_Locker_Alarm_Report.pdf` / `.docx` | Project report |
| `Locker_Alarm_Presentation_Final.pptx` | Presentation (version 1) |
| `Locker_Alarm_Instruction_and_Setup_Manual.pdf` | Instruction and setup manual (version 1; see the two setup guides above) |
| `Locker_Alarm_Demo_Guide.mp4` | Demo guide video (version 1) |
| `components list.pdf` | Lab components list |

Before uploading either sketch, install the **PubSubClient** library and fill in the settings block at the top:
- Wi-Fi (real board only);
- the EMQX address and the **device** user;
- the ThingSpeak Alerts API key (and the Write API key, for history graphs);
- your 4-digit PIN.

Never commit real keys or passwords.
