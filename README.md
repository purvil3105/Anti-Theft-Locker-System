# Smart Anti-Theft Locker Alarm System

ESP32-based IoT anti-theft alarm for lockers (IoT Laboratory project, ECE, IIIT Kota).
An SW-420 vibration sensor detects forcing (Tamper) and an SW-520D tilt sensor detects lifting (Theft).
A buzzer and LEDs give a local alarm, and the status is uploaded to ThingSpeak over HTTP.

| Folder / file | Contents |
|---|---|
| [`arduino/`](arduino) | Firmware for the real ESP32 (Arduino IDE) and setup, calibration and test guide |
| [`wokwi/`](wokwi) | Wokwi simulation: sketch, `diagram.json` circuit, and ThingSpeak setup guide |
| `Smart_Anti-Theft_Locker_Alarm_Report.pdf` / `.docx` | Project report |
| `Locker_Alarm_Presentation_Final.pptx` | Presentation |
| `Locker_Alarm_Instruction_and_Setup_Manual.pdf` | Instruction and setup manual |
| `Locker_Alarm_Demo_Guide.mp4` | Demo guide video |
| `components list.pdf` | Lab components list |

Before uploading either sketch, replace `YOUR_WRITE_API_KEY` with your ThingSpeak channel's Write API key
(and, for the real board, set your Wi-Fi name and password).
