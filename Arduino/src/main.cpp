#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

 
const uint8_t GAS_PIN    = A0;
const uint8_t FLAME_PIN  = 2;     
const uint8_t RELAY_PIN  = 7;
const uint8_t BUZZER_PIN = 8;
const uint8_t ESP_RX_PIN = 10;    
const uint8_t ESP_TX_PIN = 11;    

 
const uint8_t LCD_ADDRESS      = 0x27;    
const bool    RELAY_ACTIVE_LOW = true;
const bool    PASSIVE_BUZZER   = false;
const unsigned int BUZZER_FREQ = 2000;

 
const int WARN_ON    = 300;
const int WARN_OFF   = 270;
const int DANGER_ON  = 500;
const int DANGER_OFF = 450;

 
 
 
 
const bool FLAME_ACTIVE_LOW      = true;     
const uint8_t FLAME_CONFIRM      = 3;        
const unsigned long FLAME_HOLD_MS = 3000UL;  
const bool FAN_ON_FLAME          = false;    

 
const unsigned long WARMUP_MS          = 30000UL;
const unsigned long SAMPLE_INTERVAL_MS = 200UL;
const unsigned long LCD_INTERVAL_MS    = 500UL;
const unsigned long LCD_PAGE_MS        = 3000UL;    
const unsigned long SERIAL_INTERVAL_MS = 1000UL;
const unsigned long ESP_INTERVAL_MS    = 1000UL;
const unsigned long FAN_OVERRUN_MS     = 30000UL;
const unsigned long PEOPLE_TIMEOUT_MS  = 10000UL;   

 
const long ESP_BAUD        = 9600;
const bool ECHO_FRAME_USB  = true;

 
enum State { SAFE, WARNING, DANGER };

LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);    

State state = SAFE;
int gasLevel = 0;
bool fanOn = false;
bool buzzerOn = false;
bool warmingUp = true;
unsigned long fanOffTime = 0;
uint16_t txSeq = 0;

 
bool flameRaw = false;            
bool flameOn = false;
uint8_t flameCount = 0;
unsigned long flameLastSeen = 0;

 
int people = -1;                   
unsigned long lastPeopleMs = 0;

 
char rxBuf[32];
uint8_t rxLen = 0;

 
uint8_t lcdPage = 0;
unsigned long lastPage = 0;

unsigned long lastSample = 0;
unsigned long lastLcd = 0;
unsigned long lastSerial = 0;
unsigned long lastEsp = 0;

 
void relayWrite(bool on);
void buzzerWrite(bool on);
int  readGas();
bool readFlame();
void printLine(uint8_t row, const char *text);
void updateState();
void updateFlame(unsigned long now);
void updateFan(unsigned long now);
void updateBuzzer(unsigned long now);
bool peopleValid(unsigned long now);
void updateLcd(unsigned long now);
void sendToEsp(unsigned long now);
void handleEspLine(const char *line);
void readEsp();

 
void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void buzzerWrite(bool on) {
  buzzerOn = on;
  if (PASSIVE_BUZZER) {
    if (on) tone(BUZZER_PIN, BUZZER_FREQ);
    else    noTone(BUZZER_PIN);
  } else {
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  }
}

int readGas() {
  analogRead(GAS_PIN);           
  delay(1);
  long sum = 0;
  for (uint8_t i = 0; i < 16; i++) {
    sum += analogRead(GAS_PIN);
    delay(1);
  }
  return (int)(sum / 16);
}

 
bool readFlame() {
  bool level = digitalRead(FLAME_PIN);
  return FLAME_ACTIVE_LOW ? !level : level;
}

void printLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void updateState() {
  switch (state) {
    case SAFE:
      if (gasLevel >= DANGER_ON)      state = DANGER;
      else if (gasLevel >= WARN_ON)   state = WARNING;
      break;
    case WARNING:
      if (gasLevel >= DANGER_ON)      state = DANGER;
      else if (gasLevel < WARN_OFF)   state = SAFE;
      break;
    case DANGER:
      if (gasLevel < DANGER_OFF)      state = (gasLevel >= WARN_OFF) ? WARNING : SAFE;
      break;
  }
}

 
void updateFlame(unsigned long now) {
  if (!flameOn) {
    if (flameRaw) {
      if (++flameCount >= FLAME_CONFIRM) {
        flameOn = true;
        flameLastSeen = now;
      }
    } else {
      flameCount = 0;
    }
  } else {
    if (flameRaw) {
      flameLastSeen = now;
    } else if (now - flameLastSeen >= FLAME_HOLD_MS) {
      flameOn = false;
      flameCount = 0;
    }
  }
}

void updateFan(unsigned long now) {
  bool want = (state != SAFE) || (FAN_ON_FLAME && flameOn);
  if (want) {
    fanOn = true;
    fanOffTime = now + FAN_OVERRUN_MS;
  } else if (fanOn && (long)(now - fanOffTime) >= 0) {
    fanOn = false;
  }
  relayWrite(fanOn);
}

void updateBuzzer(unsigned long now) {
  bool on = false;
  if (flameOn)                on = true;                   
  else if (state == WARNING)  on = (now % 1000UL) < 100UL;
  else if (state == DANGER)   on = (now % 400UL) < 200UL;
  buzzerWrite(on);
}

bool peopleValid(unsigned long now) {
  return people >= 0 && (now - lastPeopleMs) < PEOPLE_TIMEOUT_MS;
}

