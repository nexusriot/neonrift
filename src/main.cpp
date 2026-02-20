#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static const char* MDNS_NAME = "neonrift";   // http://neonrift.local
static const char* NVS_NS    = "neonrift";
static const char* NVS_KEY_NETWORKS = "networks_v1";

// Tune
static const int MAX_NETWORKS = 10;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;

AsyncWebServer server(80);
Preferences prefs;

struct Network {
  String ssid;
  String pass;
  bool filled = false;
};

static Network g_networks[MAX_NETWORKS];

struct WiFiCred {
  const char* ssid;
  const char* pass;
};

static const WiFiCred WIFI_CREDS[] = {
  {"SSID1_HERE", "PASSWORD1_HERE"},
  {"SSID2_HERE", "PASSWORD2_HERE"},
};


static void ensureNvsInitialized() {
  if (!prefs.begin(NVS_NS, false)) return;
  if (prefs.getBytesLength(NVS_KEY_NETWORKS) == 0) {
    uint8_t zero = 0;
    prefs.putBytes(NVS_KEY_NETWORKS, &zero, 1); // one empty slot marker
  }
  prefs.end();
}

static const size_t WIFI_CREDS_COUNT = sizeof(WIFI_CREDS) / sizeof(WIFI_CREDS[0]);

// Storage format:
// For each slot i:
//   [1 byte ssid_len] [ssid bytes...] [1 byte pass_len] [pass bytes...]
// If ssid_len==0 => empty slot, pass_len omitted.
static bool loadNetworks() {
  // clear RAM slots
  for (int i = 0; i < MAX_NETWORKS; i++) g_networks[i] = Network{};

  // open NVS (readonly)
  if (!prefs.begin(NVS_NS, true)) {
    Serial.println("NVS: begin(readonly) failed, treating as empty");
    return true; // treat as "no saved networks"
  }

  size_t size = prefs.getBytesLength(NVS_KEY_NETWORKS);
  if (size == 0) {
    prefs.end();
    return true; // no saved networks
  }
  if (size > 1024) size = 1024;

  uint8_t buf[1024];
  size_t got = prefs.getBytes(NVS_KEY_NETWORKS, buf, size);
  prefs.end();

  if (got == 0) {
    Serial.println("NVS: getBytes returned 0, treating as empty");
    return true;
  }

  // parse blob
  size_t p = 0;
  for (int slot = 0; slot < MAX_NETWORKS && p < got; slot++) {
    uint8_t ssidLen = buf[p++];

    if (ssidLen == 0) {
      g_networks[slot].filled = false;
      continue;
    }

    if (p + ssidLen > got) break;
    String ssid;
    ssid.reserve(ssidLen);
    for (uint8_t i = 0; i < ssidLen; i++) ssid += char(buf[p++]);

    if (p >= got) break;
    uint8_t passLen = buf[p++];

    if (p + passLen > got) break;
    String pass;
    pass.reserve(passLen);
    for (uint8_t i = 0; i < passLen; i++) pass += char(buf[p++]);

    g_networks[slot].ssid = ssid;
    g_networks[slot].pass = pass;
    g_networks[slot].filled = true;
  }

  return true;
}

static bool saveNetworks() {
  uint8_t buf[1024];
  size_t p = 0;

  for (int slot = 0; slot < MAX_NETWORKS; slot++) {
    const Network& n = g_networks[slot];
    if (!n.filled || n.ssid.length() == 0) {
      if (p + 1 > sizeof(buf)) break;
      buf[p++] = 0;
      continue;
    }

    uint8_t ssidLen = (uint8_t)min((int)n.ssid.length(), 31);   // WiFi SSID max 32
    uint8_t passLen = (uint8_t)min((int)n.pass.length(), 63);   // WPA2 pass max 63

    if (p + 1 + ssidLen + 1 + passLen > sizeof(buf)) break;

    buf[p++] = ssidLen;
    for (uint8_t i = 0; i < ssidLen; i++) buf[p++] = (uint8_t)n.ssid[i];

    buf[p++] = passLen;
    for (uint8_t i = 0; i < passLen; i++) buf[p++] = (uint8_t)n.pass[i];
  }

    if (!prefs.begin(NVS_NS, false)) {
    Serial.println("NVS: begin(rw) failed");
    return false;
  }
  size_t written = prefs.putBytes(NVS_KEY_NETWORKS, buf, p);
  prefs.end();
  return written == p;
}

