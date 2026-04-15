#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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
#define WIFI_CONFIG_PIN 17

// ======================= Polarity Config =======================
// If your relay is active LOW (common for opto-isolated relay boards), keep 1.
// If your relay is active HIGH, set to 0.
#define RELAY_ACTIVE_LOW 1

// If your opto input reads LOW when "triggered", keep 1.
// If it reads HIGH when triggered, set to 0.
#define OPTO_ACTIVE_LOW 1

// ======================= Network Config =======================
char HOST_IP[16] = "192.168.0.131";
char DEVICE_ID[16] = "DEV001";
char DEVICE_ID2[16] = "DEV002";
static const bool DEVICE2_ENABLED = true;
const uint16_t HOST_PORT = 8000;
float PRICE = 5.00f;
int INVOICE_DURATION = 60;
char DESCRIPTION[64] = "Sim payment";
char ACTIVE_DEVICE_ID[16] = "";

static Preferences prefs;
static const char* NVS_NS = "scanpay";
static const char* NVS_KEY_INV_DURATION = "inv_duration";
static const uint32_t HTTP_POLL_INTERVAL_MS = 2000;
static const uint16_t HTTP_TIMEOUT_MS = 2000;
static const uint16_t INVOICE_HTTP_TIMEOUT_MS = 10000;
static const uint32_t RELAY_WATCHDOG_GRACE_MS = 1000;
static const uint32_t STATUS_INTERVAL_MS = 1000;

enum NetworkPollType : uint8_t {
  NETWORK_POLL_NONE = 0,
  NETWORK_POLL_NO_COMMAND,
  NETWORK_POLL_COMMAND
};

struct NetworkPollResult {
  NetworkPollType type;
  uint8_t deviceIndex;
  bool action;
  int durationSec;
  bool hasCommandId;
  int commandId;
  char deviceId[16];
};

struct PendingCommand {
  uint8_t deviceIndex;
  uint32_t durationMs;
  char deviceId[16];
};

struct InvoiceRequest {
  uint8_t deviceIndex;
};

static QueueHandle_t networkPollQueue = nullptr;
static TaskHandle_t networkTaskHandle = nullptr;
static volatile bool networkPollAllowed = false;

// ======================= HTTP Helpers =======================
bool connectWiFi(bool forceConfigPortal = false, uint32_t portalTimeoutMs = 180000) {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setConfigPortalTimeout((uint16_t)(portalTimeoutMs / 1000));

  WiFiManagerParameter hostParam("host_ip", "Host IP", HOST_IP, sizeof(HOST_IP));
  WiFiManagerParameter deviceParam("device_id", "Device ID", DEVICE_ID, sizeof(DEVICE_ID));
  WiFiManagerParameter deviceParam2("device_id_2", "Device ID 2", DEVICE_ID2, sizeof(DEVICE_ID2));
  char priceBuf[16];
  snprintf(priceBuf, sizeof(priceBuf), "%.2f", PRICE);
  char durationBuf[12];
  snprintf(durationBuf, sizeof(durationBuf), "%d", INVOICE_DURATION);
  WiFiManagerParameter priceParam("price", "Price", priceBuf, sizeof(priceBuf));
  WiFiManagerParameter invDurParam(NVS_KEY_INV_DURATION, "Duration (sec)", durationBuf, sizeof(durationBuf));
  WiFiManagerParameter descParam("description", "Description", DESCRIPTION, sizeof(DESCRIPTION));
  wm.addParameter(&hostParam);
  wm.addParameter(&deviceParam);
  wm.addParameter(&deviceParam2);
  wm.addParameter(&priceParam);
  wm.addParameter(&invDurParam);
  wm.addParameter(&descParam);

  bool ok = false;
  if (forceConfigPortal) {
    ok = wm.startConfigPortal("Scanpay-Setup");
  } else {
    ok = wm.autoConnect("Scanpay-Setup");
  }
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

  const char* deviceVal2 = deviceParam2.getValue();
  if (deviceVal2 && deviceVal2[0] != '\0') {
    strncpy(DEVICE_ID2, deviceVal2, sizeof(DEVICE_ID2));
    DEVICE_ID2[sizeof(DEVICE_ID2) - 1] = '\0';
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
  if (!prefs.begin(NVS_NS, true)) {
    return;
  }
  prefs.getString("host_ip", HOST_IP, sizeof(HOST_IP));
  prefs.getString("device_id", DEVICE_ID, sizeof(DEVICE_ID));
  prefs.getString("device_id_2", DEVICE_ID2, sizeof(DEVICE_ID2));
  PRICE = prefs.getFloat("price", PRICE);
  INVOICE_DURATION = prefs.getInt(NVS_KEY_INV_DURATION, INVOICE_DURATION);
  prefs.getString("description", DESCRIPTION, sizeof(DESCRIPTION));
  prefs.end();
}

void savePrefs() {
  if (!prefs.begin(NVS_NS, false)) {
    return;
  }
  prefs.putString("host_ip", HOST_IP);
  prefs.putString("device_id", DEVICE_ID);
  prefs.putString("device_id_2", DEVICE_ID2);
  prefs.putFloat("price", PRICE);
  prefs.putInt(NVS_KEY_INV_DURATION, INVOICE_DURATION);
  prefs.putString("description", DESCRIPTION);
  prefs.end();
}

// ======================= Relay & Opto Function ====================

static const uint8_t RELAY_CHANNEL_COUNT = 2;
static const uint8_t relayPins[RELAY_CHANNEL_COUNT] = { RELAY0, RELAY1 };


inline void relayWrite(uint8_t ch, bool on) {
  if (ch >= RELAY_CHANNEL_COUNT) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPins[ch], on ? LOW : HIGH);
#else
  digitalWrite(relayPins[ch], on ? HIGH : LOW);
#endif
}

