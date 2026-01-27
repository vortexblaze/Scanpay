#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

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
char HOST_IP[16] = "192.168.0.50";
char DEVICE_ID[16] = "DEV001";
const uint16_t HOST_PORT = 8000;
float PRICE = 5.00f;
int INVOICE_DURATION = 60;
char DESCRIPTION[64] = "Sim payment";

static Preferences prefs;

// ======================= HTTP Helpers =======================
bool connectWiFi(uint32_t portalTimeoutMs = 180000) {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setConfigPortalTimeout((uint16_t)(portalTimeoutMs / 1000));

  WiFiManagerParameter hostParam("host_ip", "Host IP", HOST_IP, sizeof(HOST_IP));
  WiFiManagerParameter deviceParam("device_id", "Device ID", DEVICE_ID, sizeof(DEVICE_ID));
  char priceBuf[16];
  snprintf(priceBuf, sizeof(priceBuf), "%.2f", PRICE);
  char durationBuf[12];
  snprintf(durationBuf, sizeof(durationBuf), "%d", INVOICE_DURATION);
  WiFiManagerParameter priceParam("price", "Price", priceBuf, sizeof(priceBuf));
  WiFiManagerParameter invDurParam("invoice_duration", "Duration (sec)", durationBuf, sizeof(durationBuf));
  WiFiManagerParameter descParam("description", "Description", DESCRIPTION, sizeof(DESCRIPTION));
  wm.addParameter(&hostParam);
  wm.addParameter(&deviceParam);
  wm.addParameter(&priceParam);
  wm.addParameter(&invDurParam);
  wm.addParameter(&descParam);

  bool ok = wm.autoConnect("Scanpay-Setup");
  if (!ok) {
    return false;
  }

  const char* hostVal = hostParam.getValue();
  if (hostVal && hostVal[0] != '\0') {
    strncpy(HOST_IP, hostVal, sizeof(HOST_IP));
    HOST_IP[sizeof(HOST_IP) - 1] = '\0';
  }

  const char* deviceVal = deviceParam.getValue();
  if (deviceVal && deviceVal[0] != '\0') {
    strncpy(DEVICE_ID, deviceVal, sizeof(DEVICE_ID));
    DEVICE_ID[sizeof(DEVICE_ID) - 1] = '\0';
  }

  const char* priceVal = priceParam.getValue();
  if (priceVal && priceVal[0] != '\0') {
    PRICE = (float)atof(priceVal);
  }

  const char* invDurVal = invDurParam.getValue();
  if (invDurVal && invDurVal[0] != '\0') {
    INVOICE_DURATION = atoi(invDurVal);
  }

  const char* descVal = descParam.getValue();
  if (descVal && descVal[0] != '\0') {
    strncpy(DESCRIPTION, descVal, sizeof(DESCRIPTION));
    DESCRIPTION[sizeof(DESCRIPTION) - 1] = '\0';
  }

  return true;
}

void loadPrefs() {
  if (!prefs.begin("scanpay", true)) {
    return;
  }
  prefs.getString("host_ip", HOST_IP, sizeof(HOST_IP));
  prefs.getString("device_id", DEVICE_ID, sizeof(DEVICE_ID));
  PRICE = prefs.getFloat("price", PRICE);
  INVOICE_DURATION = prefs.getInt("invoice_duration", INVOICE_DURATION);
  prefs.getString("description", DESCRIPTION, sizeof(DESCRIPTION));
  prefs.end();
}

void savePrefs() {
  if (!prefs.begin("scanpay", false)) {
    return;
  }
  prefs.putString("host_ip", HOST_IP);
  prefs.putString("device_id", DEVICE_ID);
  prefs.putFloat("price", PRICE);
  prefs.putInt("invoice_duration", INVOICE_DURATION);
  prefs.putString("description", DESCRIPTION);
  prefs.end();
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
static uint32_t lastSvcLogMs[4] = { 0, 0, 0, 0 };
static uint32_t lastPollMs = 0;
static bool action = false;
static int duration = 0;
static bool complete = false;
static uint32_t actionStartMs = 0;

inline bool timeReached(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) >= 0;
}

inline void svcLog(uint8_t ch, const char* msg, uint32_t now) {
  if (ch >= 4) return;
  if (!timeReached(now, lastSvcLogMs[ch] + 5000)) return;
  lastSvcLogMs[ch] = now;
  Serial.print("[svc] ch=");
  Serial.print(ch);
  Serial.print(" state=");
  Serial.println(msg);
}

