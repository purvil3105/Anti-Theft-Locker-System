// Smart Anti-Theft Locker Alarm System - ESP32 (Arduino IDE, board: ESP32 Dev Module)
// Version 2: PIN-protected web dashboard (arm / disarm), dashboard alerts and email alerts.
//
// Core 1 (Arduino loop): sensors, state machine, buzzer, LEDs, BOOT button, commands.
//                        It never waits for the network, so the alarm always reacts at once.
// Core 0 (netTask):      HTTPS requests to ThingSpeak:
//                          - channel update every 20 s, and soon after any change
//                          - TalkBack poll every 5 s for ARM / DISARM commands from the dashboard
//                          - email alert through the ThingSpeak Alerts API when an alarm starts

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ======================== Settings: edit these ========================
// Wi-Fi (2.4 GHz network or phone hotspot)
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ThingSpeak channel "Smart Locker Alarm": Write API key
// Fields: 1 state | 2 armed | 3 vibrations | 4 tilt | 5 total alarms
//         6 last command result | 7 last command id | 8 email status
const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";

// ThingSpeak TalkBack (Apps > TalkBack): carries commands from the web dashboard
const char* TB_ID  = "YOUR_TALKBACK_ID";
const char* TB_KEY = "YOUR_TALKBACK_API_KEY";

// ThingSpeak Alerts API key (Account > My Profile).
// The email goes to the address the ThingSpeak account is registered with.
const char* ALERTS_KEY = "YOUR_ALERTS_API_KEY";

// Owner's 4-digit PIN. Use the same PIN when you set up the web dashboard.
const char* OWNER_PIN = "1234";

// false = BOOT only arms; disarming needs the PIN (web dashboard or Serial Monitor)
// true  = BOOT arms and disarms without a PIN (offline lab testing only)
#define BOOT_CAN_DISARM false
// ======================================================================

#define PIN_BTN    0    // on-board BOOT button
#define PIN_VIB   27
#define PIN_TILT  26
#define PIN_BUZZ  18
#define PIN_GREEN 19
#define PIN_RED   21

// Flip to LOW if a sensor or the buzzer works the wrong way round
#define VIB_ACTIVE  HIGH
#define TILT_ACTIVE HIGH
#define BUZZ_ON     HIGH

const unsigned long ARM_DELAY      = 10000;
const unsigned long VIB_WINDOW     = 2000;
const unsigned long VIB_DEBOUNCE   = 100;
const unsigned long TILT_CONFIRM   = 150;
const unsigned long UPLOAD_PERIOD  = 20000;
const unsigned long TS_MIN_GAP     = 15000;   // ThingSpeak free-tier minimum
const unsigned long TB_POLL_PERIOD = 5000;    // how often to check for dashboard commands
const unsigned long WIFI_BOOT_WAIT = 15000;
const unsigned long WIFI_RETRY     = 15000;
const unsigned long HTTP_TIMEOUT   = 6000;
const int           PIN_MAX_TRIES  = 5;
const unsigned long PIN_LOCKOUT    = 300000;  // 5 min lock after 5 wrong PINs
const long          CMD_MAX_AGE    = 120;     // s; older dashboard commands are ignored
const long          TZ_OFFSET_SEC  = 19800;   // IST (UTC+5:30), used for email timestamps
const unsigned long MAIL_RETRY_WAIT = 30000;  // after a network error
const unsigned long MAIL_RATE_WAIT  = 300000;  // after HTTP 429 (limit: 2 alerts per 30 min)

enum State { DISARMED = 0, ARMED = 1, TAMPER = 2, THEFT = 3 };
const char* STATE_NAME[] = { "DISARMED", "ARMED", "TAMPER", "THEFT" };

// Field 6: result of the last dashboard command
enum CmdResult { CMD_NONE = 0, CMD_ARMED = 1, CMD_DISARMED = 2, CMD_WRONG_PIN = 3,
                 CMD_LOCKED = 4, CMD_EXPIRED = 5, CMD_INVALID = 6 };
const char* CMD_NAME[] = { "none", "armed", "disarmed", "wrong PIN", "locked out", "expired", "invalid" };

// Field 8: email alert status
enum MailStatus { MAIL_NONE = 0, MAIL_PENDING = 1, MAIL_SENT = 2, MAIL_RETRY = 3, MAIL_OFF = 4 };

