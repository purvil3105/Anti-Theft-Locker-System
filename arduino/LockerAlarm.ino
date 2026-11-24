// Smart Anti-Theft Locker Alarm - ESP32 (Arduino IDE, board: ESP32 Dev Module)
// Version 3: MQTT real-time control. BOOT button and web dashboard stay in sync.
//
// Library needed: "PubSubClient" by Nick O'Leary (Library Manager).
//
// Core 1 (Arduino loop): sensors, state machine, buzzer, LEDs, BOOT button, commands.
//                        Never waits for the network, so the alarm always reacts at once.
// Core 0 netTask:        MQTT only (EMQX Cloud, TLS 8883). Publishes the retained state
//                        the moment anything changes and receives ARM / DISARM commands.
// Core 0 httpTask:       ThingSpeak over HTTPS: email alert when an alarm starts,
//                        and an optional history upload for graphs.
//
// MQTT topics:
//   locker/state   ESP32 -> everyone, retained JSON (single source of truth)
//   locker/status  ESP32 -> everyone, retained "online" / "offline" (last will)
//   locker/ack     ESP32 -> everyone, result of each dashboard command
//   locker/cmd     dashboard -> ESP32, text "ARM 1234" or "DISARM 1234" (never retained)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <time.h>
#include <stdarg.h>

// ======================== Settings: edit these ========================
// Wi-Fi (2.4 GHz network or phone hotspot)
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// EMQX Cloud deployment (Serverless). Address: Deployment Overview. User: Access Control >
// Authentication. Use the DEVICE user here, not the web (dashboard) user.
const char* MQTT_HOST = "YOUR_DEPLOYMENT.emqxsl.com";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "YOUR_DEVICE_USERNAME";
const char* MQTT_PASS = "YOUR_DEVICE_PASSWORD";

// ThingSpeak is now used only for the email alert (and optional history graphs).
// Alerts API key: ThingSpeak > Account > My Profile. The email goes to your ThingSpeak account address.
const char* ALERTS_KEY = "YOUR_ALERTS_API_KEY";

// Optional history graphs. Set TS_HISTORY to false to skip them completely.
#define TS_HISTORY true
// Channel fields: 1 state | 2 armed | 3 vibrations | 4 tilt | 5 total alarms
//                 6 last command result | 7 last command id | 8 email status
const char* TS_WRITE_KEY = "YOUR_WRITE_API_KEY";

// Owner's 4-digit PIN. You type the same PIN in the web dashboard.
const char* OWNER_PIN = "1234";

// false = BOOT only arms; disarming needs the PIN (dashboard or Serial Monitor)
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

const char* T_STATE  = "locker/state";
const char* T_STATUS = "locker/status";
const char* T_ACK    = "locker/ack";
const char* T_CMD    = "locker/cmd";

const unsigned long ARM_DELAY       = 10000;
const unsigned long VIB_WINDOW      = 2000;
const unsigned long VIB_DEBOUNCE    = 100;
const unsigned long TILT_CONFIRM    = 150;
const unsigned long HEARTBEAT       = 30000;   // state is re-published at least this often
const unsigned long MQTT_RETRY      = 5000;
const unsigned long UPLOAD_PERIOD   = 60000;   // ThingSpeak history
const unsigned long TS_MIN_GAP      = 15000;   // ThingSpeak free-tier minimum
const unsigned long WIFI_BOOT_WAIT  = 15000;
const unsigned long WIFI_RETRY      = 15000;
const unsigned long HTTP_TIMEOUT    = 6000;
const int           PIN_MAX_TRIES   = 5;
const unsigned long PIN_LOCKOUT     = 300000;  // 5 min lock after 5 wrong PINs
const long          TZ_OFFSET_SEC   = 19800;   // IST (UTC+5:30), used for email timestamps
const unsigned long MAIL_RETRY_WAIT = 30000;   // after a network error
const unsigned long MAIL_RATE_WAIT  = 300000;  // after HTTP 429 (limit: 2 alerts per 30 min)