static int firstFreeSlot() {
  for (int i = 0; i < MAX_NETWORKS; i++) {
    if (!g_networks[i].filled) return i;
  }
  return -1;
}


struct Candidate {
  int slot;
  int rssi;
  String ssid;
};

static bool connectWithTimeout(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, pass);

  Serial.printf("Connecting to '%s' ", ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. SSID='%s' IP=%s RSSI=%d\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
  }

  Serial.printf("Failed to connect to '%s' (status=%d)\n", ssid, (int)WiFi.status());
  return false;
}

static bool connectFirstAvailableSaved(uint32_t connectTimeoutMsPerTry = CONNECT_TIMEOUT_MS) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(50);

  Serial.println("Scanning WiFi...");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n < 0) {
    Serial.printf("Scan failed: %d\n", n);
    return false;
  }
  Serial.printf("Scan done: %d networks\n", n);

  Candidate cand[32];
  int m = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);

    for (int slot = 0; slot < MAX_NETWORKS; slot++) {
      if (!g_networks[slot].filled) continue;
      if (ssid == g_networks[slot].ssid) {
        Serial.printf("  saved match: '%s' rssi=%d slot=%d\n", ssid.c_str(), rssi, slot + 1);
        if (m < (int)(sizeof(cand) / sizeof(cand[0]))) {
          cand[m++] = Candidate{slot, rssi, ssid};
        }
        break;
      }
    }
  }

  if (m == 0) {
    Serial.println("No saved networks found in scan results.");
    return false;
  }

  // Sort by RSSI desc
  for (int i = 0; i < m - 1; i++) {
    int best = i;
    for (int j = i + 1; j < m; j++) {
      if (cand[j].rssi > cand[best].rssi) best = j;
    }
    if (best != i) {
      Candidate tmp = cand[i];
      cand[i] = cand[best];
      cand[best] = tmp;
    }
  }

  for (int i = 0; i < m; i++) {
    const Network& net = g_networks[cand[i].slot];
    Serial.printf("Trying slot %d: '%s' (rssi=%d)\n", cand[i].slot + 1, net.ssid.c_str(), cand[i].rssi);
    if (connectWithTimeout(net.ssid.c_str(), net.pass.c_str(), connectTimeoutMsPerTry)) {
      return true;
    }
  }

  Serial.println("Tried all saved networks, none connected.");
  return false;
}

static bool connectFirstAvailableFallback(uint32_t connectTimeoutMsPerTry = CONNECT_TIMEOUT_MS) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(50);

  Serial.println("Scanning WiFi (fallback list)...");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n < 0) {
    Serial.printf("Scan failed: %d\n", n);
    return false;
  }
  Serial.printf("Scan done: %d networks\n", n);

  struct FBCand { int idx; int rssi; String ssid; };
  FBCand cand[32];
  int m = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);

    for (size_t k = 0; k < WIFI_CREDS_COUNT; k++) {
      if (ssid == WIFI_CREDS[k].ssid) {
        Serial.printf("  fallback match: '%s' rssi=%d\n", ssid.c_str(), rssi);
        if (m < (int)(sizeof(cand) / sizeof(cand[0]))) {
          cand[m++] = FBCand{(int)k, rssi, ssid};
        }
        break;
      }
    }
  }

  if (m == 0) {
    Serial.println("No fallback networks found in scan results.");
    return false;
  }

  // sort by RSSI desc
  for (int i = 0; i < m - 1; i++) {
    int best = i;
    for (int j = i + 1; j < m; j++) {
      if (cand[j].rssi > cand[best].rssi) best = j;
    }
    if (best != i) {
      auto tmp = cand[i];
      cand[i] = cand[best];
      cand[best] = tmp;
    }
  }

  for (int i = 0; i < m; i++) {
    const WiFiCred& c = WIFI_CREDS[cand[i].idx];
    Serial.printf("Trying fallback: '%s' (rssi=%d)\n", c.ssid, cand[i].rssi);
    if (connectWithTimeout(c.ssid, c.pass, connectTimeoutMsPerTry)) {
      return true;
    }
  }

  Serial.println("Tried all fallback networks, none connected.");
  return false;
}