// ---------- Shared between the alarm loop and netTask (guarded by `lock`) ----------
SemaphoreHandle_t lock;
#define LOCK()   xSemaphoreTake(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(lock)

State state = DISARMED;
bool  armed = false;
bool  tilt = false;
int   totalAlarms = 0;
int   vibSinceUpload = 0;
bool  pendingUpload = true;
int   cmdResult = CMD_NONE;
unsigned long cmdId = 0;
char  statusMsg[120] = "Started";

bool  mailWanted = false;
State mailState = DISARMED;
int   mailCount = 0;
char  mailTime[32] = "";
int   mailStatus = MAIL_NONE;

// Commands from netTask (TalkBack) to the alarm loop
struct Command { char text[64]; unsigned long id; long ageSec; };
QueueHandle_t cmdQueue;

// ---------- Used only by the alarm loop ----------
unsigned long armTime = 0;

const int VIB_COUNT = 3;
unsigned long vibTimes[VIB_COUNT] = {0};
int  vibIdx = 0;
bool lastVib = false;
unsigned long lastVibEvent = 0;

bool tiltRaw = false;
unsigned long tiltChangeTime = 0;

int  wrongPins = 0;
bool lockedOut = false;
unsigned long lockoutStart = 0;

bool lastBtn = HIGH;
unsigned long lastBtnTime = 0;
bool wifiWasConnected = false;
char serialLine[64];
int  serialLen = 0;

bool isSet(const char* v) { return v[0] != 0 && strncmp(v, "YOUR_", 5) != 0; }

void buzz(bool on) {
  digitalWrite(PIN_BUZZ, on ? BUZZ_ON : !BUZZ_ON);
}

// Human-readable status: printed, and sent to ThingSpeak as the entry's status text
void setStatus(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(statusMsg, sizeof(statusMsg), fmt, args);
  va_end(args);
  Serial.printf("[STATUS] %s\n", statusMsg);
}

void timeText(char* out, size_t n) {
  time_t t = time(nullptr);
  if (t < 1700000000) {                       // clock not set yet (no NTP)
    snprintf(out, n, "%lu s after power-on", millis() / 1000);
    return;
  }
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%d-%m-%Y %H:%M:%S IST", &tmv);
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_BOOT_WAIT) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" connected, IP ");
    Serial.println(WiFi.localIP());
    wifiWasConnected = true;
  } else {
    Serial.println(" not connected yet; alarm works offline and keeps retrying");
  }
}

// ======================= Alarm loop (core 1) =======================

void arm(const char* by) {
  if (armed) { setStatus("Already armed (%s)", by); return; }
  armed = true;
  armTime = millis();
  for (int i = 0; i < VIB_COUNT; i++) vibTimes[i] = 0;
  if (tilt) setStatus("Arming in 10 s (%s) - WARNING: locker is tilted, level it", by);
  else      setStatus("Arming in 10 s (%s) - close the locker", by);
  pendingUpload = true;
}

void disarm(const char* by) {
  bool wasAlarm = state >= TAMPER;
  armed = false;
  state = DISARMED;
  setStatus(wasAlarm ? "Disarmed by %s - alarm cleared" : "Disarmed by %s", by);
  pendingUpload = true;
}

// Text is "ARM 1234" or "DISARM 1234" (":" also accepted). ageSec < 0 = age unknown.
int runCommand(const char* text, long ageSec, const char* source) {
  char buf[64], verb[16] = "", pin[16] = "";
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  for (char* p = buf; *p; p++) {
    if (*p == ':' || *p == ',' || *p == '\t') *p = ' ';
    *p = toupper(*p);
  }
  sscanf(buf, "%15s %15s", verb, pin);
  bool isArm = strcmp(verb, "ARM") == 0;
  bool isDisarm = strcmp(verb, "DISARM") == 0;

  if (!isArm && !isDisarm) {
    setStatus("Unknown command from %s", source);
    return CMD_INVALID;
  }
  if (ageSec > CMD_MAX_AGE) {
    setStatus("Ignored %s from %s: sent %ld s ago (expired)", verb, source, ageSec);
    return CMD_EXPIRED;
  }
  if (lockedOut) {
    unsigned long left = (PIN_LOCKOUT - (millis() - lockoutStart)) / 1000;
    setStatus("%s from %s refused: PIN locked for %lu s more", verb, source, left);
    return CMD_LOCKED;
  }
  if (strlen(pin) != 4 || strcmp(pin, OWNER_PIN) != 0) {
    wrongPins++;
    if (wrongPins >= PIN_MAX_TRIES) {
      wrongPins = 0;
      lockedOut = true;
      lockoutStart = millis();
      setStatus("Wrong PIN %d times - commands locked for %lu min", PIN_MAX_TRIES, PIN_LOCKOUT / 60000);
      return CMD_LOCKED;
    }
    setStatus("Wrong PIN from %s (%d of %d tries)", source, wrongPins, PIN_MAX_TRIES);
    return CMD_WRONG_PIN;
  }

  wrongPins = 0;
  if (isArm) { arm(source); return CMD_ARMED; }
  disarm(source);
  return CMD_DISARMED;
}