enum State { DISARMED = 0, ARMED = 1, TAMPER = 2, THEFT = 3 };
const char* STATE_NAME[] = { "DISARMED", "ARMED", "TAMPER", "THEFT" };

enum CmdResult { CMD_NONE = 0, CMD_ARMED = 1, CMD_DISARMED = 2, CMD_WRONG_PIN = 3,
                 CMD_LOCKED = 4, CMD_INVALID = 5 };
const char* CMD_NAME[] = { "none", "armed", "disarmed", "wrong PIN", "locked out", "invalid" };

// Email status
enum MailStatus { MAIL_NONE = 0, MAIL_PENDING = 1, MAIL_SENT = 2, MAIL_RETRY = 3, MAIL_OFF = 4 };

// ---------- Shared between the alarm loop and the network tasks (guarded by `lock`) ----------
SemaphoreHandle_t lock;
#define LOCK()   xSemaphoreTake(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(lock)

State state = DISARMED;
bool  armed = false;
bool  tilt = false;
int   totalAlarms = 0;
int   vibTotal = 0;           // shown on the dashboard
int   vibSinceUpload = 0;     // ThingSpeak history
bool  pendingPublish = true;  // MQTT state needs publishing
bool  pendingUpload = true;   // ThingSpeak history needs an upload
int   cmdResult = CMD_NONE;
unsigned long cmdId = 0;
char  statusMsg[120] = "Started";
unsigned long armTime = 0;
volatile bool mqttUp = false;

bool  mailWanted = false;
State mailState = DISARMED;
int   mailCount = 0;
char  mailTime[32] = "";
int   mailStatus = MAIL_NONE;

struct Command { char text[64]; unsigned long id; };
struct Ack     { unsigned long id; int code; };
QueueHandle_t cmdQueue;   // netTask -> alarm loop
QueueHandle_t ackQueue;   // alarm loop -> netTask

// Call with `lock` held. One place that says "something changed, tell everyone".
void markChanged() {
  pendingPublish = true;
  pendingUpload = true;
}

// ---------- Used only by the alarm loop ----------
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

// Human-readable status: printed, and shown on the dashboard
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
  markChanged();
}

void disarm(const char* by) {
  bool wasAlarm = state >= TAMPER;
  armed = false;
  state = DISARMED;
  setStatus(wasAlarm ? "Disarmed by %s - alarm cleared" : "Disarmed by %s", by);
  markChanged();
}

// Text is "ARM 1234" or "DISARM 1234" (":" also accepted).
int runCommand(const char* text, const char* source) {
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
    cmdResult = runCommand(c.text, "dashboard");
    cmdId = c.id;
    Ack a = { c.id, cmdResult };
    xQueueSend(ackQueue, &a, 0);
    markChanged();
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
      Serial.printf("[INFO] state=%s armed=%d tilt=%d alarms=%d wifi=%s mqtt=%s | %s\n", STATE_NAME[state],
                    armed, tilt, totalAlarms, WiFi.status() == WL_CONNECTED ? "up" : "down",
                    mqttUp ? "up" : "down", statusMsg);
      continue;
    }
    int r = runCommand(serialLine, "Serial Monitor");
    Serial.printf("[CMD] Serial command -> %s\n", CMD_NAME[r]);
    markChanged();
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
    markChanged();
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
      vibTotal++;
      vibTimes[vibIdx] = now;
      vibIdx = (vibIdx + 1) % VIB_COUNT;
      unsigned long oldest = vibTimes[vibIdx];
      int inWindow = 0;
      for (int i = 0; i < VIB_COUNT; i++)
        if (vibTimes[i] != 0 && now - vibTimes[i] <= VIB_WINDOW) inWindow++;
      Serial.printf("[VIB] vibration (%d of %d in last 2 s)\n", inWindow, VIB_COUNT);
      if (oldest != 0 && now - oldest <= VIB_WINDOW) hit = true;
      markChanged();
    }
  }
  lastVib = v;
  return hit;
}

// Queue an email for httpTask. Alarms that happen before it is sent are merged into one email.
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
    markChanged();
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