static const uint8_t OPTO_CHANNEL_COUNT = 4;
static const uint8_t optoPins[OPTO_CHANNEL_COUNT]  = { OPTO0,  OPTO1,  OPTO2,  OPTO3  };

inline bool optoReadTriggered(uint8_t ch) {
  if (ch >= OPTO_CHANNEL_COUNT) return false;
  int raw = digitalRead(optoPins[ch]);
#if OPTO_ACTIVE_LOW
  return (raw == LOW);
#else
  return (raw == HIGH);
#endif
}

// ======================= Relay Timing =======================
// Latching relays need short edge pulses to toggle, so each channel advances
// through start/hold/stop phases based on millis() instead of delay().
enum RelayPhase : uint8_t {
  RELAY_PHASE_IDLE = 0,
  RELAY_PHASE_START_PULSE,
  RELAY_PHASE_ACTIVE_WAIT,
  RELAY_PHASE_STOP_PULSE
};

static const uint32_t RELAY_PULSE_MS = 50;
static bool relayState[RELAY_CHANNEL_COUNT] = { false, false };
static uint32_t pulseUntilMs[RELAY_CHANNEL_COUNT] = { 0, 0 };
static uint32_t pulseEdgeMs[RELAY_CHANNEL_COUNT] = { 0, 0 };
static uint32_t relayCooldownUntilMs[RELAY_CHANNEL_COUNT] = { 0, 0 };
static uint32_t relayWatchdogUntilMs[RELAY_CHANNEL_COUNT] = { 0, 0 };
static int8_t relayDeviceIndex[RELAY_CHANNEL_COUNT] = { -1, -1 };
static RelayPhase relayPhase[RELAY_CHANNEL_COUNT] = {
  RELAY_PHASE_IDLE, RELAY_PHASE_IDLE
};
static PendingCommand pendingCommands[RELAY_CHANNEL_COUNT];
static uint8_t pendingCommandHead = 0;
static uint8_t pendingCommandTail = 0;
static uint8_t pendingCommandCount = 0;
static InvoiceRequest pendingInvoices[RELAY_CHANNEL_COUNT * 2];
static uint8_t pendingInvoiceHead = 0;
static uint8_t pendingInvoiceTail = 0;
static uint8_t pendingInvoiceCount = 0;
static uint8_t activeTaskCount[2] = { 0, 0 };
static uint32_t lastInvoiceAttemptMs = 0;
static uint32_t lastPollMs = 0;
static uint32_t lastStatusMs = 0;
static int lastCommandId[2] = { -1, -1 };
static bool wifiConfigPinWasActive = false;

inline bool timeReached(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) >= 0;
}

inline bool hasPendingCommands();
inline bool enqueuePendingCommand(uint8_t deviceIndex, uint32_t durationMs,
                                  const char* deviceId);