void handleCommands() {
  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
    cmdResult = runCommand(c.text, c.ageSec, "dashboard");
    cmdId = c.id;
    pendingUpload = true;
    Serial.printf("[CMD] dashboard command #%lu -> %s\n", c.id, CMD_NAME[cmdResult]);
  }
}

// Serial Monitor commands (offline fallback): ARM 1234 | DISARM 1234 | STATUS
void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n' && serialLen < (int)sizeof(serialLine) - 1) { serialLine[serialLen++] = ch; continue; }
    if (ch != '\n') continue;
    serialLine[serialLen] = 0;
    serialLen = 0;
    if (serialLine[0] == 0) continue;
    if (strcasecmp(serialLine, "STATUS") == 0) {
      Serial.printf("[INFO] state=%s armed=%d tilt=%d alarms=%d wifi=%s | %s\n", STATE_NAME[state], armed,
                    tilt, totalAlarms, WiFi.status() == WL_CONNECTED ? "up" : "down", statusMsg);
      continue;
    }
    int r = runCommand(serialLine, -1, "Serial Monitor");
    Serial.printf("[CMD] Serial command -> %s\n", CMD_NAME[r]);
    pendingUpload = true;
  }
}

void handleButton() {
  bool b = digitalRead(PIN_BTN);
  if (b == LOW && lastBtn == HIGH && millis() - lastBtnTime > 300) {
    lastBtnTime = millis();
    if (!armed)               arm("BOOT button");
    else if (BOOT_CAN_DISARM) disarm("BOOT button");
    else Serial.println("[BTN] Already armed. Disarm with the PIN: web dashboard, or type DISARM <pin> here");
  }
  lastBtn = b;
}

void readTilt() {
  bool r = digitalRead(PIN_TILT) == TILT_ACTIVE;
  if (r != tiltRaw) { tiltRaw = r; tiltChangeTime = millis(); }
  if (tiltRaw != tilt && millis() - tiltChangeTime >= TILT_CONFIRM) {
    tilt = tiltRaw;
    Serial.printf("[TILT] %s\n", tilt ? "tilted / lifted" : "level");
  }
}

// Tamper = 3 vibrations within 2 s
bool checkTamper(bool active) {
  bool v = digitalRead(PIN_VIB) == VIB_ACTIVE;
  bool hit = false;
  unsigned long now = millis();
  if (v && !lastVib && now - lastVibEvent > VIB_DEBOUNCE) {
    lastVibEvent = now;
    if (!active) {
      Serial.println("[VIB] pulse (ignored: not armed)");
    } else {
      vibSinceUpload++;
      vibTimes[vibIdx] = now;
      vibIdx = (vibIdx + 1) % VIB_COUNT;
      unsigned long oldest = vibTimes[vibIdx];
      int inWindow = 0;
      for (int i = 0; i < VIB_COUNT; i++)
        if (vibTimes[i] != 0 && now - vibTimes[i] <= VIB_WINDOW) inWindow++;
      Serial.printf("[VIB] vibration (%d of %d in last 2 s)\n", inWindow, VIB_COUNT);
      if (oldest != 0 && now - oldest <= VIB_WINDOW) hit = true;
    }
  }
  lastVib = v;
  return hit;
}

// Queue an email for netTask. Alarms that happen before it is sent are merged into one email.
void requestEmail(State s) {
  if (!isSet(ALERTS_KEY)) { mailStatus = MAIL_OFF; return; }
  if (!mailWanted) { timeText(mailTime, sizeof(mailTime)); mailState = s; mailCount = 0; }
  if (s > mailState) mailState = s;
  mailCount++;
  mailWanted = true;
  mailStatus = MAIL_PENDING;
}

