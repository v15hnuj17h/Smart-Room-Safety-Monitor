

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include "config.h"

 
constexpr int      UNO_RX_PIN        = 16;
constexpr int      UNO_TX_PIN        = 17;
constexpr int      STATUS_LED_PIN    = 2;
constexpr uint32_t UNO_BAUD          = 9600;
constexpr uint32_t USB_BAUD          = 115200;
constexpr uint32_t LINK_TIMEOUT_MS   = 5000;
constexpr bool     SHOW_RAW_LINES    = false;

constexpr uint32_t UPLOAD_LATEST_MS  = 2000;
constexpr uint32_t UPLOAD_HISTORY_MS = 10000;
constexpr uint32_t WIFI_FIRST_WAIT_MS = 15000;
constexpr uint32_t WIFI_RETRY_MS     = 10000;
constexpr time_t   MIN_VALID_EPOCH   = 1700000000;

constexpr uint32_t OCC_POLL_MS       = 2000;    
constexpr long     OCC_MAX_AGE_S     = 15;      

HardwareSerial UnoSerial(2);

 
FirebaseData   fbdo;
FirebaseAuth   fbAuth;
FirebaseConfig fbConfig;

 
struct UnoData {
  uint16_t seq;
  int      gas;
  uint8_t  state;
  bool     fan;
  uint16_t overrunS;
  bool     buzzer;
  bool     flame;
};

static UnoData  lastData{};
static bool     haveData   = false;

static char     rxBuf[96];
static size_t   rxLen = 0;

static uint32_t goodFrames = 0;
static uint32_t badFrames  = 0;
static uint32_t lastGoodMs = 0;
static uint32_t lastWarnMs = 0;
static bool     linkOk     = false;

 
static uint32_t lastLatestMs  = 0;
static uint32_t lastHistoryMs = 0;
static uint32_t lastWifiTry   = 0;
static int      reportedState = -1;
static int      reportedLink  = -1;
static int      reportedFlame = -1;
static bool     uploadedOnce  = false;

 
static int      occCount  = -1;      
static uint32_t lastOccMs = 0;

 
const char *stateName(uint8_t s) {
  switch (s) {
    case 0:  return "SAFE";
    case 1:  return "WARNING";
    case 2:  return "DANGER";
    case 3:  return "WARMUP";
    default: return "UNKNOWN";
  }
}

bool parseFrame(const char *line, UnoData &out) {
  if (line[0] != '$') return false;

  const char *star = strrchr(line, '*');
  if (star == nullptr || strlen(star) < 3) return false;

  uint8_t calc = 0;
  for (const char *p = line + 1; p < star; p++) calc ^= (uint8_t)*p;
  uint8_t received = (uint8_t)strtoul(star + 1, nullptr, 16);
  if (calc != received) return false;

  char body[80];
  size_t n = (size_t)(star - (line + 1));
  if (n == 0 || n >= sizeof(body)) return false;
  memcpy(body, line + 1, n);
  body[n] = '\0';

  unsigned seq, state, fan, overrun, buzzer, flame;
  int gas;
  if (sscanf(body, "G,%u,%d,%u,%u,%u,%u,%u",
             &seq, &gas, &state, &fan, &overrun, &buzzer, &flame) != 7) {
    return false;
  }

  out.seq        = (uint16_t)seq;
  out.gas        = gas;
  out.state      = (uint8_t)state;
  out.fan        = (fan != 0);
  out.overrunS   = (uint16_t)overrun;
  out.buzzer     = (buzzer != 0);
  out.flame      = (flame != 0);
  return true;
}

void printData(const UnoData &d) {
  Serial.printf("[#%05u] Gas: %4d | State: %-7s | Flame: %-3s | Fan: %-3s",
                d.seq, d.gas, stateName(d.state),
                d.flame ? "YES" : "no", d.fan ? "ON" : "OFF");
  if (d.fan && d.overrunS > 0) {
    Serial.printf(" (run-on %us)", d.overrunS);
  }
  Serial.printf(" | Buzzer: %s | People: ", d.buzzer ? "ON" : "OFF");
  if (occCount >= 0) Serial.printf("%d\n", occCount); else Serial.println("--");
}

 
void sendPeopleToUno() {
  char body[16];
  snprintf(body, sizeof(body), "P,%d", occCount);
  uint8_t crc = 0;
  for (const char *p = body; *p; p++) crc ^= (uint8_t)*p;
  UnoSerial.printf("$%s*%02X\n", body, (unsigned int)crc);
}

