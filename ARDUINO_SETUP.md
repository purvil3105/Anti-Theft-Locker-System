# Build guide: Arduino IDE, code setup, circuit and box assembly

This guide takes you from an empty table to a working locker alarm, one step at a time:

| Part | What you do |
|---|---|
| [1. Parts and tools](#1-parts-and-tools) | Collect everything before you start |
| [2. Arduino IDE setup](#2-arduino-ide-setup) | Install the IDE, the ESP32 board package and the PubSubClient library |
| [3. Code setup](#3-code-setup) | Fill in Wi-Fi, MQTT, ThingSpeak and PIN settings, then upload |
| [4. Circuit connection](#4-circuit-connection-step-by-step) | Wire the sensors, buzzer and LEDs on the breadboard |
| [5. Fitting it inside the box](#5-fitting-everything-inside-the-locker-box) | Where each sensor goes and why |
| [6. Calibration](#6-calibration) | Adjust the vibration sensitivity and check the tilt direction |
| [7. First power-on test](#7-first-power-on-test) | Confirm everything works end to end |
| [8. Troubleshooting](#8-troubleshooting) | Common problems and fixes |

The cloud side (EMQX Cloud broker, ThingSpeak email alert, web dashboard) is set up in [`SETUP.md`](SETUP.md). Do that first or in parallel: step 3 needs the values it gives you.

---

## 1. Parts and tools

| # | Part | Qty | Notes |
|---|---|---|---|
| 1 | ESP32 DevKit V1 (ESP32-WROOM-32, 30-pin) | 1 | Its on-board **BOOT** button is used to arm the system |
| 2 | SW-420 vibration sensor module (3 pins: VCC, GND, DO) | 1 | Has a blue potentiometer for sensitivity |
| 3 | SW-520D tilt sensor module (3 pins: VCC, GND, D0) | 1 | Ball tilt switch with comparator |
| 4 | Active buzzer module (3 pins: VCC, GND, I/O) | 1 | Beeps by itself when its I/O pin is HIGH |
| 5 | LED, green, 5 mm | 1 | "Armed" indicator |
| 6 | LED, red, 5 mm | 1 | "Alarm" indicator |
| 7 | Resistor, 220 Ω | 2 | One in series with each LED |
| 8 | Breadboard (full size) | 1 | |
| 9 | Jumper wires (male–male and male–female) | ~15 | Male–female wires plug straight onto the module pins |
| 10 | Transparent lid box | 1 | The locker model |
| 11 | Micro-USB data cable | 1 | Must be a **data** cable, not charge-only |
| 12 | Double-sided foam tape or hot-glue gun | – | To fix the sensors to the box |
| 13 | Laptop with Arduino IDE | 1 | Windows, macOS or Linux |

---

## 2. Arduino IDE setup

Do this once per laptop.

1. **Install the Arduino IDE 2.x** from https://www.arduino.cc/en/software and open it.
2. **Add the ESP32 board package address.** Open **File → Preferences**. In **Additional boards manager URLs**, paste:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   Click **OK**.
3. **Install the ESP32 boards.** Open **Tools → Board → Boards Manager**, search for `esp32` and install **esp32 by Espressif Systems**. It is a large download; wait for it to finish.
4. **Install the MQTT library.** Open **Tools → Manage Libraries** (Library Manager), search for `PubSubClient` and install **PubSubClient by Nick O'Leary**. This is the only extra library. `WiFi`, `WiFiClientSecure` and `HTTPClient` come with the ESP32 package.
5. **Connect the ESP32** with the micro-USB cable. A new COM port should appear under **Tools → Port**. If none appears, install the USB driver for your board's USB chip:
   - **CP2102**: Silicon Labs CP210x driver
   - **CH340**: WCH CH340 driver

   The chip name is printed on the small chip next to the USB socket.
6. **Select the board.** **Tools → Board → esp32 → ESP32 Dev Module**, and **Tools → Port →** the new COM port. Leave the other Tools settings at their defaults.

---

## 3. Code setup

1. Open `arduino/LockerAlarm.ino`. The IDE asks to move it into a folder named `LockerAlarm`; click **OK**.
2. Edit the **settings block** at the top of the file. You get the values from [`SETUP.md`](SETUP.md):

   ```cpp
   // Wi-Fi (2.4 GHz network or phone hotspot)
   const char* WIFI_SSID = "YOUR_WIFI_NAME";
   const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

   // EMQX Cloud deployment: address from Deployment Overview,
   // DEVICE user from Access Control > Authentication
   const char* MQTT_HOST = "YOUR_DEPLOYMENT.emqxsl.com";
   const int   MQTT_PORT = 8883;                // MQTT over TLS
   const char* MQTT_USER = "YOUR_DEVICE_USERNAME";
   const char* MQTT_PASS = "YOUR_DEVICE_PASSWORD";

   // ThingSpeak: email alert (Account > My Profile > Alerts API Key)
   const char* ALERTS_KEY = "YOUR_ALERTS_API_KEY";

   // ThingSpeak history graphs (optional): channel Write API key
   #define TS_HISTORY true
   const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";

   // Owner's 4-digit PIN: type the same PIN in the web dashboard
   const char* OWNER_PIN = "1234";

   // false = BOOT only arms (normal); true = BOOT also disarms (offline lab tests only)
   #define BOOT_CAN_DISARM false
   ```

   | Setting | Rule |
   |---|---|
   | `WIFI_SSID` / `WIFI_PASS` | The ESP32 only works on **2.4 GHz** Wi-Fi. A phone hotspot works well; on iPhone turn on **Maximise Compatibility**. Both are case-sensitive. |
   | `MQTT_HOST` | Only the address, for example `xxxxxxxx.ala.asia-southeast1.emqxsl.com`. No `mqtts://` and no port. |
   | `MQTT_USER` / `MQTT_PASS` | The **device** user. The dashboard uses a separate **web** user. |
   | `OWNER_PIN` | Exactly 4 digits. Change it from `1234`. |
   | Any value left as `YOUR_...` | That feature switches off, and the Serial Monitor says so at start-up. |

3. **Keep your keys private.** Don't commit a copy of the sketch with real passwords or keys to a public repository, and don't share screenshots of the settings block.
4. Click **Verify** (✓). It should compile without errors. If it says `PubSubClient.h: No such file`, repeat step 2.4.
5. Click **Upload** (→). If the upload stalls at `Connecting.....`, **hold the BOOT button** until the percentage starts to rise, then release it.
6. Open **Tools → Serial Monitor**, set the speed to **115200 baud** and the line ending to **Newline**. Press the **EN** (reset) button on the board. Expected output:
   ```
   === Smart Anti-Theft Locker Alarm v3 (MQTT real-time + email alerts) ===
   Sensor levels at start: VIB=0  TILT=0 (box should be level)
   Connecting to Wi-Fi "MyHotspot"....... connected, IP 192.168.43.120
   Arm: press BOOT or use the web dashboard (10 s exit delay).
   Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.
   [MQTT] connecting to xxxxxxxx.ala.asia-southeast1.emqxsl.com as locker-A1B2C3 ... connected
   ```
7. **Serial Monitor commands** (handy for tests and if the internet is down): `ARM 1234`, `DISARM 1234`, `STATUS`.

**Wokwi simulator instead of hardware:** open https://wokwi.com/projects/new/esp32. Paste `wokwi/sketch.ino` into the `sketch.ino` tab, `wokwi/diagram.json` into `diagram.json`, and `wokwi/libraries.txt` into the `libraries.txt` tab. Fill in the same settings except Wi-Fi, which is already `Wokwi-GUEST`. In the simulator, the yellow push-button is the SW-420, the slide switch is the SW-520D and the blue button is BOOT.

---

## 4. Circuit connection step by step

**Unplug the USB cable while wiring.** All modules run from the ESP32's **3.3 V (3V3)** pin. Don't use VIN (5 V) for the sensors: their outputs would then be 5 V, which can damage the ESP32's 3.3 V inputs.

### Pin plan (report Table 4)

| Device | Device pin | ESP32 pin | Wire colour (suggested) |
|---|---|---|---|
| SW-420 vibration | VCC / GND / **DO** | 3V3 / GND / **GPIO 27** | red / black / yellow |
| SW-520D tilt | VCC / GND / **D0** | 3V3 / GND / **GPIO 26** | red / black / blue |
| Buzzer module | VCC / GND / **I/O** | 3V3 / GND / **GPIO 18** | red / black / purple |
| Green LED | Anode (long leg) via 220 Ω / Cathode | **GPIO 19** / GND | green / black |
| Red LED | Anode (long leg) via 220 Ω / Cathode | **GPIO 21** / GND | orange / black |
| BOOT button | on the board | GPIO 0 | – (no wiring) |

### Steps

1. **Place the ESP32.** Push the ESP32 DevKit across the centre channel of the breadboard, so the two rows of pins sit on opposite sides of the channel. The USB socket should face the edge of the breadboard, so the cable can be plugged in later.
2. **Power rails.** Connect the ESP32 **3V3** pin to the breadboard's **+** rail (red line) and a **GND** pin to the **−** rail (blue line). Join the top and bottom rails with two short jumpers (+ to +, − to −), so both sides of the breadboard are powered.
3. **SW-420 vibration sensor.** Using male–female jumpers: **VCC → + rail**, **GND → − rail**, **DO → GPIO 27**. Use wires about 15–20 cm long, so the sensor can reach the locker door (section 5).
4. **SW-520D tilt sensor.** **VCC → + rail**, **GND → − rail**, **D0 → GPIO 26**. Also use 15–20 cm wires.
5. **Buzzer module.** **VCC → + rail**, **GND → − rail**, **I/O → GPIO 18**.
6. **Green LED (Armed).**
   1. Run a jumper from **GPIO 19** to a free breadboard column.
   2. Place a **220 Ω resistor** from that column across to a second column.
   3. Put the LED's **long leg (anode)** in the resistor's second column, and its **short leg (cathode)** in the **− rail**.
7. **Red LED (Alarm).** Repeat step 6 with **GPIO 21**.
8. **Check before powering.**
   - Every module's GND goes to the **−** rail.
   - Every module's VCC goes to **3V3**, not VIN.
   - Each LED has its resistor in series.
   - Nothing connects to GPIO 0 except the on-board button.
9. **Power on.** Plug in the USB cable. You should hear one short beep and see both LEDs flash once: that is the power-on self-test.

### Why these pins
- GPIO 18, 19 and 21 have no effect on how the board boots, so they are safe outputs.
- GPIO 26 and 27 are used only as digital inputs, which works normally while Wi-Fi is on.
- GPIO 0 (BOOT) is a strapping pin. It is read only after start-up, so **don't hold BOOT while pressing EN or plugging in USB**, or the board enters download mode.

---

## 5. Fitting everything inside the locker box

The sensors must be **fixed firmly to the box itself**, so they move and shake with it. A sensor that is loose, or that only touches the breadboard, will miss attacks or give false alarms.

```
              SIDE VIEW (box standing normally, lid on top)

         lid (transparent, opens)          LEDs visible through the lid
   ┌───────────────────────────────────────────────────────────┐
   │ [SW-420] on the inside wall, right next to the lock  ◄─ forcing the lock shakes it
   │                                                            │
   │                                        [buzzer]           │◄─ small hole for sound
   │  [SW-520D]                                                 │
   │  upright, on a side wall          ┌──────────────────────┐ │
   │  near the base (pins down)        │ breadboard + ESP32   │ │◄─ USB cable out
   │                                   │ (LEDs face the lid)  │ │   through a rear hole
   └───────────────────────────────────┴──────────────────────┴─┘
                                  floor of the box
```

| Part | Where | How | Why |
|---|---|---|---|
| **SW-420 vibration sensor** | On the **inside wall next to the lock or latch** (the front wall, or the lid's inside surface if the lock is on the lid). Within about 5 cm of the lock. | Stick the **flat back of the module** to the wall with foam tape or hot glue, so the metal spring (the silver cylinder) is pressed against the wall. | Prying, hammering or drilling the lock shakes this wall the most. A sensor glued to the wall picks it up; one hanging on its wires does not. |
| **SW-520D tilt sensor** | On a **side wall near the floor**, standing **upright**: the gold or silver tube vertical and the pins pointing **down**. | Hot glue or foam tape, square to the floor. Check with the Serial Monitor that it reads **level** (`TILT=0`) when the box stands normally. | Upright, the ball rests on the contacts. Lifting, tipping or carrying the box makes the ball roll away, so the tilt sensor reports theft. Mounted at an angle, it can trigger by itself or not at all. |
| **ESP32 + breadboard** | Flat on the **floor**, toward the back. | Double-sided tape under the breadboard. | Keeps the board stable and away from the lock area, and the USB cable leaves through the back. |
| **Buzzer** | On the floor or a side wall **near a small hole** (about 5 mm) drilled in the box. | Foam tape. | A closed box muffles the sound; a hole lets the alarm be heard. |
| **Green and red LEDs** | On the breadboard, **facing up** toward the transparent lid. | – | Visible from outside without opening the box. |
| **USB cable** | Leaves through a **small hole at the back**. | Tie a knot or use a cable tie inside the hole. | A pulled cable must not drag the breadboard and wiring with it. |

**Tips:**
- Keep the vibration sensor **away from the buzzer**. The buzzer's own vibration must not count as an attack.
- Tie the sensor wires along the walls with tape, so they don't rattle against the box.
- Don't put the vibration sensor on the floor of the box; normal handling of the table would trigger it.
- **Arming:** press BOOT (or Arm on the dashboard), then close the lid within the **10 s exit delay**. The green LED blinks during the delay and stays on once the box is armed.

---

## 6. Calibration

Do this with the box assembled, USB connected and the Serial Monitor open.

| Check | What to do | Good result |
|---|---|---|
| **Tilt direction** | Leave the box level, then tilt it about 45° and put it back. | `[TILT] tilted / lifted`, then `[TILT] level`. If it reads the other way round, change `#define TILT_ACTIVE HIGH` to `LOW` and upload again. |
| **Vibration sensitivity** | While **disarmed**, tap next to the lock. Each tap prints `[VIB] pulse (ignored: not armed)`. | A **firm** tap prints one line; a box at rest prints nothing. Turn the SW-420 potentiometer with a small screwdriver: clockwise makes it **less** sensitive on most modules. |
| **Buzzer polarity** | Watch the power-on self-test. | One short beep. If the buzzer sounds all the time and goes quiet during an alarm, your module is low-level triggered: set `#define BUZZ_ON LOW`. |
| **Vibration polarity** | Check `Sensor levels at start`. | `VIB=0` at rest. If it shows `VIB=1` and taps don't print, set `#define VIB_ACTIVE LOW`. |

---

## 7. First power-on test

1. Open the web dashboard and log in with your PIN ([`SETUP.md`](SETUP.md), section 5). The top bar should say **Locker online**.
2. Press **BOOT**. The green LED blinks for 10 s, then stays on. The dashboard shows **Arming…**, then **Armed**, within about a second.
3. Tap near the lock **3 times within 2 seconds**. The red LED lights, the buzzer beeps, and the dashboard shows a **Tamper alert**.
4. Press **Disarm** on the dashboard and enter the PIN. Everything goes off, and the dashboard says **Locker disarmed.**
5. Arm again, then **lift or tilt** the box. The buzzer sounds continuously, the dashboard shows a **Theft alert**, and an email arrives (`LOCKER ALERT: THEFT`).
6. Disarm from the dashboard.

The full test list (T1–T18) is in the report, Section 10, and in [`SETUP.md`](SETUP.md#6-test-plan).

---

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `PubSubClient.h: No such file or directory` | Install **PubSubClient by Nick O'Leary** (step 2.4). |
| No COM port | Use a data USB cable; install the CP210x or CH340 driver. |
| Upload stuck at `Connecting.....` | Hold BOOT until the upload starts. |
| Board keeps restarting | Don't hold BOOT while resetting. Check that nothing is wired to GPIO 0. |
| `Connecting to Wi-Fi......` never finishes | Use 2.4 GHz Wi-Fi; check the SSID and password spelling; move the hotspot closer. |
| `[MQTT] ... failed, rc=-2` | Network or TLS problem: check `MQTT_HOST` (address only) and `MQTT_PORT` (8883), and that the network allows port 8883. |
| `[MQTT] ... failed, rc=4` or `rc=5` | Wrong MQTT username or password. Use the **device** user from EMQX Access Control → Authentication. |
| Dashboard says **Locker offline** | The ESP32 isn't connected to the broker; check the `[MQTT]` lines in the Serial Monitor. |
| `[EMAIL] failed, HTTP 401` | Wrong `ALERTS_KEY`, or it was regenerated. |
| `[EMAIL] ThingSpeak limit reached` | Normal after more than 2 alerts in 30 minutes; it retries automatically. |
| Alarm starts as soon as the exit delay ends | The box is tilted, or `TILT_ACTIVE` is the wrong way round; check the `[TILT]` lines. |
| Too many false tamper alarms | Make the SW-420 less sensitive, fix it more firmly, and move it away from the buzzer. |
| Can't disarm without internet | Type `DISARM <pin>` in the Serial Monitor, or set `BOOT_CAN_DISARM true` for lab testing. |