void updateState() {
  bool exitDelay = armed && (millis() - armTime < ARM_DELAY);
  readTilt();
  bool tamperHit = checkTamper(armed && !exitDelay);

  if (!armed || exitDelay) { state = DISARMED; return; }

  State next = (state == DISARMED) ? ARMED : state;
  if (tilt)                              next = THEFT;
  else if (tamperHit && state < TAMPER)  next = TAMPER;

  if (next != state) {
    Serial.printf("[STATE] %s -> %s%s\n", STATE_NAME[state], STATE_NAME[next],
                  next >= TAMPER ? "   *** ALARM ***" : "");
    if (next >= TAMPER) {
      totalAlarms++;
      requestEmail(next);
      setStatus(next == THEFT ? "THEFT alarm: locker tilted or lifted" : "TAMPER alarm: locker being forced");
    } else {
      setStatus("Armed - locker protected");
    }
    state = next;
    pendingUpload = true;
  }
}

void updateOutputs() {
  unsigned long t = millis();
  bool exitDelay = armed && (t - armTime < ARM_DELAY);
  switch (state) {
    case DISARMED:
      digitalWrite(PIN_GREEN, exitDelay ? (t / 250) % 2 : LOW);
      digitalWrite(PIN_RED, LOW);
      buzz(false);
      break;
    case ARMED:
      digitalWrite(PIN_GREEN, HIGH);
      digitalWrite(PIN_RED, LOW);
      buzz(false);
      break;
    case TAMPER:
      digitalWrite(PIN_GREEN, LOW);
      digitalWrite(PIN_RED, HIGH);
      buzz((t / 300) % 2);
      break;
    case THEFT:
      digitalWrite(PIN_GREEN, LOW);
      digitalWrite(PIN_RED, HIGH);
      buzz(true);
      break;
  }
}

void reportWiFiChanges() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wifiWasConnected) {
    wifiWasConnected = connected;
    if (connected) {
      Serial.print("[NET] Wi-Fi connected, IP ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("[NET] Wi-Fi lost - local alarm still active, reconnecting");
    }
  }
}

// ======================= Network task (core 0) =======================

String urlEncode(const char* s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (; *s; s++) {
    char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else if (c == ' ') out += '+';
    else { out += '%'; out += hex[(c >> 4) & 15]; out += hex[c & 15]; }
  }
  return out;
}

// Minimal JSON reader for ThingSpeak's flat replies: returns the value of "key", or "" if missing/null
String jsonField(const String& json, const char* key) {
  String pat = String("\"") + key + "\":";
  int i = json.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  if (i >= (int)json.length()) return "";
  if (json[i] == '"') {
    int j = json.indexOf('"', i + 1);
    return j < 0 ? "" : json.substring(i + 1, j);
  }
  int j = i;
  while (j < (int)json.length() && json[j] != ',' && json[j] != '}') j++;
  String v = json.substring(i, j);
  v.trim();
  return v == "null" ? "" : v;
}

