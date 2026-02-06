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
#else
  digitalWrite(relayPins[ch], on ? HIGH : LOW);
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
static bool relayState[4] = { false, false, false, false };
static uint32_t pulseUntilMs[4] = { 0, 0, 0, 0 };
static uint32_t relayCooldownUntilMs[4] = { 0, 0, 0, 0 };
static uint8_t rrNextCh = 0;
static uint32_t lastPollMs = 0;
static uint32_t lastStatusMs = 0;
static bool pendingCommand = false;
static uint32_t pendingDurationMs = 0;
static bool action = false;
static int duration = 0;
static bool complete = false;
static uint32_t actionStartMs = 0;

inline bool timeReached(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) >= 0;
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
    pendingCommand = false;
    pendingDurationMs = 0;
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
    if (actionVal != 0 && !pendingCommand) {
      pendingCommand = true;
      if (durVal < 0) durVal = 0;
      pendingDurationMs = (uint32_t)durVal * 1000U;
    }
  }
}

inline bool channelAvailable(uint8_t ch, uint32_t now) {
  if (ch >= 4) return false;
  if (relayState[ch]) return false;
  if (!timeReached(now, relayCooldownUntilMs[ch])) return false;
  return true;
}

inline bool anyChannelAvailable(uint32_t now) {
  for (uint8_t ch = 0; ch < 4; ch++) {
    if (channelAvailable(ch, now)) return true;
  }
  return false;
}

inline int8_t pickChannelRoundRobin(uint32_t now) {
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t ch = (uint8_t)((rrNextCh + i) % 4);
    if (channelAvailable(ch, now)) {
      rrNextCh = (uint8_t)((ch + 1) % 4);
      return (int8_t)ch;
    }
  }
  return -1;
}

inline void startRelayPulse(uint8_t ch, uint32_t cooldownMs, uint32_t now) {
  if (ch >= 4) return;
  relayWrite(ch, true);
  relayState[ch] = true;
  pulseUntilMs[ch] = now + 1000;
  relayCooldownUntilMs[ch] = now + cooldownMs;
}

inline void updateRelayPulses(uint32_t now) {
  for (uint8_t ch = 0; ch < 4; ch++) {
    if (relayState[ch] && pulseUntilMs[ch] > 0 && timeReached(now, pulseUntilMs[ch])) {
      relayWrite(ch, false);
      relayState[ch] = false;
      pulseUntilMs[ch] = 0;
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
    pinMode(optoPins[i], INPUT);
  }

}

void loop() {
  uint32_t now = millis();

  updateRelayPulses(now);

  if (pendingCommand) {
    int8_t ch = pickChannelRoundRobin(now);
    if (ch >= 0) {
      startRelayPulse((uint8_t)ch, pendingDurationMs, now);
      pendingCommand = false;
    }
  }

  if (timeReached(now, lastStatusMs + 1000)) {
    lastStatusMs = now;
    uint8_t busyMask = 0;
    for (uint8_t ch = 0; ch < 4; ch++) {
      if (!channelAvailable(ch, now)) {
        busyMask |= (uint8_t)(1U << ch);
      }
    }
    Serial.print("[status] t=");
    Serial.print(now);
    Serial.print(" busy=0x");
    Serial.print(busyMask, HEX);
    Serial.print(" pending=");
    Serial.print(pendingCommand ? "1" : "0");
    Serial.print(" action=");
    Serial.print(action ? "1" : "0");
    Serial.print(" duration=");
    Serial.print(duration);
    Serial.print(" complete=");
    Serial.print(complete ? "1" : "0");
    Serial.print(" relay0=");
    Serial.print(relayState[0] ? "on" : "off");
    Serial.print(" opto0=");
    Serial.println(optoReadTriggered(0) ? "active" : "inactive");
  }

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

  if (!pendingCommand && anyChannelAvailable(now) && WiFi.status() == WL_CONNECTED &&
      timeReached(now, lastPollMs + 5000)) {
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

  (void)now;
}