inline bool dequeuePendingCommand(PendingCommand* out);
inline void cancelPendingCommandsForDevice(uint8_t deviceIndex);
inline bool hasPendingCommandForDevice(uint8_t deviceIndex);
inline bool channelAvailable(uint8_t ch, uint32_t now);
inline int8_t relayChannelForDevice(uint8_t deviceIndex);
inline void processPendingCommands(uint32_t now);
inline void startRelayPulse(uint8_t ch, uint32_t onMs, uint32_t now,
                            uint8_t deviceIndex);
inline bool enqueueInvoiceRequest(uint8_t deviceIndex);
inline bool peekInvoiceRequest(InvoiceRequest* out);
inline void popInvoiceRequest();
inline bool dequeueInvoiceRequestAt(uint8_t offset, InvoiceRequest* out);
bool requestInvoice(
  const char* serverIp,
  const char* deviceId,
  const char* amount,
  uint32_t durationSec,
  String& invoiceId,
  String& payUrl,
  String& errorMsg
);
inline void processInvoiceRequests(uint32_t now);
inline void logBlockedCommand(const NetworkPollResult& result, uint32_t now);

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

inline bool parseHttpBody(const String& body, uint8_t deviceIndex,
                          const char* deviceId, NetworkPollResult* out) {
  if (deviceIndex == 1 && !DEVICE2_ENABLED) return false;
  if (!out || deviceIndex > 1 || !deviceId || deviceId[0] == '\0') return false;
  out->type = NETWORK_POLL_NONE;
  out->deviceIndex = deviceIndex;
  out->action = false;
  out->durationSec = 0;
  out->hasCommandId = false;
  out->commandId = -1;
  strncpy(out->deviceId, deviceId, sizeof(out->deviceId));
  out->deviceId[sizeof(out->deviceId) - 1] = '\0';

  if (jsonHas(body, "has_command", "false")) {
    out->type = NETWORK_POLL_NO_COMMAND;
    return true;
  }

  if (jsonHas(body, "has_command", "true")) {
    int actionVal = 0;
    int durVal = 0;
    int cmdId = -1;
    (void)jsonInt(body, "action", &actionVal);
    (void)jsonInt(body, "duration_sec", &durVal);
    out->type = NETWORK_POLL_COMMAND;
    out->action = (actionVal != 0);
    out->durationSec = (durVal < 0) ? 0 : durVal;
    out->hasCommandId = jsonInt(body, "command_id", &cmdId) && cmdId >= 0;
    out->commandId = out->hasCommandId ? cmdId : -1;
    return true;
  }

  return false;
}

inline void applyNetworkPollResult(const NetworkPollResult& result) {
  if (result.deviceIndex == 1 && !DEVICE2_ENABLED) return;
  if (result.deviceIndex > 1 || result.type == NETWORK_POLL_NONE) return;
  if (result.type == NETWORK_POLL_NO_COMMAND) {
    // "No command" means no new work from backend.
    // Keep local action/duration state so an in-progress cycle can finish
    // and trigger invoice generation.
    cancelPendingCommandsForDevice(result.deviceIndex);
    return;
  }

  if (result.type == NETWORK_POLL_COMMAND) {
    uint32_t now = millis();
    int8_t mappedRelay = relayChannelForDevice(result.deviceIndex);
    if (result.hasCommandId &&
        lastCommandId[result.deviceIndex] == result.commandId) {
      // Duplicate command from backend poll, ignore restart.
      return;
    }
    if (result.action &&
        (mappedRelay < 0 ||
         hasPendingCommandForDevice(result.deviceIndex) ||
         !channelAvailable((uint8_t)mappedRelay, now))) {
      logBlockedCommand(result, now);
      return;
    }
    if (result.action &&
        !enqueuePendingCommand(result.deviceIndex,
                               (uint32_t)result.durationSec * 1000U,
                               result.deviceId)) {
      logBlockedCommand(result, now);
      return;
    }
    if (result.hasCommandId) {
      lastCommandId[result.deviceIndex] = result.commandId;
    }
    strncpy(ACTIVE_DEVICE_ID, result.deviceId, sizeof(ACTIVE_DEVICE_ID));
    ACTIVE_DEVICE_ID[sizeof(ACTIVE_DEVICE_ID) - 1] = '\0';
  }
}

