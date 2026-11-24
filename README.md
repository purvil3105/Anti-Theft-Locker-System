# Smart Anti-Theft Locker Alarm System

ESP32-based IoT anti-theft alarm for lockers (IoT Laboratory project, ECE, IIIT Kota).
An SW-420 vibration sensor detects forcing (Tamper) and an SW-520D tilt sensor detects lifting (Theft).
A buzzer and LEDs give a local alarm, and the ESP32 reports to ThingSpeak over HTTPS.

**Version 2 adds:**
- a **web dashboard** where the owner logs in with a **4-digit PIN** and **arms / disarms** the locker from anywhere;
- **alerts on the dashboard** (banner, siren, browser notification) when the locker reports Tamper or Theft;
- an **email alert** to the owner (2023kuec2054@iiitkota.ac.in) through the ThingSpeak Alerts API;
- a FreeRTOS network task on the ESP32's second core, so cloud requests never pause the alarm.

**Live dashboard:** https://mohitanuragi.github.io/smart-locker-dashboard/
(try it without hardware: https://mohitanuragi.github.io/smart-locker-dashboard/?demo, PIN 1234)

Start with **[`SETUP.md`](SETUP.md)**. It lists the assumptions and the complete ThingSpeak, firmware and dashboard setup.

| Folder / file | Contents |
|---|---|
| [`SETUP.md`](SETUP.md) | Assumptions, ThingSpeak setup (channel, TalkBack, Alerts API), firmware and dashboard setup, test plan |
| [`dashboard/`](dashboard) | Owner web dashboard (static HTML/JS). Open `dashboard/index.html?demo` to try it without hardware (PIN 1234) |
| [`arduino/`](arduino) | Firmware for the real ESP32 (Arduino IDE) and the setup, calibration and test guide |
| [`wokwi/`](wokwi) | Wokwi simulation: sketch, `diagram.json` circuit, and guide |
| `Smart_Anti-Theft_Locker_Alarm_Report.pdf` / `.docx` | Project report (updated for version 2) |
| `Locker_Alarm_Presentation_Final.pptx` | Presentation |
| `Locker_Alarm_Instruction_and_Setup_Manual.pdf` | Instruction and setup manual (version 1; see `SETUP.md` for version 2) |
| `Locker_Alarm_Demo_Guide.mp4` | Demo guide video (version 1) |
| `components list.pdf` | Lab components list |

Before uploading either sketch, fill in the settings block at the top: Wi-Fi (real board only), the ThingSpeak Write API key, the TalkBack ID and key, the Alerts API key and your 4-digit PIN.