void updateLcd(unsigned long now) {
  char line[17];
  char pbuf[6];

  if (peopleValid(now)) snprintf(pbuf, sizeof(pbuf), "%d", people > 999 ? 999 : people);
  else                  snprintf(pbuf, sizeof(pbuf), "--");

   
  bool alarm = (state != SAFE) || flameOn;
  if (alarm) {
    lcdPage = 0;
  } else if (now - lastPage >= LCD_PAGE_MS) {
    lastPage = now;
    lcdPage ^= 1;
  }

  if (lcdPage == 0) {
    snprintf(line, sizeof(line), "Gas:%4d P:%-3s", gasLevel, pbuf);
    printLine(0, line);

    const char *status;
    if (flameOn)                status = "FLAME!";
    else if (state == DANGER)   status = "DANGER!";
    else if (state == WARNING)  status = "WARN";
    else                        status = "SAFE";
    snprintf(line, sizeof(line), "%-7s Fan:%-3s", status, fanOn ? "ON" : "OFF");
    printLine(1, line);
  } else {
    snprintf(line, sizeof(line), "Flame: %s", flameOn ? "YES!" : "none");
    printLine(0, line);
    snprintf(line, sizeof(line), "People:%-3s %s", pbuf, peopleValid(now) ? "" : "nocam");
    printLine(1, line);
  }
}

 
void sendToEsp(unsigned long now) {
  unsigned int stateCode = warmingUp ? 3 : (unsigned int)state;

  unsigned int overrunS = 0;
  if (!warmingUp && fanOn && state == SAFE && !flameOn && (long)(now - fanOffTime) < 0) {
    overrunS = (unsigned int)((fanOffTime - now) / 1000UL);
  }

  char body[48];
  snprintf(body, sizeof(body), "G,%u,%d,%u,%u,%u,%u,%u",
           (unsigned int)txSeq++, gasLevel, stateCode,
           fanOn ? 1U : 0U, overrunS, buzzerOn ? 1U : 0U,
           flameOn ? 1U : 0U);

  uint8_t crc = 0;
  for (const char *p = body; *p; p++) crc ^= (uint8_t)*p;

  char frame[60];
  snprintf(frame, sizeof(frame), "$%s*%02X", body, (unsigned int)crc);

  espSerial.println(frame);
  if (ECHO_FRAME_USB) {
    Serial.print(F("TX -> ESP32: "));
    Serial.println(frame);
  }
}

 
 
void handleEspLine(const char *line) {
  if (line[0] != '$') return;
  const char *star = strrchr(line, '*');
  if (star == nullptr || strlen(star) < 3) return;

  uint8_t calc = 0;
  for (const char *p = line + 1; p < star; p++) calc ^= (uint8_t)*p;
  if (calc != (uint8_t)strtoul(star + 1, nullptr, 16)) return;    

  int count;
  if (sscanf(line + 1, "P,%d", &count) == 1) {
    people = count;                
    lastPeopleMs = millis();
  }
}

void readEsp() {
  while (espSerial.available()) {
    char c = (char)espSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (rxLen > 0) {
        rxBuf[rxLen] = '\0';
        handleEspLine(rxBuf);
      }
      rxLen = 0;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxLen = 0;                   
    }
  }
}

 
void setup() {
  Serial.begin(9600);
  espSerial.begin(ESP_BAUD);

  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLAME_PIN, INPUT);
  buzzerWrite(false);

  lcd.init();
  lcd.backlight();
  printLine(0, "Gas Monitor");

  unsigned long start = millis();
  while (millis() - start < WARMUP_MS) {
    readEsp();
    char line[17];
    unsigned long remaining = (WARMUP_MS - (millis() - start)) / 1000UL + 1;
    snprintf(line, sizeof(line), "Warming up %2lus", remaining);
    printLine(1, line);

    unsigned long now = millis();
    if (now - lastEsp >= ESP_INTERVAL_MS) {
      lastEsp = now;
      gasLevel = readGas();
      flameRaw = readFlame();
      sendToEsp(now);
    }
    delay(250);
  }
  warmingUp = false;

  buzzerWrite(true);
  delay(150);
  buzzerWrite(false);

  gasLevel = readGas();
  flameRaw = readFlame();
  lcd.clear();
}

 
void loop() {
  unsigned long now = millis();

  readEsp();

  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    gasLevel = readGas();
    flameRaw = readFlame();
    updateState();
    updateFlame(now);
    updateFan(now);
  }

  updateBuzzer(now);

  if (now - lastLcd >= LCD_INTERVAL_MS) {
    lastLcd = now;
    updateLcd(now);
  }

  if (now - lastEsp >= ESP_INTERVAL_MS) {
    lastEsp = now;
    sendToEsp(now);
  }

  if (now - lastSerial >= SERIAL_INTERVAL_MS) {
    lastSerial = now;
    Serial.print("Gas: ");
    Serial.print(gasLevel);
    Serial.print("  State: ");
    Serial.print(state == SAFE ? "SAFE" : (state == WARNING ? "WARNING" : "DANGER"));
    Serial.print("  Flame: ");
    Serial.print(flameOn ? "YES" : "no");
    Serial.print("  People: ");
    if (peopleValid(now)) Serial.print(people); else Serial.print("--");
    Serial.print("  Fan: ");
    Serial.println(fanOn ? "ON" : "OFF");
  }
}