long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Age in seconds of a TalkBack command from its created_at ("2026-11-24T05:08:16Z"); -1 if unknown
long commandAge(const String& iso) {
  time_t now = time(nullptr);
  int Y, M, D, h, m, s;
  if (now < 1700000000 || sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return -1;
  long offset = 0;
  const char* z = iso.c_str() + 19;
  while (*z == '.' || isdigit((unsigned char)*z)) z++;
  if (*z == '+' || *z == '-') {
    int oh = 0, om = 0;
    sscanf(z + 1, "%d:%d", &oh, &om);
    offset = (oh * 3600L + om * 60L) * (*z == '-' ? -1 : 1);
  }
  long long created = (long long)daysFromCivil(Y, M, D) * 86400LL + h * 3600L + m * 60L + s - offset;
  long age = (long)((long long)now - created);
  return age < 0 ? 0 : age;
}

void pollTalkBack(WiFiClientSecure& tls) {
  static int lastCode = 200;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT);
  http.setTimeout(HTTP_TIMEOUT);
  http.begin(tls, String("https://api.thingspeak.com/talkbacks/") + TB_ID + "/commands/execute.json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(String("api_key=") + TB_KEY);
  String resp = code > 0 ? http.getString() : String("");
  http.end();

  if (code != 200) {
    if (code != lastCode)
      Serial.printf("[TALKBACK] poll failed, HTTP %d%s\n", code,
                    code == 401 || code == 404 ? " (check TB_ID and TB_KEY)" : "");
    lastCode = code;
    return;
  }
  lastCode = 200;

  String text = jsonField(resp, "command_string");
  if (text.length() == 0) return;              // queue empty: ThingSpeak returns {}

  Command c;
  strncpy(c.text, text.c_str(), sizeof(c.text) - 1);
  c.text[sizeof(c.text) - 1] = 0;
  c.id = strtoul(jsonField(resp, "id").c_str(), nullptr, 10);
  c.ageSec = commandAge(jsonField(resp, "created_at"));
  Serial.printf("[TALKBACK] command #%lu received (age %ld s)\n", c.id, c.ageSec);
  xQueueSend(cmdQueue, &c, 0);
}

void uploadChannel(WiFiClientSecure& tls) {
  LOCK();
  int st = state, ar = armed, vib = vibSinceUpload, ti = tilt, al = totalAlarms;
  int cr = cmdResult, ms = mailStatus;
  unsigned long ci = cmdId;
  String msg = urlEncode(statusMsg);
  pendingUpload = false;                       // set again by any change during the upload
  UNLOCK();

  String body = String("api_key=") + TS_WRITE_KEY +
                "&field1=" + String(st) + "&field2=" + String(ar) + "&field3=" + String(vib) +
                "&field4=" + String(ti) + "&field5=" + String(al) + "&field6=" + String(cr) +
                "&field7=" + String(ci) + "&field8=" + String(ms) + "&status=" + msg;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT);
  http.setTimeout(HTTP_TIMEOUT);
  http.begin(tls, "https://api.thingspeak.com/update");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String("");
  http.end();
  resp.trim();

  Serial.printf("[CLOUD] state=%d armed=%d vib=%d tilt=%d alarms=%d cmd=%d/#%lu mail=%d -> HTTP %d, entry %s\n",
                st, ar, vib, ti, al, cr, ci, ms, code, resp.c_str());

  bool ok = code == 200 && resp != "0" && resp.length() > 0;
  LOCK();
  if (ok) {
    vibSinceUpload -= vib;
    if (vibSinceUpload < 0) vibSinceUpload = 0;
  } else {
    pendingUpload = true;
  }
  UNLOCK();
  if (!ok && code == 200) Serial.println("[CLOUD] ThingSpeak returned 0: wrong Write API key or < 15 s since last update");
  else if (!ok)           Serial.println("[CLOUD] upload failed, will retry");
}

// Returns the delay before the next email attempt (0 = nothing to retry)
unsigned long sendEmail(WiFiClientSecure& tls) {
  LOCK();
  if (!mailWanted) { UNLOCK(); return 0; }
  State s = mailState;
  int n = mailCount, total = totalAlarms;
  char when[32];
  strncpy(when, mailTime, sizeof(when));
  mailWanted = false;                          // taken; put back below if sending fails
  mailCount = 0;
  UNLOCK();

  String subject = s == THEFT ? "LOCKER ALERT: THEFT - locker lifted or tilted"
                              : "LOCKER ALERT: TAMPER - locker being forced";
  String body = String("Smart Anti-Theft Locker Alarm\\n\\n") +
                "Alarm: " + (s == THEFT ? "THEFT (locker tilted, lifted or carried away)"
                                        : "TAMPER (repeated vibration: forcing or hammering)") + "\\n" +
                "First detected: " + when + "\\n" +
                "Alarms in this alert: " + String(n) + "\\n" +
                "Total alarms since power-on: " + String(total) + "\\n\\n" +
                "The buzzer keeps sounding until the system is disarmed. "
                "Open the web dashboard to check the locker and disarm it with your PIN.";
  String json = "{\"subject\":\"" + subject + "\",\"body\":\"" + body + "\"}";

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT);
  http.setTimeout(HTTP_TIMEOUT);
  http.begin(tls, "https://api.thingspeak.com/alerts/send");
  http.addHeader("ThingSpeak-Alerts-API-Key", ALERTS_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  int code = http.POST(json);
  http.end();

  bool ok = code == 202 || code == 200;
  LOCK();
  if (ok) {
    if (!mailWanted) mailStatus = MAIL_SENT;
  } else {
    strncpy(mailTime, when, sizeof(mailTime));  // keep the time of the first alarm
    if (!mailWanted || s > mailState) mailState = s;
    mailCount += n;
    mailWanted = true;
    mailStatus = MAIL_RETRY;
  }
  pendingUpload = true;
  UNLOCK();

  if (ok) {
    Serial.printf("[EMAIL] %s alert accepted by ThingSpeak (HTTP %d)\n", STATE_NAME[s], code);
    return 0;
  }
  if (code == 429) {
    Serial.println("[EMAIL] ThingSpeak limit reached (2 alerts per 30 min) - retrying in 5 min");
    return MAIL_RATE_WAIT;
  }
  Serial.printf("[EMAIL] failed, HTTP %d%s - retrying in 30 s\n", code,
                code == 401 ? " (check ALERTS_KEY)" : "");
  return MAIL_RETRY_WAIT;
}

