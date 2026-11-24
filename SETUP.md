# Setup guide: MQTT cloud, email alerts and web dashboard (version 3)

This guide sets up the cloud side of the Smart Anti-Theft Locker Alarm: the **EMQX Cloud MQTT broker**, **ThingSpeak** (email alert and optional history graphs) and the owner's **web dashboard**.
For the Arduino IDE, code settings, wiring and fitting the sensors inside the box, see **[`ARDUINO_SETUP.md`](ARDUINO_SETUP.md)**.

| Section | What it covers |
|---|---|
| [1. Assumptions](#1-assumptions) | Everything the design takes for granted |
| [2. How version 3 works](#2-how-version-3-works) | Data flow, MQTT topics, timings, why MQTT |
| [3. EMQX Cloud setup](#3-emqx-cloud-setup-mqtt-broker) | Deployment, address, ports, users |
| [4. ThingSpeak setup](#4-thingspeak-setup-email-alert-and-history) | Account email, Alerts API key, history channel |
| [5. Web dashboard setup](#5-web-dashboard-setup) | Opening or hosting the page, first-run form, PIN |
| [6. Test plan](#6-test-plan) | Tests T1 to T18 |
| [7. Troubleshooting](#7-troubleshooting) | Common problems |
| [8. Security notes](#8-security-notes) | What protects what |

---

## 1. Assumptions

### Accounts and email
- **A1.** Alert emails go to **2023kuec2054@iiitkota.ac.in**. The ThingSpeak Alerts API only emails the address the ThingSpeak (MathWorks) account is registered with, so **that ThingSpeak account must use this address**.
- **A2.** One **EMQX Cloud** account holds the MQTT broker: a Serverless deployment, which the free tier covers for this project's traffic.
- **A3.** The owner can receive mail from MathWorks at the institute address. Check the spam folder the first time.

### Network
- **A4.** The locker has a **2.4 GHz** Wi-Fi network or phone hotspot with internet access.
- **A5.** The network allows outgoing **TCP 8883** (MQTT over TLS) for the ESP32, and **TCP 443 / 8084** (HTTPS and secure WebSocket) for the ESP32 and the browser. Some campus networks block 8883; a phone hotspot avoids that.
- **A6.** The owner's phone or laptop needs internet access, but does **not** need to be on the same Wi-Fi as the locker.

### Protocol and cloud
- **A7.** **MQTT replaces HTTP** for everything time-critical: locker state, alerts on the dashboard and arm/disarm commands. EMQX Cloud is the broker.
- **A8.** **ThingSpeak stays only for two things that need HTTP:**
  - the **email alert** through the Alerts API, limited to **2 emails per 30 minutes**;
  - optional **history graphs**: one upload every 60 s, or sooner after a change, never less than 15 s apart.
- **A9.** The ESP32 and the dashboard connect to the broker with **two different users**: a device user and a web user.

### Hardware
- **A10.** The hardware and pins are unchanged (report Table 4). The PIN is entered on the dashboard; there is no keypad. The unit is powered over USB, with no battery backup.

### Time
- **A11.** The ESP32 sets its clock from NTP for the email timestamp, shown in **IST (UTC+5:30)**. If NTP is blocked, the email shows "N s after power-on" instead.

### Security
- **A12.** The 4-digit PIN is set in two places that must match: `OWNER_PIN` in the firmware and the PIN chosen in the dashboard's first-run form. The **ESP32 checks the PIN on every command** and blocks commands for 5 minutes after 5 wrong PINs.
- **A13.** **BOOT only arms.** Disarming needs the PIN, from the dashboard or by typing `DISARM <pin>` in the Serial Monitor. `BOOT_CAN_DISARM true` is for offline lab tests only.
- **A14.** Traffic is encrypted with TLS, but the ESP32 does not verify the server certificate (`setInsecure()`). This is a laboratory simplification; see section 8.
- **A15.** Broker credentials and ThingSpeak keys live only in the firmware and in the browser's local storage. They are never committed to the repository or shown in screenshots.

### Behaviour
- **A16.** "Theft or buzzer" alerts mean the two alarm states: **Tamper** (3 vibrations within 2 s, beeping buzzer) and **Theft** (tilt or lift, continuous buzzer).
- **A17.** The dashboard keeps the **last 120 state updates** in the browser for its timeline and history lists. Long-term history is in the ThingSpeak channel.

---

## 2. How version 3 works

```
 SW-420 ─┐
 SW-520D ┼─► ESP32 ── Wi-Fi ──► EMQX Cloud broker ◄── MQTT over secure WebSocket (8084) ──► Web dashboard
 BOOT ───┘   │  ▲   MQTT/TLS 8883   (locker/state, /status, /ack ◄── ESP32;  locker/cmd ◄── dashboard)
 buzzer ◄────┤  │
 LEDs ◄──────┘  └── HTTPS ──► ThingSpeak: Alerts API (email) + channel (history graphs)
```

- **Core 1** runs the alarm: sensors, state machine, buzzer and LEDs. It never waits for the network.
- **Core 0** runs two tasks:
  - an **MQTT task**: keeps the broker connection, receives commands and publishes the state the moment it changes;
  - an **HTTP task**: sends the email and uploads history to ThingSpeak.

### MQTT topics

| Topic | Direction | Content | Retained | QoS |
|---|---|---|---|---|
| `locker/state` | ESP32 → all | JSON state (below), published on every change and every 30 s | yes | 0 |
| `locker/status` | ESP32 → all | `online`, or `offline` (the broker's **last will**, sent if the ESP32 drops off) | yes | 1 (will) |
| `locker/ack` | ESP32 → all | `{"id":3,"code":2,"result":"disarmed"}`, the result of each command | no | 0 |
| `locker/cmd` | dashboard → ESP32 | `ARM 1234` or `DISARM 1234` | **never** | 1 |

`locker/state` message: `{"state":3,"name":"THEFT","armed":1,"exitLeft":0,"tilt":1,"vib":3,"alarms":3,"mail":1,"rssi":-58,"up":812,"msg":"THEFT alarm: locker tilted or lifted"}`.
In this message, `state` is 0 Disarmed, 1 Armed, 2 Tamper, 3 Theft, and `mail` is 0 none, 1 sending, 2 sent, 3 retrying, 4 not set up.

Because `locker/state` is **retained**, a dashboard that opens later gets the current state at once. Both the BOOT button and the dashboard change the same state on the ESP32, so they always stay in sync.

### Timings

| Event | HTTP (version 2) | MQTT (version 3) |
|---|---|---|
| Local alarm (buzzer, red LED) | under 1 s | under 1 s (unchanged) |
| Alarm appears on the dashboard | up to ~20 s | **about 1 s** |
| Dashboard command reaches the locker | 1.5–8 s | **under 1 s** |
| Dashboard shows the locker's confirmation | up to ~20 s | **about 1 s** |
| BOOT press shows on the dashboard | up to ~20 s | **about 1 s** |
| Email alert request | 1–3 s after the alarm | 1–3 s (unchanged, still HTTPS) |

### Why MQTT
With HTTP, the ESP32 had to poll ThingSpeak for commands, and ThingSpeak accepts at most one channel update every 15 s, so the dashboard could lag by 20 s. MQTT keeps one connection open in each direction: the broker pushes each message to the subscribers immediately, the messages are small, and the last will reports a dead locker without polling.

---

## 3. EMQX Cloud setup (MQTT broker)

Console labels may differ slightly as EMQX updates its site.

1. Sign up or sign in at https://www.emqx.com/en/cloud and open the **EMQX Cloud console**.
2. **Create a deployment:** choose **Serverless**, pick a region close to you (for example Asia), give it a name (for example `smart-locker`) and deploy. Wait until the status is **Running**.
3. Open the deployment's **Overview** and note:

   | Value | Used by |
   |---|---|
   | **Address** (looks like `xxxxxxxx.ala.<region>.emqxsl.com`) | Firmware `MQTT_HOST` and the dashboard's **Broker address** |
   | **MQTT over TLS port: 8883** | Firmware `MQTT_PORT` |
   | **WebSocket over TLS port: 8084** | The dashboard's **WebSocket (TLS) port** |

4. **Create two users:** open **Access Control → Authentication → Add**.

   | User | Example name | Used by |
   |---|---|---|
   | Device user | `locker-device` | Firmware `MQTT_USER` / `MQTT_PASS` |
   | Web user | `locker-web` | Dashboard first-run form |

   Use long, random passwords, different for each user.
5. *(Recommended)* **Restrict the topics.** Under **Access Control → Authorization**, let:
   - `locker-device` publish to `locker/state`, `locker/status` and `locker/ack`, and subscribe to `locker/cmd`;
   - `locker-web` subscribe to `locker/state`, `locker/status` and `locker/ack`, and publish to `locker/cmd`.
6. *(Optional)* Test with any MQTT client (for example MQTTX): connect with the web user, subscribe to `locker/#` and power on the locker. A retained `locker/state` JSON and `locker/status = online` should appear.

---

## 4. ThingSpeak setup (email alert and history)

1. Sign in at https://thingspeak.mathworks.com with an account registered to **2023kuec2054@iiitkota.ac.in** (A1). In **Account → My Profile**, set the **Time Zone** to **(GMT+05:30) Kolkata**.
2. **Alerts API key:** on **Account → My Profile**, find **Alerts API Key** and click **Generate New API Key**. Copy it into the firmware's `ALERTS_KEY`. Generating a new key later invalidates the old one.
3. **History channel (optional):**
   - **Channels → New Channel**, name it `Smart Locker Alarm`, and tick and name the fields: 1 System State, 2 Armed, 3 Vibration Events, 4 Tilt Detected, 5 Total Alarms, 6 Command Result, 7 Command ID, 8 Email Status.
   - Copy the **Write API Key** into `TS_WRITE_KEY`.
   - To skip history graphs, set `#define TS_HISTORY false`.
4. On the channel's **Private View**, add charts if you want them for the report: Field 1 as a step chart and Field 3 as a column chart.

---

## 5. Web dashboard setup

The dashboard is three static files in [`dashboard/`](dashboard): `index.html`, `style.css` and `app.js`. It loads the MQTT.js library from a CDN, so it needs internet access, but no server or installation.

### 5.1 Open it
- **On a laptop:** double-click `dashboard/index.html`.
- **On any phone or computer:** publish the folder with GitHub Pages. The files contain no keys, so this is safe.
- **Without hardware:** open `dashboard/index.html?demo` for a simulated locker (PIN `1234`).

### 5.2 First-run form

| Box | Enter |
|---|---|
| Broker address | The EMQX **Address**: no `wss://`, no port |
| WebSocket (TLS) port | `8084` |
| Web username / Web password | The **web** user from section 3, step 4 |
| 4-digit PIN / Repeat PIN | The same digits as `OWNER_PIN` in the firmware |

Click **Test connection**. It should say `Broker OK. The locker is online.`, or `...the locker is offline` if it isn't powered yet. Then click **Save** and log in with the PIN. The settings are stored only in that browser; the PIN is stored as a salted SHA-256 hash.

### 5.3 Using it
- **Login:** 4-digit PIN. Five wrong PINs lock the keypad for 60 s, and the dashboard locks itself after 10 minutes without activity.
- **Arm / Disarm:** press the button and enter the PIN. The locker's answer appears in about a second ("Locker armed." or "Locker disarmed."). The buttons are disabled while the locker is offline, because a command sent then would be lost.
- **Alerts:** a red banner, a siren and a browser notification. **Silence sound** stops only the dashboard siren; the locker's buzzer keeps sounding until the system is disarmed.
- **Status pill:** "Locker online" while the broker reports `online` and updates keep arriving; "Locker offline" when the last will arrives or nothing has come for 90 s.

---

## 6. Test plan

| ID | Test | Procedure | Expected result |
|---|---|---|---|
| T1 | Wi-Fi and broker | Power on | `[MQTT] ... connected`; dashboard "Locker online" |
| T2 | Arm with BOOT | Press BOOT | Green LED blinks 10 s, then stays on; dashboard Arming… → Armed |
| T3 | Disarm from dashboard | Disarm + PIN | LEDs and buzzer off; "Locker disarmed."; `[CMD] dashboard command #n -> disarmed` |
| T4 | Single knock rejected | One tap while armed | `[VIB] vibration (1 of 3 ...)`; no alarm |
| T5 | Tamper | 3+ taps within 2 s | `ARMED -> TAMPER *** ALARM ***`; red LED, beeping; dashboard Tamper alert |
| T6 | Theft | Lift or tilt | `ARMED -> THEFT`; continuous buzzer; dashboard Theft alert |
| T7 | Theft priority | Tilt during a tamper alarm | `TAMPER -> THEFT`; the alert changes to Theft |
| T8 | Latching | Stop all disturbance; press BOOT | Alarm continues; `[BTN] Already armed...` |
| T9 | BOOT and dashboard in sync | Press BOOT with the dashboard open | Dashboard changes within about 1 s; Activity shows "Arming in 10 s (BOOT button)" |
| T10 | Locker offline | Stop the simulation or unplug the ESP32 | Last will `offline`; dashboard "Locker offline"; buttons disabled |
| T11 | Dashboard login lockout | 5 wrong PINs | "Wrong PIN. n tries left"; keypad locked 60 s |
| T12 | Remote arm | Arm + PIN | `[MQTT] command #n received`; "Locker armed." |
| T13 | PIN mismatch | Different `OWNER_PIN` in the firmware; Disarm | Ack code 3; "The locker rejected the PIN"; locker stays armed |
| T14 | Locker lockout | 5 wrong PIN commands (Serial `DISARM 0000` ×5) | `commands locked for 5 min`; ack code 4 |
| T15 | Email alert | Trigger theft | `[EMAIL] THEFT alert accepted by ThingSpeak (HTTP 202)`; email received |
| T16 | Email rate limit | 3 alarms within 30 min | Email tile "Retrying"; sent later |
| T17 | History upload | Leave running, or cause a change | `[HISTORY] ... HTTP 200, entry N` |
| T18 | No stale commands | Make the locker offline; reconnect it | No old command is executed (commands are never retained; clean session) |

---

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| `[CONFIG] MQTT_HOST / MQTT_USER / MQTT_PASS not set` | Fill in the EMQX values in the firmware. |
| `[MQTT] ... failed, rc=-2` | Network or TLS problem: check the address (no `mqtts://`) and port 8883, and try a phone hotspot. |
| `[MQTT] ... failed, rc=4` / `rc=5` | Wrong device username or password. |
| Dashboard: "Cannot reach the MQTT broker" | Check the address, port **8084**, web user and password, and that the page has internet access (MQTT.js loads from a CDN). |
| Dashboard always "Locker offline" | The ESP32 isn't connected to the broker; check the `[MQTT]` lines in the Serial Monitor. |
| "The locker rejected the PIN" | The dashboard PIN and `OWNER_PIN` differ. |
| `[EMAIL] failed, HTTP 401` | Wrong `ALERTS_KEY`, or it was regenerated. |
| Email accepted but not received | Look in the spam folder; check the ThingSpeak account email (A1). |
| `[HISTORY] ... entry 0` | Wrong Write API key, or less than 15 s since the last upload. |

---

## 8. Security notes

- **PIN:** every disarm is checked on the ESP32, with a 5-minute lockout after 5 wrong PINs. The dashboard's own PIN check is a convenience gate, storing only a salted hash.
- **Broker access:** the device and web users are separate and can be restricted to their topics (section 3, step 5). Anyone holding the web user's password can send commands, but still needs the PIN.
- **No replay:** commands are never retained and the ESP32 uses a clean session, so a command sent while the locker is offline is not executed later.
- **TLS:** traffic to EMQX and ThingSpeak is encrypted, but `setInsecure()` skips certificate checks. To harden it, load the broker's CA certificate with `tlsMqtt.setCACert(...)`.
- **Secrets:** don't commit real keys or passwords, and crop screenshots so the settings block isn't visible. If keys were exposed, regenerate them: the EMQX user passwords and the ThingSpeak Alerts and Write API keys.
- **Physical access:** unplugging the USB cable silences the alarm (no battery backup).