inline bool channelAvailable(uint8_t ch, uint32_t now) {
  if (ch >= RELAY_CHANNEL_COUNT) return false;
  if (relayState[ch]) return false;
  if (!timeReached(now, relayCooldownUntilMs[ch])) return false;
  return true;
}

inline bool anyChannelAvailable(uint32_t now) {
  for (uint8_t ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    if (channelAvailable(ch, now)) return true;
  }
  return false;
}

inline int8_t relayChannelForDevice(uint8_t deviceIndex) {
  if (deviceIndex >= RELAY_CHANNEL_COUNT) return -1;
  if (deviceIndex == 1 && !DEVICE2_ENABLED) return -1;
  return (int8_t)deviceIndex;
}

inline bool hasPendingCommands() {
  return pendingCommandCount > 0;
}

inline bool enqueuePendingCommand(uint8_t deviceIndex, uint32_t durationMs,
                                  const char* deviceId) {
  if (pendingCommandCount >= RELAY_CHANNEL_COUNT) return false;
  PendingCommand& cmd = pendingCommands[pendingCommandTail];
  cmd.deviceIndex = deviceIndex;
  cmd.durationMs = durationMs;
  strncpy(cmd.deviceId, deviceId, sizeof(cmd.deviceId));
  cmd.deviceId[sizeof(cmd.deviceId) - 1] = '\0';
  pendingCommandTail = (uint8_t)((pendingCommandTail + 1) % RELAY_CHANNEL_COUNT);
  pendingCommandCount++;
  return true;
}

inline bool dequeuePendingCommand(PendingCommand* out) {
  if (!out || pendingCommandCount == 0) return false;
  *out = pendingCommands[pendingCommandHead];
  pendingCommandHead = (uint8_t)((pendingCommandHead + 1) % RELAY_CHANNEL_COUNT);
  pendingCommandCount--;
  return true;
}

inline void cancelPendingCommandsForDevice(uint8_t deviceIndex) {
  if (pendingCommandCount == 0) return;
  PendingCommand kept[RELAY_CHANNEL_COUNT];
  uint8_t keptCount = 0;
  while (pendingCommandCount > 0) {
    PendingCommand cmd;
    (void)dequeuePendingCommand(&cmd);
    if (cmd.deviceIndex != deviceIndex) {
      kept[keptCount++] = cmd;
    }
  }
  pendingCommandHead = 0;
  pendingCommandTail = 0;
  pendingCommandCount = 0;
  for (uint8_t i = 0; i < keptCount; i++) {
    (void)enqueuePendingCommand(kept[i].deviceIndex, kept[i].durationMs,
                                kept[i].deviceId);
  }
}

inline bool hasPendingCommandForDevice(uint8_t deviceIndex) {
  if (pendingCommandCount == 0) return false;
  uint8_t idx = pendingCommandHead;
  for (uint8_t i = 0; i < pendingCommandCount; i++) {
    if (pendingCommands[idx].deviceIndex == deviceIndex) {
      return true;
    }
    idx = (uint8_t)((idx + 1) % RELAY_CHANNEL_COUNT);
  }
  return false;
}

inline void logBlockedCommand(const NetworkPollResult& result, uint32_t now) {
  (void)result;
  (void)now;
}

inline bool enqueueInvoiceRequest(uint8_t deviceIndex) {
  const uint8_t capacity = (uint8_t)(sizeof(pendingInvoices) / sizeof(pendingInvoices[0]));
  if (deviceIndex > 1 || pendingInvoiceCount >= capacity) return false;
  pendingInvoices[pendingInvoiceTail].deviceIndex = deviceIndex;
  pendingInvoiceTail = (uint8_t)((pendingInvoiceTail + 1) % capacity);
  pendingInvoiceCount++;
  return true;
}

inline bool peekInvoiceRequest(InvoiceRequest* out) {
  if (!out || pendingInvoiceCount == 0) return false;
  *out = pendingInvoices[pendingInvoiceHead];
  return true;
}

inline void popInvoiceRequest() {
  if (pendingInvoiceCount == 0) return;
  const uint8_t capacity = (uint8_t)(sizeof(pendingInvoices) / sizeof(pendingInvoices[0]));
  pendingInvoiceHead = (uint8_t)((pendingInvoiceHead + 1) % capacity);
  pendingInvoiceCount--;
}