void netTask(void*) {
  WiFiClientSecure tls;
  tls.setInsecure();   // lab simplification: encrypted, but the server certificate is not checked
  unsigned long lastUpload = 0, lastPoll = 0, lastWiFiRetry = 0, mailBlockedAt = 0, mailWait = 0;
  bool tbOn = isSet(TB_ID) && isSet(TB_KEY);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWiFiRetry >= WIFI_RETRY) { lastWiFiRetry = millis(); WiFi.reconnect(); }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (tbOn && millis() - lastPoll >= TB_POLL_PERIOD) {
      lastPoll = millis();
      pollTalkBack(tls);
    }

    if (millis() - mailBlockedAt >= mailWait) {
      unsigned long w = sendEmail(tls);
      if (w) { mailBlockedAt = millis(); mailWait = w; }
    }

    LOCK();
    bool pend = pendingUpload;
    UNLOCK();
    unsigned long since = millis() - lastUpload;
    if (since >= UPLOAD_PERIOD || (pend && since >= TS_MIN_GAP)) {
      uploadChannel(tls);
      lastUpload = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================== Setup ==============================

void setup() {
  Serial.begin(115200);
  lock = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(4, sizeof(Command));

  pinMode(PIN_BTN,  INPUT_PULLUP);
  pinMode(PIN_VIB,  INPUT);
  pinMode(PIN_TILT, INPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_RED,   OUTPUT);
  buzz(false);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_RED, LOW);

  delay(200);
  Serial.println();
  Serial.println("=== Smart Anti-Theft Locker Alarm v2 (web dashboard + email alerts) ===");
  Serial.printf("Sensor levels at start: VIB=%d  TILT=%d (box should be level)\n",
                digitalRead(PIN_VIB), digitalRead(PIN_TILT));

  digitalWrite(PIN_GREEN, HIGH); digitalWrite(PIN_RED, HIGH); buzz(true);
  delay(150);
  digitalWrite(PIN_GREEN, LOW);  digitalWrite(PIN_RED, LOW);  buzz(false);

  tiltRaw = digitalRead(PIN_TILT) == TILT_ACTIVE;
  tilt = tiltRaw;

  if (!isSet(TS_WRITE_KEY))         Serial.println("[CONFIG] TS_WRITE_KEY not set: cloud uploads will fail");
  if (!isSet(TB_ID) || !isSet(TB_KEY)) Serial.println("[CONFIG] TalkBack not set: dashboard arm/disarm is off");
  if (!isSet(ALERTS_KEY))           Serial.println("[CONFIG] ALERTS_KEY not set: email alerts are off");
  if (strlen(OWNER_PIN) != 4)       Serial.println("[CONFIG] OWNER_PIN must be exactly 4 digits");

  startWiFi();
  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");   // clock for command age and email time

  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);

  Serial.println("Arm: press BOOT or use the web dashboard (10 s exit delay).");
  Serial.println("Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.");
}

void loop() {
  LOCK();
  if (lockedOut && millis() - lockoutStart >= PIN_LOCKOUT) {
    lockedOut = false;
    setStatus("PIN lockout over - commands accepted again");
  }
  handleButton();
  handleSerial();
  handleCommands();
  updateState();
  UNLOCK();

  updateOutputs();
  reportWiFiChanges();
  delay(10);
}