// ======================= MQTT task (core 0) =======================

WiFiClientSecure tlsMqtt;
PubSubClient mqtt(tlsMqtt);
unsigned long cmdSeq = 0;

// Runs inside mqtt.loop(): hand the command to the alarm loop. The PIN is never printed.
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  if (strcmp(topic, T_CMD) != 0) return;
  Command c;
  size_t n = len < sizeof(c.text) - 1 ? len : sizeof(c.text) - 1;
  memcpy(c.text, payload, n);
  c.text[n] = 0;
  c.id = ++cmdSeq;
  Serial.printf("[MQTT] command #%lu received\n", c.id);
  xQueueSend(cmdQueue, &c, 0);
}

void jsonSafe(char* s) {
  for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 32) *s = ' ';
}

void publishState() {
  LOCK();
  int st = state, ar = armed, ti = tilt, al = totalAlarms, vt = vibTotal, ms = mailStatus;
  unsigned long left = 0;
  if (armed && millis() - armTime < ARM_DELAY) left = (ARM_DELAY - (millis() - armTime) + 999) / 1000;
  char msg[120];
  strncpy(msg, statusMsg, sizeof(msg) - 1);
  msg[sizeof(msg) - 1] = 0;
  pendingPublish = false;                      // set again by any change during the publish
  UNLOCK();
  jsonSafe(msg);

  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"state\":%d,\"name\":\"%s\",\"armed\":%d,\"exitLeft\":%lu,\"tilt\":%d,"
           "\"vib\":%d,\"alarms\":%d,\"mail\":%d,\"rssi\":%d,\"up\":%lu,\"msg\":\"%s\"}",
           st, STATE_NAME[st], ar, left, ti, vt, al, ms, WiFi.RSSI(), millis() / 1000, msg);
  if (!mqtt.publish(T_STATE, buf, true)) {
    Serial.println("[MQTT] state publish failed, will retry");
    LOCK(); pendingPublish = true; UNLOCK();
  }
}

void publishAcks() {
  Ack a;
  while (xQueueReceive(ackQueue, &a, 0) == pdTRUE) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"id\":%lu,\"code\":%d,\"result\":\"%s\"}", a.id, a.code, CMD_NAME[a.code]);
    mqtt.publish(T_ACK, buf, false);
  }
}

bool mqttConnect() {
  char cid[32];
  snprintf(cid, sizeof(cid), "locker-%06X", (unsigned)(ESP.getEfuseMac() >> 24));
  Serial.printf("[MQTT] connecting to %s as %s ... ", MQTT_HOST, cid);
  // clean session = true, so commands sent while the device was offline are never replayed
  bool ok = mqtt.connect(cid, MQTT_USER, MQTT_PASS, T_STATUS, 1, true, "offline", true);
  if (!ok) {
    Serial.printf("failed, rc=%d (-2 network/TLS, 4 or 5 = wrong user/password)\n", mqtt.state());
    return false;
  }
  Serial.println("connected");
  mqtt.publish(T_STATUS, "online", true);
  mqtt.subscribe(T_CMD, 1);
  LOCK(); pendingPublish = true; UNLOCK();     // publish the real state straight away
  return true;
}