inline bool dequeueInvoiceRequestAt(uint8_t offset, InvoiceRequest* out) {
  if (!out || offset >= pendingInvoiceCount) return false;

  const uint8_t capacity = (uint8_t)(sizeof(pendingInvoices) / sizeof(pendingInvoices[0]));
  InvoiceRequest kept[RELAY_CHANNEL_COUNT * 2];
  uint8_t keptCount = 0;
  bool removed = false;

  while (pendingInvoiceCount > 0) {
    InvoiceRequest req = pendingInvoices[pendingInvoiceHead];
    popInvoiceRequest();

    if (!removed && keptCount == offset) {
      *out = req;
      removed = true;
      continue;
    }

    kept[keptCount++] = req;
  }

  pendingInvoiceHead = 0;
  pendingInvoiceTail = 0;
  pendingInvoiceCount = 0;
  for (uint8_t i = 0; i < keptCount; i++) {
    pendingInvoices[pendingInvoiceTail] = kept[i];
    pendingInvoiceTail = (uint8_t)((pendingInvoiceTail + 1) % capacity);
    pendingInvoiceCount++;
  }

  return removed;
}

inline void finishRelayTask(uint8_t ch, bool successful) {
  if (ch >= RELAY_CHANNEL_COUNT) return;
  int8_t deviceIndex = relayDeviceIndex[ch];
  if (deviceIndex >= 0 && deviceIndex <= 1) {
    if (activeTaskCount[deviceIndex] > 0) {
      activeTaskCount[deviceIndex]--;
    }
    if (successful) {
      (void)enqueueInvoiceRequest((uint8_t)deviceIndex);
    }
  }
  relayDeviceIndex[ch] = -1;
}

bool requestInvoice(
  const char* serverIp,
  const char* deviceId,
  const char* amount,
  uint32_t durationSec,
  String& invoiceId,
  String& payUrl,
  String& errorMsg
) {
  HTTPClient http;

  String url = "http://" + String(serverIp) + ":8000/api/device/" +
               String(deviceId) + "/request-invoice/";
  String body = "{\"amount\":\"" + String(amount) +
                "\",\"description\":\"ESP32 auto invoice\"" +
                ",\"duration_sec\":" + String(durationSec) + "}";

  Serial.print("API ");
  Serial.println(url);

  http.setTimeout(INVOICE_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(url)) {
    errorMsg = "http.begin() failed";
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);

  // Always read the response body before closing the connection.
  String response = http.getString();
  http.end();

  if (httpCode != 201) {
    errorMsg = "HTTP " + String(httpCode) + " -> " + response;
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    errorMsg = "JSON parse failed: " + String(err.c_str());
    return false;
  }

  invoiceId = doc["public_id"] | "";
  payUrl = doc["pay_url"] | "";

  if (invoiceId.isEmpty() || payUrl.isEmpty()) {
    errorMsg = "Missing public_id or pay_url in response";
    return false;
  }

  errorMsg = "";
  return true;
}

inline void processInvoiceRequests(uint32_t now) {
  if (pendingInvoiceCount == 0 || WiFi.status() != WL_CONNECTED) return;
  if (!timeReached(now, lastInvoiceAttemptMs + 1000U)) return;

  uint8_t readyOffset = pendingInvoiceCount;
  for (uint8_t i = 0; i < pendingInvoiceCount; i++) {
    uint8_t idx = (uint8_t)((pendingInvoiceHead + i) %
                            (sizeof(pendingInvoices) / sizeof(pendingInvoices[0])));
    const InvoiceRequest& candidate = pendingInvoices[idx];
    int8_t mappedRelay = relayChannelForDevice(candidate.deviceIndex);
    if (mappedRelay >= 0 && channelAvailable((uint8_t)mappedRelay, now)) {
      readyOffset = i;
      break;
    }
  }

  if (readyOffset >= pendingInvoiceCount) return;

  InvoiceRequest req;
  if (!dequeueInvoiceRequestAt(readyOffset, &req)) return;
  lastInvoiceAttemptMs = now;

  const char* deviceId = (req.deviceIndex == 0) ? DEVICE_ID : DEVICE_ID2;
  if (req.deviceIndex == 1 && !DEVICE2_ENABLED) return;

  char amountBuf[16];
  snprintf(amountBuf, sizeof(amountBuf), "%.2f", PRICE);
  String invoiceId;
  String payUrl;
  String errorMsg;
  if (!requestInvoice(HOST_IP, deviceId, amountBuf,
                      (uint32_t)INVOICE_DURATION,
                      invoiceId, payUrl, errorMsg)) {
    (void)enqueueInvoiceRequest(req.deviceIndex);
  }
}