static uint64_t getChipId64() {
#if defined(ESP_ARDUINO_VERSION_MAJOR)
  return ESP.getEfuseMac();
#else
  return (uint64_t)ESP.getEfuseMac();
#endif
}

static String chipIdHex() {
  uint64_t id = getChipId64();
  char buf[17];
  snprintf(buf, sizeof(buf), "%08X%08X", (uint32_t)(id >> 32), (uint32_t)(id & 0xFFFFFFFF));
  return String(buf);
}

static void sendJson(AsyncWebServerRequest* request, int status, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse* res = request->beginResponse(status, "application/json; charset=utf-8", out);
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

static void sendError(AsyncWebServerRequest* request, int status, const char* message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendJson(request, status, doc);
}

static void handleHealth(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["status"] = "ok";
  doc["ts_ms"] = (uint64_t)millis();
  sendJson(request, 200, doc);
}

static void handleInfo(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;

  doc["chip_id"] = chipIdHex();
  doc["sdk"] = ESP.getSdkVersion();
  doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  doc["flash_size"] = ESP.getFlashChipSize();
  doc["uptime_ms"] = (uint64_t)millis();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["status"] = (int)WiFi.status();
  wifi["connected"] = WiFi.isConnected();
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  wifi["mac"] = WiFi.macAddress();

  sendJson(request, 200, doc);
}

static void handleNetworksList(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["connected"] = WiFi.isConnected();
  doc["current_ssid"] = WiFi.isConnected() ? WiFi.SSID() : "";

  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < MAX_NETWORKS; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["slot"] = i + 1;
    o["filled"] = g_networks[i].filled;
    o["ssid"] = g_networks[i].filled ? g_networks[i].ssid : "";
    o["active"] = (WiFi.isConnected() && g_networks[i].filled && WiFi.SSID() == g_networks[i].ssid);
  }

  sendJson(request, 200, doc);
}

// Body: "<ssid>\n<password>"
static void handleNetworksAdd(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
  String body;
  body.reserve(len + 1);
  for (size_t i = 0; i < len; i++) body += char(data[i]);

  int nl = body.indexOf('\n');
  if (nl < 1) {
    sendError(request, 400, "use format: <ssid>\\n<password>");
    return;
  }

  String ssid = body.substring(0, nl);
  String pass = body.substring(nl + 1);
  ssid.trim();
  if (pass.endsWith("\r")) pass.remove(pass.length() - 1);

  if (ssid.length() == 0) {
    sendError(request, 400, "ssid_empty");
    return;
  }
  if (ssid.length() > 32) {
    sendError(request, 400, "ssid_too_long");
    return;
  }
  if (pass.length() > 63) {
    sendError(request, 400, "password_too_long");
    return;
  }

  // If already exists -> update password
  for (int i = 0; i < MAX_NETWORKS; i++) {
    if (g_networks[i].filled && g_networks[i].ssid == ssid) {
      g_networks[i].pass = pass;
      bool ok = saveNetworks();
      JsonDocument doc;
      doc["ok"] = ok;
      doc["updated"] = true;
      doc["slot"] = i + 1;
      sendJson(request, ok ? 200 : 500, doc);
      return;
    }
  }

  int slot = firstFreeSlot();
  if (slot < 0) {
    sendError(request, 400, "no_free_slots");
    return;
  }

  g_networks[slot].ssid = ssid;
  g_networks[slot].pass = pass;
  g_networks[slot].filled = true;

  bool ok = saveNetworks();
  JsonDocument doc;
  doc["ok"] = ok;
  doc["added"] = true;
  doc["slot"] = slot + 1;
  sendJson(request, ok ? 200 : 500, doc);
}

