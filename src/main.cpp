#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ======================= Pin Mapping (same as your code) =======================
#define RELAY0 23
#define RELAY1 5
#define RELAY2 4
#define RELAY3 13

#define OPTO0 25
#define OPTO1 26
#define OPTO2 27
#define OPTO3 33

// ======================= Polarity Config =======================
// If your relay is active LOW (common for opto-isolated relay boards), keep 1.
// If your relay is active HIGH, set to 0.
#define RELAY_ACTIVE_LOW 1

// If your opto input reads LOW when "triggered", keep 1.
// If it reads HIGH when triggered, set to 0.
#define OPTO_ACTIVE_LOW 1

bool relay0 = false; // relay0 flag, currently using only one for development use

// ======================= Network Config =======================
const char* WIFI_SSID = "test1";
const char* WIFI_PASS = "b7badb97";

const char* HOST_IP = "192.168.0.50";
const uint16_t HOST_PORT = 8000;
const char* DEVICE_ID = "DEV001";

// ======================= HTTP Helpers =======================
bool connectWiFi(uint32_t timeoutMs = 10000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
    Serial.println("Trying to Connect Wifi");
  }

  return (WiFi.status() == WL_CONNECTED);
  
}

// ======================= Relay & Opto Function ====================

static const uint8_t relayPins[4] = { RELAY0, RELAY1, RELAY2, RELAY3 };


inline void relayWrite(uint8_t ch, bool on) {
  if (ch >= 4) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPins[ch], on ? LOW : HIGH);
   Serial.println("Relay had been turned HI");
#else
  digitalWrite(relayPins[ch], on ? HIGH : LOW);
  Serial.println("Relay had been turned LOW");
#endif
}

static const uint8_t optoPins[4]  = { OPTO0,  OPTO1,  OPTO2,  OPTO3  };

inline bool optoReadTriggered(uint8_t ch) {
  if (ch >= 4) return false;
  int raw = digitalRead(optoPins[ch]);
#if OPTO_ACTIVE_LOW
  return (raw == LOW);
#else
  return (raw == HIGH);
#endif
}

// ======================= Relay Timing =======================
// Per-channel duration (ms) used for logging/recording only.
static uint32_t Duration[4] = { 0, 0, 0, 0 };
static bool relayState[4] = { false, false, false, false };
static uint32_t lastServiceMs[4] = { 0, 0, 0, 0 };
static uint32_t pulseUntilMs[4] = { 0, 0, 0, 0 };
static bool serviceEnabled[4] = { false, false, false, false };
static uint32_t serviceDurationMs[4] = { 0, 0, 0, 0 };
static bool serviceOptoState[4] = { false, false, false, false };
static uint32_t lastServiceTickMs = 0;

inline bool timeReached(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) >= 0;
}

inline void toLowerInPlace(char* s) {
  if (!s) return;
  while (*s) {
    if (*s >= 'A' && *s <= 'Z') {
      *s = (char)(*s - 'A' + 'a');
    }
    s++;
  }
}