inline bool jsonHas(const String& body, const char* key, const char* value) {
  String needle = String("\"") + key + "\":" + value;
  return body.indexOf(needle) >= 0;
}

inline bool jsonInt(const String& body, const char* key, int* out) {
  if (!out) return false;
  String needle = String("\"") + key + "\":";
  int idx = body.indexOf(needle);
  if (idx < 0) return false;
  idx += needle.length();
  while (idx < (int)body.length() && (body[idx] == ' ' || body[idx] == '\t')) idx++;
  int sign = 1;
  if (idx < (int)body.length() && body[idx] == '-') {
    sign = -1;
    idx++;
  }
  long val = 0;
  bool any = false;
  while (idx < (int)body.length()) {
    char c = body[idx];
    if (c < '0' || c > '9') break;
    any = true;
    val = val * 10 + (c - '0');
    idx++;
  }
  if (!any) return false;
  *out = (int)(val * sign);
  return true;
}

inline void handleHttpBody(const String& body) {
  if (jsonHas(body, "has_command", "false")) {
    action = false;
    duration = 0;
    complete = false;
    actionStartMs = 0;
    return;
  }
  if (jsonHas(body, "has_command", "true")) {
    int actionVal = 0;
    int durVal = 0;
    (void)jsonInt(body, "action", &actionVal);
    (void)jsonInt(body, "duration_sec", &durVal);
    complete = false;
    action = (actionVal != 0);
    duration += durVal;
    actionStartMs = 0;
  }
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
    svcLog(ch, "off", now);
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
    svcLog(ch, "opto_active_skip", now);
    return;
  }

  // End an active pulse.
  if (relayState[ch] && pulseUntilMs[ch] > 0 && timeReached(now, pulseUntilMs[ch])) {
    svcLog(ch, "pulse_end", now);
    relayWrite(ch, false);
    relayState[ch] = false;
    pulseUntilMs[ch] = 0;
    // Record the end time so the OFF interval is durationMs.
    lastServiceMs[ch] = now;
  }

  // Start a new 1s pulse after remaining OFF for durationMs.
  if (!relayState[ch] && durationMs > 0 && timeReached(now, lastServiceMs[ch] + durationMs)) {
    svcLog(ch, "pulse_start", now);
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

  loadPrefs();

  if (connectWiFi()) {
    Serial.print("WiFi connected, IP=");
    Serial.println(WiFi.localIP());
    Serial.print("Host IP param=");
    Serial.println(HOST_IP);
    Serial.print("Device ID param=");
    Serial.println(DEVICE_ID);
    Serial.print("Price param=");
    Serial.println(PRICE, 2);
    Serial.print("Invoice duration param=");
    Serial.println(INVOICE_DURATION);
    Serial.print("Description param=");
    Serial.println(DESCRIPTION);
    savePrefs();
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

  if (action) {
    if (actionStartMs == 0) {
      actionStartMs = now;
    }
    if (duration > 0 && timeReached(now, actionStartMs + (uint32_t)duration * 1000U)) {
      complete = true;
    }
  } else {
    actionStartMs = 0;
  }

  if (complete && action && WiFi.status() == WL_CONNECTED) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:8000/api/device/%s/request-invoice/", HOST_IP, DEVICE_ID);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    char body[256];
    snprintf(body, sizeof(body),
             "{\"amount\":%.2f,\"description\":\"%s\",\"duration_sec\":%d}",
             PRICE, DESCRIPTION, INVOICE_DURATION);
    int code = http.POST((uint8_t*)body, strlen(body));
    if (code >= 200 && code < 300) {
      complete = false;
      action = false;
      duration = 0;
      actionStartMs = 0;
    } else {
      Serial.print("[http] post error ");
      Serial.println(code);
    }
    http.end();
  }

  if (!action && WiFi.status() == WL_CONNECTED && timeReached(now, lastPollMs + 5000)) {
    lastPollMs = now;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:8000/api/device/%s/next/", HOST_IP, DEVICE_ID);
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    if (code > 0) {
      String body = http.getString();
      Serial.print("[http] ");
      Serial.println(body);
      handleHttpBody(body);
    } else {
      Serial.print("[http] error ");
      Serial.println(code);
    }
    http.end();
  }

  if ((now % 1000) == 0 && now != lastServiceTickMs) {
    lastServiceTickMs = now;
    for (uint8_t ch = 0; ch < 4; ch++) {
      servicehandler(ch, serviceEnabled[ch], serviceDurationMs[ch], serviceOptoState[ch]);
    }
  }

  (void)now;
}