void netTask(void*) {
  tlsMqtt.setInsecure();   // lab simplification: encrypted, but the server certificate is not checked
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(768);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  unsigned long lastTry = 0, lastPublish = 0, lastWiFiRetry = 0;
  bool firstTry = true;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      mqttUp = false;
      if (millis() - lastWiFiRetry >= WIFI_RETRY) { lastWiFiRetry = millis(); WiFi.reconnect(); }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (!mqtt.connected()) {
      if (mqttUp) { mqttUp = false; Serial.println("[MQTT] connection lost, reconnecting"); }
      if (firstTry || millis() - lastTry >= MQTT_RETRY) {
        firstTry = false;
        lastTry = millis();
        if (mqttConnect()) { mqttUp = true; lastPublish = millis(); }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    mqtt.loop();
    publishAcks();

    LOCK();
    bool pend = pendingPublish;
    UNLOCK();
    if (pend || millis() - lastPublish >= HEARTBEAT) {
      publishState();
      lastPublish = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ======================= HTTP task (core 0): email + history =======================

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

  Serial.printf("[HISTORY] state=%d armed=%d vib=%d alarms=%d -> HTTP %d, entry %s\n",
                st, ar, vib, al, code, resp.c_str());

  bool ok = code == 200 && resp != "0" && resp.length() > 0;
  LOCK();
  if (ok) {
    vibSinceUpload -= vib;
    if (vibSinceUpload < 0) vibSinceUpload = 0;
  } else {
    pendingUpload = true;
  }
  UNLOCK();
  if (!ok && code == 200) Serial.println("[HISTORY] ThingSpeak returned 0: wrong Write API key or < 15 s since last update");
  else if (!ok)           Serial.println("[HISTORY] upload failed, will retry");
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
  markChanged();
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

void httpTask(void*) {
  WiFiClientSecure tls;
  tls.setInsecure();   // lab simplification: encrypted, but the server certificate is not checked
  unsigned long lastUpload = 0, mailBlockedAt = 0, mailWait = 0;
  bool historyOn = TS_HISTORY && isSet(TS_WRITE_KEY);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    if (millis() - mailBlockedAt >= mailWait) {
      unsigned long w = sendEmail(tls);
      if (w) { mailBlockedAt = millis(); mailWait = w; }
      else   { mailWait = 0; }
    }

    if (historyOn) {
      LOCK();
      bool pend = pendingUpload;
      UNLOCK();
      unsigned long since = millis() - lastUpload;
      if (since >= UPLOAD_PERIOD || (pend && since >= TS_MIN_GAP)) {
        uploadChannel(tls);
        lastUpload = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ============================== Setup ==============================

void setup() {
  Serial.begin(115200);
  lock = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(4, sizeof(Command));
  ackQueue = xQueueCreate(8, sizeof(Ack));

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
  Serial.println("=== Smart Anti-Theft Locker Alarm v3 (MQTT real-time + email alerts) ===");
  Serial.printf("Sensor levels at start: VIB=%d  TILT=%d (box should be level)\n",
                digitalRead(PIN_VIB), digitalRead(PIN_TILT));

  digitalWrite(PIN_GREEN, HIGH); digitalWrite(PIN_RED, HIGH); buzz(true);
  delay(150);
  digitalWrite(PIN_GREEN, LOW);  digitalWrite(PIN_RED, LOW);  buzz(false);

  tiltRaw = digitalRead(PIN_TILT) == TILT_ACTIVE;
  tilt = tiltRaw;

  bool mqttOk = isSet(MQTT_HOST) && isSet(MQTT_USER) && isSet(MQTT_PASS);
  if (!mqttOk)                Serial.println("[CONFIG] MQTT_HOST / MQTT_USER / MQTT_PASS not set: dashboard control is off");
  if (!isSet(ALERTS_KEY))     Serial.println("[CONFIG] ALERTS_KEY not set: email alerts are off");
  if (TS_HISTORY && !isSet(TS_WRITE_KEY)) Serial.println("[CONFIG] TS_WRITE_KEY not set: history graphs are off");
  if (strlen(OWNER_PIN) != 4) Serial.println("[CONFIG] OWNER_PIN must be exactly 4 digits");

  startWiFi();
  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");   // clock for email time

  if (mqttOk) xTaskCreatePinnedToCore(netTask, "mqtt", 20480, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(httpTask, "http", 20480, nullptr, 1, nullptr, 0);

  Serial.println("Arm: press BOOT or use the web dashboard (10 s exit delay).");
  Serial.println("Disarm: web dashboard with the PIN, or type  DISARM <pin>  here. Type STATUS for a summary.");
}

void loop() {
  LOCK();
  if (lockedOut && millis() - lockoutStart >= PIN_LOCKOUT) {
    lockedOut = false;
    setStatus("PIN lockout over - commands accepted again");
    markChanged();
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