void handleLine(const char *line) {
  if (SHOW_RAW_LINES) {
    Serial.printf("RAW: %s\n", line);
  }

  UnoData data;
  if (parseFrame(line, data)) {
    goodFrames++;
    lastGoodMs = millis();
    lastData   = data;
    haveData   = true;
    if (!linkOk) {
      linkOk = true;
      Serial.println("--- Link to Arduino established ---");
    }
    printData(data);
    sendPeopleToUno();      
  } else {
    badFrames++;
    Serial.printf("Bad frame (CRC/format error): \"%s\"  [good=%lu bad=%lu]\n",
                  line, (unsigned long)goodFrames, (unsigned long)badFrames);
  }
}

void readUno() {
  while (UnoSerial.available()) {
    char c = (char)UnoSerial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      if (rxLen > 0) {
        rxBuf[rxLen] = '\0';
        handleLine(rxBuf);
      }
      rxLen = 0;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxLen = 0;
      badFrames++;
    }
  }
}

void linkWatch(uint32_t now) {
  bool timedOut = linkOk ? (now - lastGoodMs > LINK_TIMEOUT_MS)
                         : (now - lastWarnMs > LINK_TIMEOUT_MS);

  if (timedOut && (now - lastWarnMs > LINK_TIMEOUT_MS)) {
    lastWarnMs = now;
    linkOk = false;
    Serial.println("WARNING: no valid data from Arduino - check wiring, common GND and baud rate");
  }
}

 
void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void netDiag() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress ip;
  if (WiFi.hostByName("identitytoolkit.googleapis.com", ip)) {
    Serial.printf("DNS OK: identitytoolkit.googleapis.com = %s\n", ip.toString().c_str());
  } else {
    Serial.println("DNS FAILED - this network has no internet access or blocks DNS");
  }
}

