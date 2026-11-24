# Setup guide: web dashboard, PIN control and email alerts (version 2)

This guide sets up version 2 of the Smart Anti-Theft Locker Alarm from scratch: the ThingSpeak cloud, the ESP32 firmware and the owner's web dashboard.
Section 1 lists every assumption the design makes. Read it first, because several steps depend on it (for example, which email address receives the alerts).

| Section | What it covers |
|---|---|
| [1. Assumptions](#1-assumptions) | Everything the design takes for granted |
| [2. How version 2 works](#2-how-version-2-works) | Data flow, protocol choice, timings |
| [3. ThingSpeak setup](#3-thingspeak-setup) | Account, channel, TalkBack, Alerts API key, widgets |
| [4. Firmware setup](#4-firmware-setup) | The values to paste into the sketch, upload, expected output |
| [5. Web dashboard setup](#5-web-dashboard-setup) | Opening or hosting the page, first-run form, PIN |
| [6. Test plan](#6-test-plan) | Tests T1 to T18 |
| [7. Troubleshooting](#7-troubleshooting) | Common problems |
| [8. Security notes](#8-security-notes) | What the PIN protects and what it does not |

---

## 1. Assumptions

### Accounts and email
- **A1.** Alert emails go to **2023kuec2054@iiitkota.ac.in**. The ThingSpeak Alerts API can only email the address the ThingSpeak (MathWorks) account is registered with, so **the ThingSpeak account must be created with, or changed to, 2023kuec2054@iiitkota.ac.in**. No other recipient can be configured in the firmware.
- **A2.** One ThingSpeak account holds the channel, the TalkBack and the Alerts API key. A free account or an academic (campus) licence both work.
- **A3.** The owner can receive mail from MathWorks at the institute address. Institute mail filters may move it to spam the first time.

### Network
- **A4.** The locker has a 2.4 GHz Wi-Fi network or phone hotspot with internet access. The ESP32 cannot use 5 GHz.
- **A5.** The network allows outgoing HTTPS (port 443) to `api.thingspeak.com` and NTP (UDP 123) to `pool.ntp.org`. Some campus networks block NTP; the system still works without it (see A13).
- **A6.** The owner opens the dashboard on any device with a modern browser (Chrome, Edge, Firefox, Safari) and internet access. The phone does **not** need to be on the same Wi-Fi as the locker.

### Protocol and cloud
- **A7.** **HTTP (over TLS, i.e. HTTPS) is used, not MQTT.** It needs no extra Arduino library and no broker, and it fits the existing ThingSpeak design. ThingSpeak's TalkBack app carries commands from the dashboard to the ESP32, and its Alerts API sends the email. Section 2 explains the choice.
- **A8.** ThingSpeak free-licence limits apply: at least **15 s between channel updates**, about 3 million messages a year, and **2 alert emails per 30 minutes**. The firmware stays inside these limits.
- **A9.** Uploading every 20 s, 24 hours a day, uses about 4,320 messages a day (about 1.58 million a year), within the free limit.
- **A10.** TalkBack polling (every 5 s) does not count as channel messages. ThingSpeak's licensing FAQ counts only channel writes as messages, and no TalkBack rate limit is documented. If ThingSpeak ever limits it, raise `TB_POLL_PERIOD` in the firmware.

### Hardware
- **A11.** The hardware, wiring and pins are unchanged from version 1 (report Table 4). No keypad or extra parts are added: the PIN is entered on the web dashboard.
- **A12.** The unit is powered over USB as before. There is still no battery backup.

### Time
- **A13.** The ESP32 sets its clock from NTP. Times in emails are shown in **IST (UTC+5:30)**; change `TZ_OFFSET_SEC` for another zone. Without NTP, the email shows "N s after power-on" instead of a clock time, and the 2-minute command expiry is skipped.

### Security
- **A14.** The 4-digit PIN is set in two places that must match: `OWNER_PIN` in the firmware and the PIN chosen in the dashboard's first-run form. The dashboard stores only a salted SHA-256 hash of it.
- **A15.** The ESP32 checks the PIN on **every** arm and disarm command. After 5 wrong PINs it ignores all commands for 5 minutes. The dashboard locks its keypad for 60 s after 5 wrong PINs (longer each time) and locks itself after 10 minutes without activity.
- **A16.** **Arming needs no PIN at the locker.** The BOOT button can still arm, because arming cannot help a thief. **Disarming needs the PIN**: by default the BOOT button no longer disarms (`BOOT_CAN_DISARM false`). For offline lab testing, set it to `true`, or type `DISARM <pin>` in the Serial Monitor.
- **A17.** HTTPS traffic is encrypted, but the ESP32 does not check ThingSpeak's certificate (`setInsecure()`). This is acceptable for a laboratory project; section 8 explains how to change it.
- **A18.** The ThingSpeak API keys are entered into the dashboard in the browser and stored only in that browser's local storage. They are never written into the repository, so the dashboard folder can be published (for example on GitHub Pages) without exposing them.

### Behaviour
- **A19.** "Theft or buzzer" alerts mean the two alarm states: **Tamper** (3 vibrations within 2 s, beeping buzzer) and **Theft** (tilt or lift, continuous buzzer). Each alarm, and each escalation from Tamper to Theft, raises a dashboard alert and requests one email. Alarms that occur while an email is still waiting are merged into that email.
- **A20.** The dashboard shows about the last 40 minutes (120 updates). Older history stays in the ThingSpeak channel and can be exported as CSV.

---

## 2. How version 2 works

```
 SW-420 ─┐                         HTTPS POST /update (every 20 s and after changes)
 SW-520D ┼─► ESP32 ─── Wi-Fi ───►  ThingSpeak channel (fields 1-8 + status) ──► web dashboard reads it every 5 s
 BOOT ───┘     │  ▲                                                               │
 buzzer ◄──────┤  └── HTTPS POST /talkbacks/<id>/commands/execute (every 5 s) ◄── TalkBack queue ◄── "ARM 1234" / "DISARM 1234"
 LEDs ◄────────┘
               └──── HTTPS POST /alerts/send (when an alarm starts) ──► ThingSpeak Alerts ──► email to 2023kuec2054@iiitkota.ac.in
```

- **Core 1** of the ESP32 runs the alarm exactly as in version 1: sensors, state machine, buzzer and LEDs. It never waits for the network.
- **Core 0** runs a separate FreeRTOS task for the network: channel uploads, the TalkBack poll and the email. A slow request no longer pauses the buzzer pattern or causes missed vibration pulses (version 1 limitation, now fixed).
- **Arm / disarm from the dashboard:** the owner enters the PIN, the dashboard puts `ARM 1234` or `DISARM 1234` in the TalkBack queue, the ESP32 fetches it within 5 s, checks the PIN and acts. It reports the result in fields 6 and 7, and the dashboard shows the result.
- **Alerts:** on Tamper or Theft, the ESP32 sounds the buzzer at once, sends the email through the Alerts API within a few seconds, and uploads the new state. The dashboard shows a red banner, plays a siren and shows a browser notification.

### Why HTTP and not MQTT

| | HTTP + TalkBack (chosen) | MQTT |
|---|---|---|
| Arduino libraries | None extra (`HTTPClient`, `WiFiClientSecure` come with the ESP32 core) | PubSubClient or similar |
| Dashboard in the browser | Plain `fetch()` calls; ThingSpeak allows cross-origin requests | Needs MQTT over WebSockets and a JS MQTT library |
| Commands to the ESP32 | TalkBack queue: commands wait if the locker is briefly offline | Needs a subscribed topic and an always-open connection |
| Email | Same REST style (Alerts API) | Still needs the Alerts REST API |
| Delay | Command within 5 s, dashboard view within about 20 s (15 s ThingSpeak limit) | Faster to the dashboard, but channel writes are still limited to one per 15 s |
| Fit with version 1 | Same style as the existing upload | New protocol to learn and debug |

### Timings

| Event | Delay |
|---|---|
| Local alarm (buzzer, red LED) | Under 1 s, same as version 1 |
| Email request sent by the ESP32 | 1 to 3 s after the alarm; delivery usually under a minute |
| Alert on the dashboard | Up to about 20 s (15 s ThingSpeak gap + 5 s dashboard poll) |
| Arm / disarm command reaches the ESP32 | Up to 5 s, plus 1 to 2 s for the HTTPS request |
| Dashboard shows "confirmed" | Usually under 20 s after the command |
| Command expiry | The ESP32 ignores commands more than 2 minutes old (for example, sent while it was offline) |

### Channel fields

| Field | Name | Values |
|---|---|---|
| 1 | System State | 0 Disarmed, 1 Armed, 2 Tamper, 3 Theft |
| 2 | Armed | 1 armed (including the 10 s exit delay), 0 disarmed |
| 3 | Vibration Events | Vibrations counted since the previous upload |
| 4 | Tilt Detected | 1 tilted / lifted, 0 level |
| 5 | Total Alarms | Alarms since power-on |
| 6 | Command Result | 0 none, 1 armed, 2 disarmed, 3 wrong PIN, 4 locked out, 5 expired, 6 invalid |
| 7 | Command ID | TalkBack ID of the last dashboard command the ESP32 handled |
| 8 | Email Status | 0 none, 1 sending, 2 sent, 3 retrying, 4 not configured |
| status | (entry status text) | Last event in words, for example `THEFT alarm: locker tilted or lifted` |

---

## 3. ThingSpeak setup

ThingSpeak's menus change from time to time; the labels below were correct when this guide was written and may differ slightly.

### 3.1 Account
1. Go to https://thingspeak.mathworks.com and click **Sign Up** (or **Sign In** if the account exists).
2. Use **2023kuec2054@iiitkota.ac.in** as the account email (assumption A1). If the account already exists under another email, change the email in the MathWorks account profile, or create a new account with this address.
3. Confirm the address from the verification email.
4. Open **Account → My Profile** and set **Time Zone** to **(GMT+05:30) Kolkata**, so alert timestamps and channel times are in IST.

### 3.2 Channel
1. **Channels → My Channels → New Channel**.
2. Fill in:

   | Setting | Value |
   |---|---|
   | Name | `Smart Locker Alarm` |
   | Description | `ESP32 anti-theft locker alarm (IIIT Kota IoT Lab)` |
   | Field 1 | `System State` (tick the box) |
   | Field 2 | `Armed` |
   | Field 3 | `Vibration Events` |
   | Field 4 | `Tilt Detected` |
   | Field 5 | `Total Alarms` |
   | Field 6 | `Command Result` |
   | Field 7 | `Command ID` |
   | Field 8 | `Email Status` |

   Tick all eight field boxes. If you are upgrading the version 1 channel, open **Channel Settings**, tick and name fields 6, 7 and 8, and click **Save Channel**.
3. Click **Save Channel**.
4. Note the **Channel ID** shown under the channel name.
5. Open the **API Keys** tab and note the **Write API Key** and the first **Read API Key**.
6. Leave **Sharing** on **Keep channel view private**. The dashboard reads the channel with the Read API key. (If you make the channel public, the dashboard's Read API key box can be left empty.)

### 3.3 TalkBack (commands from the dashboard)
1. **Apps → TalkBack → New TalkBack**.
2. **Name:** `Locker Commands`.
3. **Log to Channel:** leave it **empty / None**. Logging would copy every command, including the PIN, into a channel, and the extra writes would collide with the 15 s limit.
4. Click **Save TalkBack**.
5. Note the **TalkBack ID** and the TalkBack **API Key** shown on the TalkBack page. This key is different from the channel keys.

### 3.4 Alerts API key (email)
1. **Account → My Profile**.
2. Find **Alerts API Key** and click **Generate New API Key**. (Generating a new key later invalidates the old one, so update the firmware if you do.)
3. Note the key. Alerts are sent to the account email from step 3.1, limited to **2 every 30 minutes**.

### 3.5 Where each value goes

| Value from ThingSpeak | Firmware (`LockerAlarm.ino` / `sketch.ino`) | Dashboard first-run form |
|---|---|---|
| Channel ID | – | Channel ID |
| Write API Key | `TS_WRITE_KEY` | – |
| Read API Key | – | Read API key |
| TalkBack ID | `TB_ID` | TalkBack ID |
| TalkBack API Key | `TB_KEY` | TalkBack API key |
| Alerts API Key | `ALERTS_KEY` | – |
| Owner PIN (you choose) | `OWNER_PIN` | 4-digit PIN (same digits) |

### 3.6 ThingSpeak's own dashboard (optional, for the report)
The web dashboard in `dashboard/` replaces this for day-to-day use, but the ThingSpeak channel view is useful for screenshots and history. On the channel's **Private View** tab:

| Field | Edit the chart (pencil icon) or **Add Widgets** | Settings |
|---|---|---|
| 1 State | Chart: Type **step**, Results **60**, Y-axis min 0 max 3 | Title "System state (0 Disarmed, 1 Armed, 2 Tamper, 3 Theft)" |
| 1 State | Widget **Numeric Display** | Name "Current state" |
| 2 Armed | Widget **Lamp Indicator** | Condition `>= 1`, colour green |
| 3 Vibrations | Chart: Type **column**, Results **60** | Title "Vibration events per upload" |
| 4 Tilt | Widget **Lamp Indicator** | Condition `>= 1`, colour red |
| 5 Total alarms | Widget **Numeric Display** | |
| 6 Command result | Widget **Numeric Display** | Name "Last command (1 arm, 2 disarm, 3 wrong PIN)" |
| 8 Email status | Widget **Numeric Display** | Name "Email (2 = sent)" |

Field 7 needs no widget. To export the history, use **Data Import / Export → Export recent data** (CSV).

### 3.7 Check the cloud without the hardware (optional)
Paste these into a browser address bar, using your own keys. The first adds a test entry (the reply is the entry number; `0` means a wrong key or less than 15 s since the last update). The second shows the TalkBack queue.
```
https://api.thingspeak.com/update?api_key=WRITE_KEY&field1=1&field2=1&status=setup+test
https://api.thingspeak.com/talkbacks/TALKBACK_ID/commands.json?api_key=TALKBACK_KEY
```
The dashboard's **Test connection** button (section 5) checks the channel and the TalkBack for you.

---

## 4. Firmware setup

The Arduino IDE setup (ESP32 board package, driver, upload steps) is unchanged; see [`arduino/README.md`](arduino/README.md). **No new libraries are needed.**

1. Open `arduino/LockerAlarm.ino` (real board) or paste `wokwi/sketch.ino` into Wokwi.
2. Edit the settings block at the top:
   ```cpp
   const char* WIFI_SSID    = "YOUR_WIFI_NAME";        // real board only
   const char* WIFI_PASS    = "YOUR_WIFI_PASSWORD";
   const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";    // 3.2 step 5
   const char* TB_ID        = "YOUR_TALKBACK_ID";      // 3.3 step 5
   const char* TB_KEY       = "YOUR_TALKBACK_API_KEY"; // 3.3 step 5
   const char* ALERTS_KEY   = "YOUR_ALERTS_API_KEY";   // 3.4
   const char* OWNER_PIN    = "1234";                  // choose your own 4 digits
   #define BOOT_CAN_DISARM false
   ```
   Any value left as `YOUR_...` switches that feature off, and the Serial Monitor says so at start-up.
3. Upload, open the Serial Monitor at 115200 baud and press **EN**. Expected output:
   ```
   === Smart Anti-Theft Locker Alarm v2 (web dashboard + email alerts) ===
   Sensor levels at start: VIB=0  TILT=0 (box should be level)
   Connecting to Wi-Fi "MyHotspot"....... connected, IP 192.168.43.120
   Arm: press BOOT or use the web dashboard (10 s exit delay).
   Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.
   [CLOUD] state=0 armed=0 vib=0 tilt=0 alarms=0 cmd=0/#0 mail=0 -> HTTP 200, entry 1
   ```
4. Serial Monitor commands (set the line ending to **Newline**): `ARM 1234`, `DISARM 1234`, `STATUS`.

---

## 5. Web dashboard setup

The dashboard is three static files in [`dashboard/`](dashboard): `index.html`, `style.css` and `app.js`. It needs no server, no build step and no installation.

### 5.1 Open it
The dashboard is already published at **https://mohitanuragi.github.io/smart-locker-dashboard/** (demo: add `?demo`). Open that link on any phone or computer and go to 5.2.

To run your own copy instead, choose one:
- **On a laptop:** double-click `dashboard/index.html`. It opens in the browser and works straight away.
- **On any phone or computer (recommended for the demo):** publish the repository with **GitHub Pages** (repository **Settings → Pages → Deploy from a branch → main / root**). The dashboard is then at `https://<user>.github.io/Anti-Theft-Locker-System/dashboard/`. The files contain no keys (A18), so publishing them is safe.
- **To try it without hardware or ThingSpeak:** open `dashboard/index.html?demo`. This simulated locker follows the firmware's rules; the PIN is `1234`. Demo controls let you knock, tilt, press BOOT and drop the Wi-Fi.

### 5.2 First-run form
The first time the page opens in a browser, it asks for:

| Box | Enter |
|---|---|
| Channel ID | From 3.2 step 4 |
| Read API key | From 3.2 step 5 (empty if the channel is public) |
| TalkBack ID | From 3.3 step 5 |
| TalkBack API key | From 3.3 step 5 |
| 4-digit PIN / Repeat PIN | The same digits as `OWNER_PIN` in the firmware |

Click **Test connection**. It should say `Channel OK, last update … TalkBack OK.` Then click **Save** and log in with the PIN. Each browser needs this once; the settings stay in that browser only.

### 5.3 Using it
- **Login:** 4-digit PIN on the keypad (or keyboard digits).
- **Arm / Disarm:** press the button and enter the PIN again. The message under the buttons shows "Waiting for the locker to confirm…", then the locker's answer.
- **Alerts:** a red banner with the alarm type, start time and email status, a siren sound, and a browser notification. **Enable notifications** in the top bar needs an HTTPS page (GitHub Pages) in most browsers. **Silence sound** stops the dashboard siren only; the locker's buzzer keeps sounding until the system is disarmed.
- **Status pill:** "Locker online" if an update arrived in the last 60 s, otherwise "Locker offline · last seen …".
- **Settings (gear icon):** change keys or the PIN. **Lock** logs out. **Forgot PIN?** on the login screen wipes the saved settings so the form can be filled in again. The locker still requires `OWNER_PIN`.

---

## 6. Test plan

T1 to T10 are the version 1 tests (report Table 7). Note the changes in T3 and T8: disarming now uses the dashboard (or Serial), not BOOT. T11 to T18 are new.

| ID | Test | Procedure | Expected result |
|---|---|---|---|
| T1 | Wi-Fi and cloud | Power on | "connected"; first entry within about 20 s; dashboard shows "Locker online" |
| T2 | Arming (BOOT) | Press BOOT | Green LED blinks 10 s then stays on; Field 2 = 1; dashboard shows Arming…, then Armed |
| T3 | Disarming | Dashboard: Disarm + correct PIN | LEDs and buzzer off; Fields 1 and 2 = 0; dashboard shows "Locker disarmed." |
| T4 | Single knock rejected | One tap while armed | No alarm; state stays 1 |
| T5 | Tamper | 3+ taps within 2 s | Red LED + beeping; Field 1 = 2; Field 5 + 1 |
| T6 | Theft | Lift or tilt | Red LED + continuous buzzer; Field 1 = 3; Field 4 = 1 |
| T7 | Theft priority | Tilt during a tamper alarm | State 2 → 3; dashboard alert changes to Theft; second email requested |
| T8 | Latching | Stop all disturbance; press BOOT | Alarm continues; BOOT does not disarm (`[BTN] Already armed…`) |
| T9 | Upload rate limit | Two alarms within 10 s | No `entry 0` lines |
| T10 | Wi-Fi loss | Hotspot off during an alarm | Local alarm continues; dashboard shows "Locker offline"; email and uploads go out after reconnection |
| T11 | Dashboard login | Wrong PIN 5 times, then the right PIN | "Wrong PIN. n tries left"; keypad locked 60 s; correct PIN then opens the dashboard |
| T12 | Remote arm | Dashboard: Arm + PIN | `[TALKBACK] command #… received`; "Locker armed." on the dashboard; Field 6 = 1 |
| T13 | PIN mismatch | Set a different `OWNER_PIN` in the firmware; try Disarm | Locker stays armed; dashboard shows "The locker rejected the PIN…"; Field 6 = 3 |
| T14 | Locker lockout | 5 wrong PIN commands (Serial `DISARM 0000` ×5) | `commands locked for 5 min`; even the right PIN gets Field 6 = 4 until the lockout ends |
| T15 | Dashboard alert | Trigger tamper or theft with the dashboard open | Red banner, siren, notification within about 20 s; alarm listed under Alarm history |
| T16 | Email alert | Trigger theft | Email "LOCKER ALERT: THEFT…" at 2023kuec2054@iiitkota.ac.in; `[EMAIL] … accepted`; Field 8 = 2 |
| T17 | Email rate limit | Trigger 3 alarms within 30 min | Third email: `[EMAIL] ThingSpeak limit reached… retrying in 5 min`; Field 8 = 3, then 2 when sent |
| T18 | Stale command | Unplug the locker, send Arm, wait 3 min, plug it in | Locker ignores the command (`expired`); Field 6 = 5 |

Record for the report: the email delay (alarm to inbox), the dashboard alert delay, the command confirmation time, and screenshots of the dashboard during T15 and of the email from T16.

---

## 7. Troubleshooting

| Symptom | Fix |
|---|---|
| `[CONFIG] … not set` at start-up | A value in the settings block is still `YOUR_...`. |
| `[TALKBACK] poll failed, HTTP 401/404` | Wrong `TB_ID` or `TB_KEY` (TalkBack key, not a channel key). |
| Dashboard: "TalkBack refused the API key" | Same, in the dashboard settings. |
| Dashboard: "no access to the channel" | Wrong Channel ID or Read API key, or the channel is private and the Read key box is empty. |
| Dashboard always says "The locker rejected the PIN" | The dashboard PIN and `OWNER_PIN` differ. Change one so they match. |
| Commands confirmed only after about 20 s | Normal: the result is uploaded after the 15 s ThingSpeak gap. |
| Command shows "expired" | The ESP32's clock is ahead or the command waited over 2 minutes. Check that the locker is online. |
| `[EMAIL] failed, HTTP 401` | Wrong `ALERTS_KEY`, or the key was regenerated. |
| `[EMAIL] … accepted` but no email | Look in the spam/junk folder of 2023kuec2054@iiitkota.ac.in; confirm the ThingSpeak account email is that address (A1). |
| `HTTP -1` on uploads | No internet on the hotspot, or the request timed out. It retries automatically. |
| Email time shows "s after power-on" | NTP is blocked on the network (A13). Alerts still work. |
| Locker cannot be disarmed with no Wi-Fi | Use the Serial Monitor `DISARM <pin>`, or set `BOOT_CAN_DISARM true` for lab tests. |
| No sound on the dashboard | Click anywhere on the page once (browsers block sound until the page is used); check the volume. |
| "Enable notifications" missing | Notifications need HTTPS; use the GitHub Pages link. The banner and siren still work. |

---

## 8. Security notes

- **What the PIN protects:** turning the alarm off. Every disarm, from the dashboard or the Serial Monitor, is checked against `OWNER_PIN` on the ESP32, and 5 wrong PINs block commands for 5 minutes.
- **The dashboard's own PIN check** is a convenience gate in the browser. It stores a salted SHA-256 hash, not the PIN. A 4-digit PIN cannot resist an offline guess against that hash, so the real protection is the ESP32's check and lockout. Keep the TalkBack API key private.
- **Physical access:** anyone who can reach the board can still unplug the USB power, which silences the alarm (no battery backup, A12).
- **Certificate check:** `tls.setInsecure()` encrypts traffic but does not verify ThingSpeak's certificate. To harden it, replace that line with `tls.setCACert(rootCA)`, using the root certificate that `api.thingspeak.com` currently uses.
- **Replay:** commands expire after 2 minutes and the dashboard clears the TalkBack queue before each new command, so an old command cannot be replayed later.
- **Keys in the firmware:** the sketch holds the Write, TalkBack and Alerts keys. Do not commit your real keys to a public repository.