// Body: "<number>" (1..MAX_NETWORKS)
static void handleNetworksDelete(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
  String body;
  body.reserve(len + 1);
  for (size_t i = 0; i < len; i++) body += char(data[i]);
  body.trim();

  if (body.length() == 0) {
    sendError(request, 400, "empty_body");
    return;
  }

  int slot = body.toInt(); // 0 if invalid
  if (slot < 1 || slot > MAX_NETWORKS) {
    sendError(request, 400, "slot_out_of_range");
    return;
  }
  int idx = slot - 1;

  if (!g_networks[idx].filled) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["deleted"] = false;
    doc["slot"] = slot;
    doc["note"] = "slot_was_empty";
    sendJson(request, 200, doc);
    return;
  }

  bool wasActive = WiFi.isConnected() && (WiFi.SSID() == g_networks[idx].ssid);

  g_networks[idx] = Network{};
  bool ok = saveNetworks();

  JsonDocument doc;
  doc["ok"] = ok;
  doc["deleted"] = true;
  doc["slot"] = slot;
  doc["was_active"] = wasActive;
  sendJson(request, ok ? 200 : 500, doc);
}

static void handleWifiReconnect(AsyncWebServerRequest* request) {
  JsonDocument doc;

  bool ok = connectFirstAvailableSaved(CONNECT_TIMEOUT_MS);
  if (!ok) ok = connectFirstAvailableFallback(CONNECT_TIMEOUT_MS);

  doc["ok"] = ok;
  doc["connected"] = WiFi.isConnected();
  doc["ssid"] = WiFi.isConnected() ? WiFi.SSID() : "";
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  doc["source"] = WiFi.isConnected() ? "saved_or_fallback" : "none";

  sendJson(request, ok ? 200 : 503, doc);
}

static void handleNotFound(AsyncWebServerRequest* request) {
  String path = request->url();
  if (path == "/") path = "/index.html";

  if (LittleFS.exists(path)) {
    request->send(LittleFS, path);
    return;
  }

  if (path == "/favicon.ico") {
    request->send(204);
    return;
  }

  sendError(request, 404, "not_found");
}

static void setupFs() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    return;
  }

  Serial.println("LittleFS mounted.");
  Serial.println("LittleFS files:");

  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    Serial.printf("  %s (%u bytes)\n", f.name(), (unsigned)f.size());
    f = root.openNextFile();
  }

  Serial.printf("exists(/index.html)=%s\n", LittleFS.exists("/index.html") ? "yes" : "no");
}

static void setupWifi() {
  bool ok = connectFirstAvailableSaved(CONNECT_TIMEOUT_MS);

  if (!ok) {
    Serial.println("Saved networks did not connect, trying fallback list...");
    ok = connectFirstAvailableFallback(CONNECT_TIMEOUT_MS);
  }

  if (!ok) {
    Serial.println("WiFi NOT connected (saved + fallback all failed).");
  }
}

static void setupMdns() {
  if (WiFi.isConnected()) {
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
    } else {
      Serial.println("mDNS setup failed");
    }
  }
}

static void setupRoutes() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.on("/health", HTTP_GET, handleHealth);
  server.on("/info",   HTTP_GET, handleInfo);

  server.on("/networks", HTTP_GET, handleNetworksList);

  // AsyncWebServer body handlers
  server.on(
    "/networks/add",
    HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    handleNetworksAdd
  );

  server.on(
    "/networks/delete",
    HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    handleNetworksDelete
  );

  server.on("/wifi/reconnect", HTTP_POST, handleWifiReconnect);

 server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
  if (LittleFS.exists("/favicon.ico")) {
    request->send(LittleFS, "/favicon.ico", "image/x-icon");
  } else {
    request->send(204);
  }
});

  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=300");

  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  ensureNvsInitialized();
  delay(200);

  setupFs();

  if (!loadNetworks()) {
    Serial.println("WARN: failed to load networks from NVS");
  }

  setupWifi();
  setupMdns();
  setupRoutes();

  server.begin();
  Serial.printf("Listening on %s:80\n", WiFi.localIP().toString().c_str());
  Serial.println("Async web server started on port 80");
}

void loop() {
  // async server - nothing needed
}