void wifiWatch(uint32_t now) {
  static bool wasUp = false;
  bool up = (WiFi.status() == WL_CONNECTED);

  if (up && !wasUp) {
    Serial.printf("WiFi connected: SSID=%s  IP=%s  RSSI=%d dBm\n",
                  WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!up && wasUp) {
    Serial.println("WiFi connection lost");
  }
  wasUp = up;

  if (!up && now - lastWifiTry > WIFI_RETRY_MS) {
    lastWifiTry = now;
    Serial.println("Trying to reconnect WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

 
void firebaseBegin() {
  fbConfig.api_key      = API_KEY;
  fbConfig.database_url = DATABASE_URL;
  fbAuth.user.email     = USER_EMAIL;
  fbAuth.user.password  = USER_PASSWORD;
  fbConfig.token_status_callback = tokenStatusCallback;

  Firebase.reconnectWiFi(true);

  fbdo.setBSSLBufferSize(4096  , 1024  );
  fbdo.setResponseSize(2048);

  Firebase.begin(&fbConfig, &fbAuth);
}

void buildJson(FirebaseJson &json, bool linkNow) {
  json.set("ts", (int)time(nullptr));
  json.set("uno_link", linkNow);
  json.set("rssi", (int)WiFi.RSSI());
  json.set("uptime_s", (int)(millis() / 1000UL));
  json.set("frames_ok", (int)goodFrames);
  json.set("frames_bad", (int)badFrames);
  if (haveData) {
    json.set("seq", (int)lastData.seq);
    json.set("gas", lastData.gas);
    json.set("level", (int)lastData.state);
    json.set("levelName", stateName(lastData.state));
    json.set("fan", lastData.fan);
    json.set("overrun_s", (int)lastData.overrunS);
    json.set("buzzer", lastData.buzzer);
    json.set("flame", lastData.flame);
  }
}

bool pushAlert(const String &base, const char *type, int level, int gas, const char *message) {
  FirebaseJson a;
  a.set("ts", (int)time(nullptr));
  a.set("type", type);           
  a.set("level", level);
  a.set("gas", gas);
  a.set("message", message);
  bool ok = Firebase.RTDB.pushJSON(&fbdo, (base + "/alerts").c_str(), &a);
  if (!ok) {
    Serial.printf("[cloud] alert FAILED: %s\n", fbdo.errorReason().c_str());
  }
  return ok;
}

void cloudStep(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!Firebase.ready()) return;
  if (time(nullptr) < MIN_VALID_EPOCH) return;

  bool linkNow       = haveData && (now - lastGoodMs <= LINK_TIMEOUT_MS);
  bool stateChanged  = haveData && (reportedState != (int)lastData.state);
  bool flameChanged  = haveData && (reportedFlame != (int)lastData.flame);
  bool linkChanged   = (reportedLink != (int)linkNow);
  bool latestDue     = (now - lastLatestMs >= UPLOAD_LATEST_MS);
  bool historyDue    = haveData && (now - lastHistoryMs >= UPLOAD_HISTORY_MS);

  if (!(latestDue || stateChanged || flameChanged || linkChanged)) return;
  lastLatestMs = now;

  String base = String("/devices/") + DEVICE_ID;
  FirebaseJson json;
  buildJson(json, linkNow);

  bool ok = Firebase.RTDB.setJSON(&fbdo, (base + "/latest").c_str(), &json);
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, ok ? HIGH : LOW);
  if (!ok) {
    Serial.printf("[cloud] latest FAILED: %s\n", fbdo.errorReason().c_str());
    return;
  }
  if (!uploadedOnce) {
    uploadedOnce = true;
    Serial.println("[cloud] first upload OK - data is reaching Firebase");
  }

   
  if (haveData && reportedState == -1) {
    reportedState = lastData.state;
  } else if (stateChanged) {
    char msg[72];
    snprintf(msg, sizeof(msg), "%s -> %s (gas %d)",
             stateName((uint8_t)reportedState), stateName(lastData.state), lastData.gas);
    if (pushAlert(base, "LEVEL", lastData.state, lastData.gas, msg)) {
      reportedState = lastData.state;
    }
  }

   
  if (haveData && reportedFlame == -1) {
    reportedFlame = lastData.flame ? 1 : 0;
  } else if (flameChanged) {
    char msg[72];
    snprintf(msg, sizeof(msg), "%s (gas %d)",
             lastData.flame ? "FLAME DETECTED" : "Flame cleared", lastData.gas);
    if (pushAlert(base, "FLAME", lastData.flame ? 2 : 0, lastData.gas, msg)) {
      reportedFlame = lastData.flame ? 1 : 0;
    }
  }

   
  if (reportedLink == -1) {
    reportedLink = (int)linkNow;
  } else if (linkChanged) {
    const char *msg = linkNow ? "Arduino link restored" : "Arduino link lost (no data for 5 s)";
    if (pushAlert(base, "LINK", haveData ? lastData.state : 0, haveData ? lastData.gas : 0, msg)) {
      reportedLink = (int)linkNow;
    }
  }

   
  if (historyDue) {
    lastHistoryMs = now;
    if (Firebase.RTDB.pushJSON(&fbdo, (base + "/history").c_str(), &json)) {
      Serial.println("[cloud] history point stored");
    } else {
      Serial.printf("[cloud] history FAILED: %s\n", fbdo.errorReason().c_str());
    }
  }
}

 
 
void occupancyStep(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!Firebase.ready()) return;
  if (time(nullptr) < MIN_VALID_EPOCH) return;
  if (now - lastOccMs < OCC_POLL_MS) return;
  lastOccMs = now;

  String path = String("/rooms/") + ROOM_ID + "/occupancy";
  if (!Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
    Serial.printf("[cloud] occupancy read FAILED: %s\n", fbdo.errorReason().c_str());
    occCount = -1;
    return;
  }

  FirebaseJson json;
  FirebaseJsonData r;
  json.setJsonData(fbdo.jsonString());

  int    count = -1;
  double ts    = 0;
  json.get(r, "count");
  if (r.success) count = r.to<int>();
  json.get(r, "ts");
  if (r.success) ts = r.to<double>();

  bool fresh = (count >= 0) && (ts > 0) && ((long)time(nullptr) - (long)ts <= OCC_MAX_AGE_S);
  occCount = fresh ? count : -1;
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(300);
  Serial.println();
  Serial.println("ESP32 gateway started");

  if (STATUS_LED_PIN >= 0) pinMode(STATUS_LED_PIN, OUTPUT);
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, LOW);

  UnoSerial.setRxBufferSize(512);
  UnoSerial.begin(UNO_BAUD, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);
  Serial.printf("Listening on UART2: RX=GPIO%d, TX=GPIO%d, %lu baud\n",
                UNO_RX_PIN, UNO_TX_PIN, (unsigned long)UNO_BAUD);

  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  wifiBegin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_FIRST_WAIT_MS) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected yet - will keep retrying in the background");
  } else {
    Serial.printf("WiFi OK, signal %d dBm (better than -75 is good)\n", WiFi.RSSI());
    netDiag();
  }

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  firebaseBegin();

  lastWarnMs = millis();
}

void loop() {
  uint32_t now = millis();

  readUno();
  wifiWatch(now);
  cloudStep(now);
  occupancyStep(now);
  linkWatch(now);
}