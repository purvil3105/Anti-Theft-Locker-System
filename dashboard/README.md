# Owner web dashboard

A static web page (no server, no build, no install) for the Smart Anti-Theft Locker Alarm:

- **4-digit PIN login**, with the keypad locked after 5 wrong tries and automatic lock after 10 minutes idle;
- **Arm / Disarm** buttons, each confirmed with the PIN, sent to the ESP32 through ThingSpeak TalkBack (the ESP32 checks the PIN again);
- **Live status** read from the ThingSpeak channel every 5 s: state, tilt, vibrations, alarm count, email status, online / offline;
- **Alerts** when the locker reports Tamper or Theft: red banner, siren, browser notification, tab title;
- **State timeline**, **alarm history** and **activity log** for about the last 40 minutes.

| File | Purpose |
|---|---|
| `index.html` | Page structure: setup form, login, dashboard, PIN dialog |
| `style.css` | Styles (light and dark, phone-friendly) |
| `app.js` | Logic: ThingSpeak and TalkBack calls, PIN hashing, alerts, demo simulator |

## Run it
- **Try it now:** open `index.html?demo`. A simulated locker follows the firmware's rules. The PIN is `1234`; demo controls simulate knocks, tilt, BOOT and Wi-Fi loss.
- **Real locker:** open `index.html` (double-click it, or publish it with GitHub Pages) and fill in the first-run form with your Channel ID, Read API key, TalkBack ID, TalkBack API key and PIN. The full steps are in [`../SETUP.md`](../SETUP.md#5-web-dashboard-setup).

Keys and the PIN hash are stored only in the browser's local storage, never in these files.
Add `?theme=light` or `?theme=dark` to the address to override the system colour scheme.