inline bool parseBoolToken(const char* s, bool* out) {
  if (!s || !out) return false;
  if (strcmp(s, "1") == 0 || strcmp(s, "on") == 0 || strcmp(s, "true") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(s, "0") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

// Handler: event trigger (toggle relay) + record the provided duration.
inline void stateupdate(uint8_t ch, uint32_t durationMs) {
  if (ch >= 4) return;
  bool nextState = !relayState[ch];
  relayState[ch] = nextState;
  relayWrite(ch, nextState);
  Duration[ch] = durationMs;
}

// Service handler: periodic pulse unless opto is active.
inline void servicehandler(uint8_t ch, bool on, uint32_t durationMs, bool optostate) {
  if (ch >= 4) return;

  Duration[ch] = durationMs;
  uint32_t now = millis();

  if (!on) {
    Serial.print("[svc] ch=");
    Serial.print(ch);
    Serial.println(" state=off");
    if (relayState[ch]) {
      relayWrite(ch, false);
      relayState[ch] = false;
    }
    pulseUntilMs[ch] = 0;
    lastServiceMs[ch] = now;
    return;
  }

  // If opto is active, skip the timed pulse logic.
  if (optostate) {
    Serial.print("[svc] ch=");
    Serial.print(ch);
    Serial.println(" state=opto_active_skip");
    return;
  }

  // End an active pulse.
  if (relayState[ch] && pulseUntilMs[ch] > 0 && timeReached(now, pulseUntilMs[ch])) {
    Serial.print("[svc] ch=");
    Serial.print(ch);
    Serial.println(" state=pulse_end");
    relayWrite(ch, false);
    relayState[ch] = false;
    pulseUntilMs[ch] = 0;
    // Record the end time so the OFF interval is durationMs.
    lastServiceMs[ch] = now;
  }

  // Start a new 1s pulse after remaining OFF for durationMs.
  if (!relayState[ch] && durationMs > 0 && timeReached(now, lastServiceMs[ch] + durationMs)) {
    Serial.print("[svc] ch=");
    Serial.print(ch);
    Serial.println(" state=pulse_start");
    relayWrite(ch, true);
    relayState[ch] = true;
    pulseUntilMs[ch] = now + 1000;
  }
}

inline void serialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  state <ch> <durationMs>   (event trigger: toggles relay)");
  Serial.println("  svc <ch> <on|off> <durationMs> <opto on|off>");
  Serial.println("  help");
}

inline void serialHandleLine(char* line) {
  char* tok = strtok(line, " ,\t");
  if (!tok) return;
  toLowerInPlace(tok);

  if (strcmp(tok, "help") == 0) {
    serialHelp();
    return;
  }

  if (strcmp(tok, "state") == 0 || strcmp(tok, "relay") == 0) {
    char* chTok = strtok(NULL, " ,\t");
    char* durTok = strtok(NULL, " ,\t");
    if (!chTok || !durTok) {
      Serial.println("Usage: state <ch> <durationMs>");
      return;
    }
    uint8_t ch = (uint8_t)strtoul(chTok, nullptr, 10);
    uint32_t dur = (uint32_t)strtoul(durTok, nullptr, 10);
    stateupdate(ch, dur);
    Serial.print("[serial] state ch=");
    Serial.print(ch);
    Serial.print(" toggle=");
    Serial.print(relayState[ch] ? "on" : "off");
    Serial.print(" duration=");
    Serial.println(dur);
    return;
  }

  if (strcmp(tok, "svc") == 0 || strcmp(tok, "service") == 0) {
    char* chTok = strtok(NULL, " ,\t");
    char* onTok = strtok(NULL, " ,\t");
    char* durTok = strtok(NULL, " ,\t");
    char* optoTok = strtok(NULL, " ,\t");
    if (!chTok || !onTok || !durTok || !optoTok) {
      Serial.println("Usage: svc <ch> <on|off> <durationMs> <opto on|off>");
      return;
    }
    toLowerInPlace(onTok);
    toLowerInPlace(optoTok);
    bool on = false;
    bool opto = false;
    if (!parseBoolToken(onTok, &on) || !parseBoolToken(optoTok, &opto)) {
      Serial.println("Invalid on/off token.");
      return;
    }
    uint8_t ch = (uint8_t)strtoul(chTok, nullptr, 10);
    uint32_t dur = (uint32_t)strtoul(durTok, nullptr, 10);
    serviceEnabled[ch] = on;
    serviceDurationMs[ch] = dur;
    serviceOptoState[ch] = opto;
    lastServiceMs[ch] = millis();
    servicehandler(ch, on, dur, opto);
    Serial.print("[serial] svc ch=");
    Serial.print(ch);
    Serial.print(" on=");
    Serial.print(on ? "on" : "off");
    Serial.print(" duration=");
    Serial.print(dur);
    Serial.print(" opto=");
    Serial.println(opto ? "on" : "off");
    return;
  }

  Serial.println("Unknown command. Type 'help'.");
}

inline void serialHandler() {
  static char lineBuf[80];
  static uint8_t idx = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[idx] = '\0';
      if (idx > 0) {
        serialHandleLine(lineBuf);
      }
      idx = 0;
    } else if (idx < (sizeof(lineBuf) - 1)) {
      lineBuf[idx++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("=== Relay Response and Web Service Starting ===");

  if (connectWiFi()) {
    Serial.print("WiFi connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed");
  }
  
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    relayWrite(i, false); // start OFF
  
  }

}

void loop() {
  uint32_t now = millis();

  serialHandler();

  if ((now % 1000) == 0 && now != lastServiceTickMs) {
    lastServiceTickMs = now;
    for (uint8_t ch = 0; ch < 4; ch++) {
      servicehandler(ch, serviceEnabled[ch], serviceDurationMs[ch], serviceOptoState[ch]);
    }
  }

  (void)now;
}