inline void processPendingCommands(uint32_t now) {
  if (pendingCommandCount == 0) return;

  PendingCommand kept[RELAY_CHANNEL_COUNT];
  uint8_t keptCount = 0;
  while (pendingCommandCount > 0) {
    PendingCommand cmd;
    if (!dequeuePendingCommand(&cmd)) {
      break;
    }

    int8_t ch = relayChannelForDevice(cmd.deviceIndex);
    if (ch >= 0 && channelAvailable((uint8_t)ch, now)) {
      strncpy(ACTIVE_DEVICE_ID, cmd.deviceId, sizeof(ACTIVE_DEVICE_ID));
      ACTIVE_DEVICE_ID[sizeof(ACTIVE_DEVICE_ID) - 1] = '\0';
      startRelayPulse((uint8_t)ch, cmd.durationMs, now, cmd.deviceIndex);
      continue;
    }

    kept[keptCount++] = cmd;
  }

  pendingCommandHead = 0;
  pendingCommandTail = 0;
  pendingCommandCount = 0;
  for (uint8_t i = 0; i < keptCount; i++) {
    (void)enqueuePendingCommand(kept[i].deviceIndex, kept[i].durationMs,
                                kept[i].deviceId);
  }
}



inline void startRelayPulse(uint8_t ch, uint32_t onMs, uint32_t now,
                            uint8_t deviceIndex) {
  if (ch >= RELAY_CHANNEL_COUNT) return;
  relayWrite(ch, true);
  relayState[ch] = true;
  relayDeviceIndex[ch] = (deviceIndex <= 1) ? (int8_t)deviceIndex : -1;
  if (deviceIndex <= 1) {
    activeTaskCount[deviceIndex]++;
  }
  relayPhase[ch] = RELAY_PHASE_START_PULSE;
  pulseEdgeMs[ch] = now + RELAY_PULSE_MS;
  pulseUntilMs[ch] = now + onMs;
  relayCooldownUntilMs[ch] = now + onMs;
  relayWatchdogUntilMs[ch] = now + onMs + RELAY_WATCHDOG_GRACE_MS +
                             (2U * RELAY_PULSE_MS);
}

inline void updateRelayPulses(uint32_t now) {
  for (uint8_t ch = 0; ch < RELAY_CHANNEL_COUNT; ch++) {
    if (!relayState[ch]) {
      continue;
    }

    if (relayWatchdogUntilMs[ch] > 0 &&
        timeReached(now, relayWatchdogUntilMs[ch])) {
      relayWrite(ch, false);
      relayState[ch] = false;
      relayPhase[ch] = RELAY_PHASE_IDLE;
      pulseUntilMs[ch] = 0;
      pulseEdgeMs[ch] = 0;
      relayWatchdogUntilMs[ch] = 0;
      finishRelayTask(ch, false);
      continue;
    }

    if (relayPhase[ch] == RELAY_PHASE_START_PULSE &&
        timeReached(now, pulseEdgeMs[ch])) {
      relayWrite(ch, false);
      relayPhase[ch] = RELAY_PHASE_ACTIVE_WAIT;
      continue;
    }

    if (relayPhase[ch] == RELAY_PHASE_ACTIVE_WAIT &&
        pulseUntilMs[ch] > 0 && timeReached(now, pulseUntilMs[ch])) {
      relayWrite(ch, true);
      relayPhase[ch] = RELAY_PHASE_STOP_PULSE;
      pulseEdgeMs[ch] = now + RELAY_PULSE_MS;
      continue;
    }

    if (relayPhase[ch] == RELAY_PHASE_STOP_PULSE &&
        timeReached(now, pulseEdgeMs[ch])) {
      relayWrite(ch, false);
      relayState[ch] = false;
      relayPhase[ch] = RELAY_PHASE_IDLE;
      pulseUntilMs[ch] = 0;
      pulseEdgeMs[ch] = 0;
      relayWatchdogUntilMs[ch] = 0;
      finishRelayTask(ch, true);
    }
  }
}
inline void drainNetworkPollQueue() {
  if (networkPollQueue == nullptr) return;
  NetworkPollResult result;
  while (xQueueReceive(networkPollQueue, &result, 0) == pdTRUE) {
    applyNetworkPollResult(result);
  }
}

