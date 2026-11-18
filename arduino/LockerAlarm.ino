// Smart Anti-Theft Locker Alarm System - ESP32 (Arduino IDE, board: ESP32 Dev Module)

#include <WiFi.h>
#include <HTTPClient.h>

// Wi-Fi (2.4 GHz network or phone hotspot)
const char* WIFI_SSID  = "YOUR_WIFI_NAME";
const char* WIFI_PASS  = "YOUR_WIFI_PASSWORD";

// ThingSpeak Write API key (channel "Smart Locker Alarm")
// Fields: 1 state | 2 armed | 3 vibrations | 4 tilt | 5 total alarms
const char* TS_API_KEY = "YOUR_WRITE_API_KEY";

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
const unsigned long TS_MIN_GAP     = 15000;  // ThingSpeak free-tier minimum
const unsigned long WIFI_BOOT_WAIT = 15000;

enum State { DISARMED = 0, ARMED = 1, TAMPER = 2, THEFT = 3 };
const char* STATE_NAME[] = { "DISARMED", "ARMED", "TAMPER", "THEFT" };
State state = DISARMED;

bool armed = false;
unsigned long armTime = 0;

const int VIB_COUNT = 3;
unsigned long vibTimes[VIB_COUNT] = {0};
int  vibIdx = 0;
bool lastVib = false;
unsigned long lastVibEvent = 0;

bool tiltRaw = false;
unsigned long tiltChangeTime = 0;

int  vibSinceUpload = 0;
bool tilt = false;
int  totalAlarms = 0;

unsigned long lastUpload = 0;
bool pendingUpload = true;
bool wifiWasConnected = false;

bool lastBtn = HIGH;
unsigned long lastBtnTime = 0;

void buzz(bool on) {
  digitalWrite(PIN_BUZZ, on ? BUZZ_ON : !BUZZ_ON);
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

void setup() {
  Serial.begin(115200);
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
  Serial.println("=== Smart Anti-Theft Locker Alarm ===");
  Serial.printf("Sensor levels at start: VIB=%d  TILT=%d (box should be level)\n",
                digitalRead(PIN_VIB), digitalRead(PIN_TILT));

  digitalWrite(PIN_GREEN, HIGH); digitalWrite(PIN_RED, HIGH); buzz(true);
  delay(150);
  digitalWrite(PIN_GREEN, LOW);  digitalWrite(PIN_RED, LOW);  buzz(false);

  tiltRaw = digitalRead(PIN_TILT) == TILT_ACTIVE;
  tilt = tiltRaw;

  startWiFi();
  Serial.println("Press BOOT to arm (10 s exit delay).");
}

void handleButton() {
  bool b = digitalRead(PIN_BTN);
  if (b == LOW && lastBtn == HIGH && millis() - lastBtnTime > 300) {
    lastBtnTime = millis();
    armed = !armed;
    if (armed) {
      armTime = millis();
      for (int i = 0; i < VIB_COUNT; i++) vibTimes[i] = 0;
      Serial.println("[BTN] Arming in 10 s... close the locker");
    } else {
      state = DISARMED;
      Serial.println("[BTN] Disarmed - alarm cleared");
    }
    pendingUpload = true;
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

void updateState() {
  bool exitDelay = armed && (millis() - armTime < ARM_DELAY);
  readTilt();
  bool tamperHit = checkTamper(armed && !exitDelay);

  if (!armed || exitDelay) { state = DISARMED; return; }

  State next = (state == DISARMED) ? ARMED : state;
  if (tilt)                              next = THEFT;
  else if (tamperHit && state < TAMPER)  next = TAMPER;

  if (next != state) {
    if (next >= TAMPER) totalAlarms++;
    Serial.printf("[STATE] %s -> %s%s\n", STATE_NAME[state], STATE_NAME[next],
                  next >= TAMPER ? "   *** ALARM ***" : "");
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

void uploadIfDue() {
  unsigned long t = millis();
  bool periodic = t - lastUpload >= UPLOAD_PERIOD;
  bool urgent   = pendingUpload && (t - lastUpload >= TS_MIN_GAP);
  if (!periodic && !urgent) return;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    lastUpload = t;
    pendingUpload = true;
    return;
  }

  // ThingSpeak update URL
  String url = "http://api.thingspeak.com/update?api_key=" + String(TS_API_KEY) +
               "&field1=" + String((int)state) +
               "&field2=" + String(armed ? 1 : 0) +
               "&field3=" + String(vibSinceUpload) +
               "&field4=" + String(tilt ? 1 : 0) +
               "&field5=" + String(totalAlarms);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.begin(url);
  int code = http.GET();
  String body = code > 0 ? http.getString() : String("");
  http.end();
  lastUpload = millis();

  Serial.printf("[CLOUD] state=%d armed=%d vib=%d tilt=%d alarms=%d -> HTTP %d, entry %s\n",
                (int)state, armed ? 1 : 0, vibSinceUpload, tilt ? 1 : 0, totalAlarms,
                code, body.c_str());

  if (code == 200 && body != "0") {
    pendingUpload = false;
    vibSinceUpload = 0;
  } else if (code == 200) {
    Serial.println("[CLOUD] ThingSpeak returned 0: wrong Write API key or < 15 s since last update");
  } else {
    Serial.println("[CLOUD] upload failed, will retry");
  }
}

void loop() {
  handleButton();
  updateState();
  updateOutputs();
  reportWiFiChanges();
  uploadIfDue();
  delay(10);
}
