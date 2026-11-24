# Owner web dashboard

**Live:** https://mohitanuragi.github.io/smart-locker-dashboard/ · **Demo:** https://mohitanuragi.github.io/smart-locker-dashboard/?demo (PIN 1234)

A static web page (no server, no build, no install) for the Smart Anti-Theft Locker Alarm. It connects to the **EMQX Cloud MQTT broker** over secure WebSocket (port 8084) using MQTT.js:

- **4-digit PIN login**, with the keypad locked after 5 wrong tries and automatic lock after 10 minutes idle;
- **Arm / Disarm** buttons, each confirmed with the PIN, published to `locker/cmd` (the ESP32 checks the PIN again and replies on `locker/ack`);
- **Live status** pushed by the ESP32 on the retained `locker/state` topic the moment anything changes: state, tilt, vibrations, alarm count, email status;
- **Online / offline** from `locker/status` (the ESP32's last will);
- **Alerts** when the locker reports Tamper or Theft: red banner, siren, browser notification, tab title;
- **State timeline**, **alarm history** and **activity log** for the last 120 updates (kept in the browser).

| File | Purpose |
|---|---|
| `index.html` | Page structure: setup form, login, dashboard, PIN dialog. Loads MQTT.js from a CDN |
| `style.css` | Styles (light and dark, phone-friendly) |
| `app.js` | Logic: MQTT connection and topics, PIN hashing, alerts, demo simulator |

## Run it
- **Try it now:** open `index.html?demo`. A simulated locker follows the firmware's rules. The PIN is `1234`; demo controls simulate knocks, tilt, BOOT and Wi-Fi loss.
- **Real locker:** open `index.html` (double-click it, or publish it with GitHub Pages) and fill in the first-run form:
  - the EMQX broker address;
  - the WebSocket port (`8084`);
  - the **web** username and password;
  - your PIN.

  The full steps are in [`../SETUP.md`](../SETUP.md#5-web-dashboard-setup).

The broker credentials and the PIN hash are stored only in the browser's local storage, never in these files.
Add `?theme=light` or `?theme=dark` to the address to override the system colour scheme.