inline bool pollNextForDevice(const char* deviceId, uint8_t deviceIndex) {
  if (deviceIndex == 1 && !DEVICE2_ENABLED) return false;
  if (!deviceId || deviceId[0] == '\0' || networkPollQueue == nullptr) return false;
  char url[128];
  snprintf(url, sizeof(url), "http://%s:8000/api/device/%s/next/", HOST_IP, deviceId);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    String body = http.getString();
    NetworkPollResult result;
    bool parsed = parseHttpBody(body, deviceIndex, deviceId, &result);
    http.end();
    if (parsed) {
      return (xQueueSend(networkPollQueue, &result, 0) == pdTRUE) &&
             (result.type == NETWORK_POLL_COMMAND);
    }
    return false;
  }

  http.end();
  return false;
}

void networkTask(void* parameter) {
  (void)parameter;
  for (;;) {
    uint32_t now = millis();
    if (networkPollAllowed && WiFi.status() == WL_CONNECTED &&
        timeReached(now, lastPollMs + HTTP_POLL_INTERVAL_MS)) {
      lastPollMs = now;
      (void)pollNextForDevice(DEVICE_ID, 0);
      if (DEVICE2_ENABLED) {
        (void)pollNextForDevice(DEVICE_ID2, 1);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(WIFI_CONFIG_PIN, INPUT_PULLUP);
  bool forceConfigPortal = (digitalRead(WIFI_CONFIG_PIN) == LOW);
  wifiConfigPinWasActive = forceConfigPortal;

  loadPrefs();

  if (connectWiFi(forceConfigPortal)) {
    savePrefs();
  }
  
  for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);
    relayWrite(i, false); // start OFF
  }

  for (uint8_t i = 0; i < OPTO_CHANNEL_COUNT; i++) {
#if OPTO_ACTIVE_LOW
    pinMode(optoPins[i], INPUT_PULLUP);
#else
    pinMode(optoPins[i], INPUT_PULLDOWN);
#endif
  }

  networkPollQueue = xQueueCreate(4, sizeof(NetworkPollResult));
  if (networkPollQueue != nullptr &&
      xTaskCreate(networkTask, "scanpay-net", 6144, nullptr, 1,
                  &networkTaskHandle) != pdPASS) {
    vQueueDelete(networkPollQueue);
    networkPollQueue = nullptr;
  }
}

void loop() {
  uint32_t now = millis();
  bool wifiConfigPinActive = (digitalRead(WIFI_CONFIG_PIN) == LOW);

  if (wifiConfigPinActive && !wifiConfigPinWasActive) {
    networkPollAllowed = false;
    if (connectWiFi(true)) {
      savePrefs();
    }
  }
  wifiConfigPinWasActive = wifiConfigPinActive;

  updateRelayPulses(now);
  drainNetworkPollQueue();
  processInvoiceRequests(now);
  processPendingCommands(now);

  if (timeReached(now, lastStatusMs + STATUS_INTERVAL_MS)) {
    lastStatusMs = now;
    Serial.print("S ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "W1" : "W0");
    Serial.print(" C");
    Serial.print(wifiConfigPinActive ? "1" : "0");
    Serial.print(" R");
    Serial.print(relayState[0] ? "1" : "0");
    Serial.print(relayState[1] ? "1" : "0");
    Serial.print(" Q");
    Serial.print(pendingCommandCount);
    Serial.print(" T");
    Serial.print(activeTaskCount[0]);
    Serial.print(activeTaskCount[1]);
    Serial.print(" I");
    Serial.print(pendingInvoiceCount);
    Serial.print(" O");
    Serial.print(optoReadTriggered(0) ? "1" : "0");
    Serial.print(optoReadTriggered(1) ? "1" : "0");
    Serial.print(optoReadTriggered(2) ? "1" : "0");
    Serial.println(optoReadTriggered(3) ? "1" : "0");
  }

  networkPollAllowed = (!wifiConfigPinActive &&
                        pendingCommandCount < RELAY_CHANNEL_COUNT &&
                        WiFi.status() == WL_CONNECTED);

  (void)now;
}
