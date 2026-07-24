#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include <vector>
#include <algorithm>

// Optional local config (Wi-Fi seeds, API token). See include/secrets.h.example.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const char* MDNS_NAME = "neonrift";       // http://neonrift.local
static const char* AP_SSID   = "neonrift-setup";  // captive-portal SoftAP
static const uint32_t CONNECT_TIMEOUT_MS = 15000; // per-network connect attempt
static const uint32_t TELEMETRY_PERIOD_MS = 2000; // WebSocket push interval
static const char* NTP_SERVER1 = "pool.ntp.org";
static const char* NTP_SERVER2 = "time.nist.gov";

#ifndef LED_PIN
#define LED_PIN 2  // onboard LED on most esp32dev boards
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
Preferences prefs;

struct Cred {
  String ssid;
  String pass;
};
static std::vector<Cred> g_creds;

static bool     g_apMode      = false;     // running the setup portal
static bool     g_ledState    = false;
static bool     g_reboot      = false;     // deferred reboot (after provisioning/OTA)
static uint32_t g_rebootAt    = 0;
static uint32_t g_lastTelemetry = 0;

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
static const char* apiToken() {
#ifdef API_TOKEN
  return API_TOKEN;
#else
  return "";
#endif
}

// Returns true if the request is authorized (or auth is disabled).
static bool authOk(AsyncWebServerRequest* request) {
  const char* tok = apiToken();
  if (!tok || tok[0] == '\0') return true;  // auth disabled
  if (request->hasHeader("Authorization")) {
    String h = request->header("Authorization");
    if (h.startsWith("Bearer ") && h.substring(7) == tok) return true;
  }
  if (request->hasParam("token")) {
    if (request->getParam("token")->value() == tok) return true;
  }
  if (request->hasParam("token", true)) {  // POST body param
    if (request->getParam("token", true)->value() == tok) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Credential storage (NVS + compile-time seeds)
// ---------------------------------------------------------------------------
static int credIndex(const String& ssid) {
  for (size_t i = 0; i < g_creds.size(); i++) {
    if (g_creds[i].ssid == ssid) return (int)i;
  }
  return -1;
}

static void saveCreds() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& c : g_creds) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = c.ssid;
    o["pass"] = c.pass;
  }
  String out;
  serializeJson(doc, out);
  prefs.begin("wifi", false);
  prefs.putString("creds", out);
  prefs.end();
}

static void addSeed(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') return;
  if (credIndex(String(ssid)) >= 0) return;  // saved creds take precedence
  g_creds.push_back(Cred{String(ssid), String(pass ? pass : "")});
}

static void seedCreds() {
#ifdef WIFI_SEED_SSID1
  addSeed(WIFI_SEED_SSID1, WIFI_SEED_PASS1);
#endif
#ifdef WIFI_SEED_SSID2
  addSeed(WIFI_SEED_SSID2, WIFI_SEED_PASS2);
#endif
#ifdef WIFI_SEED_SSID3
  addSeed(WIFI_SEED_SSID3, WIFI_SEED_PASS3);
#endif
}

static void loadCreds() {
  g_creds.clear();
  prefs.begin("wifi", true);
  String s = prefs.getString("creds", "");
  prefs.end();
  if (s.length()) {
    JsonDocument doc;
    if (deserializeJson(doc, s) == DeserializationError::Ok) {
      for (JsonObject o : doc.as<JsonArray>()) {
        Cred c;
        c.ssid = o["ssid"].as<String>();
        c.pass = o["pass"].as<String>();
        if (c.ssid.length()) g_creds.push_back(c);
      }
    }
  }
  seedCreds();  // merge compile-time defaults that aren't already present
  Serial.printf("Loaded %u Wi-Fi credential(s)\n", (unsigned)g_creds.size());
}

// ---------------------------------------------------------------------------
// Wi-Fi connect
// ---------------------------------------------------------------------------
static bool connectWithTimeout(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  Serial.printf("Connecting to '%s' ", ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected. SSID='%s' IP=%s RSSI=%d\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("Failed to connect to '%s' (status=%d)\n", ssid, (int)WiFi.status());
  return false;
}

static bool connectFirstAvailableKnown(uint32_t timeoutPerTry) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  delay(50);

  Serial.println("Scanning Wi-Fi...");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n <= 0) {
    Serial.printf("Scan found no networks (%d)\n", n);
    return false;
  }
  Serial.printf("Scan done: %d networks\n", n);

  struct Cand { int credIdx; int rssi; String ssid; };
  std::vector<Cand> cand;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int idx = credIndex(ssid);
    if (idx >= 0) {
      Serial.printf("  known: '%s' rssi=%d\n", ssid.c_str(), WiFi.RSSI(i));
      cand.push_back(Cand{idx, WiFi.RSSI(i), ssid});
    }
  }
  if (cand.empty()) {
    Serial.println("No known networks in range.");
    return false;
  }

  std::sort(cand.begin(), cand.end(),
            [](const Cand& a, const Cand& b) { return a.rssi > b.rssi; });

  for (auto& c : cand) {
    Serial.printf("Trying '%s' (rssi=%d)\n", c.ssid.c_str(), c.rssi);
    if (connectWithTimeout(g_creds[c.credIdx].ssid.c_str(),
                           g_creds[c.credIdx].pass.c_str(), timeoutPerTry)) {
      return true;
    }
  }
  Serial.println("Tried all known networks, none connected.");
  return false;
}

static void startApPortal() {
  WiFi.mode(WIFI_AP_STA);  // AP for the portal, STA so we can still scan
  WiFi.softAP(AP_SSID);
  delay(100);
  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(53, "*", ip);  // wildcard DNS => captive portal
  g_apMode = true;
  Serial.printf("Setup portal: SSID='%s' http://%s/\n", AP_SSID, ip.toString().c_str());
}

// ---------------------------------------------------------------------------
// Device info / telemetry (shared by /info HTTP and the WebSocket)
// ---------------------------------------------------------------------------
static uint64_t getChipId64() { return ESP.getEfuseMac(); }

static String chipIdHex() {
  uint64_t id = getChipId64();
  char buf[17];
  snprintf(buf, sizeof(buf), "%08X%08X", (uint32_t)(id >> 32), (uint32_t)(id & 0xFFFFFFFF));
  return String(buf);
}

static void addTime(JsonDocument& doc) {
  time_t now = time(nullptr);
  bool synced = now > 1700000000;  // sometime in 2023+
  doc["time_synced"] = synced;
  if (synced) {
    doc["time_epoch"] = (uint64_t)now;
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char iso[25];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    doc["time_iso"] = iso;
  }
}

static void buildInfo(JsonDocument& doc) {
  doc["ok"] = true;
  doc["ap_mode"] = g_apMode;

  doc["chip_id"] = chipIdHex();
  doc["sdk"] = ESP.getSdkVersion();
  doc["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  doc["sketch_size"] = ESP.getSketchSize();
  doc["free_sketch_space"] = ESP.getFreeSketchSpace();

  doc["uptime_ms"] = (uint64_t)millis();
  doc["uptime_s"] = (uint64_t)(millis() / 1000ULL);

  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min_free"] = ESP.getMinFreeHeap();
  doc["heap_max_alloc"] = ESP.getMaxAllocHeap();
  doc["psram_size"] = ESP.getPsramSize();
  doc["psram_free"] = ESP.getFreePsram();

  doc["flash_size"] = ESP.getFlashChipSize();
  doc["reset_reason"] = (int)esp_reset_reason();

  doc["led"] = g_ledState;
  addTime(doc);

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["mode"] = (int)WiFi.getMode();
  wifi["status"] = (int)WiFi.status();
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  wifi["gateway"] = WiFi.isConnected() ? WiFi.gatewayIP().toString() : String("");
  wifi["subnet"] = WiFi.isConnected() ? WiFi.subnetMask().toString() : String("");
  wifi["mac"] = WiFi.macAddress();
  if (g_apMode) {
    wifi["ap_ssid"] = AP_SSID;
    wifi["ap_ip"] = WiFi.softAPIP().toString();
    wifi["ap_clients"] = (int)WiFi.softAPgetStationNum();
  }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------
static void handleHealth(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["status"] = "ok";
  doc["ts_ms"] = (uint64_t)millis();
  addTime(doc);
  sendJson(request, 200, doc);
}

static void handleInfo(AsyncWebServerRequest* request) {
  JsonDocument doc;
  buildInfo(doc);
  sendJson(request, 200, doc);
}

// GET /api/scan -> visible networks (blocking scan, used during setup)
static void handleScan(AsyncWebServerRequest* request) {
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n && i < 32; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    o["known"] = (credIndex(WiFi.SSID(i)) >= 0);
  }
  WiFi.scanDelete();
  sendJson(request, 200, doc);
}

// GET /api/wifi -> saved SSIDs (no passwords) + current connection
static void handleWifiList(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["connected"] = WiFi.isConnected();
  doc["current"] = WiFi.isConnected() ? WiFi.SSID() : String("");
  doc["ap_mode"] = g_apMode;
  JsonArray arr = doc["saved"].to<JsonArray>();
  for (auto& c : g_creds) arr.add(c.ssid);
  sendJson(request, 200, doc);
}

// DELETE /api/wifi?ssid=...
static void handleWifiDelete(AsyncWebServerRequest* request) {
  if (!authOk(request)) { sendError(request, 401, "unauthorized"); return; }
  if (!request->hasParam("ssid")) { sendError(request, 400, "ssid_required"); return; }
  String ssid = request->getParam("ssid")->value();
  int idx = credIndex(ssid);
  if (idx < 0) { sendError(request, 404, "not_found"); return; }
  g_creds.erase(g_creds.begin() + idx);
  saveCreds();
  JsonDocument doc;
  doc["ok"] = true;
  doc["removed"] = ssid;
  doc["note"] = "compile-time seed networks reappear on reboot";
  sendJson(request, 200, doc);
}

static void handleLedGet(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["pin"] = LED_PIN;
  doc["on"] = g_ledState;
  sendJson(request, 200, doc);
}

static void applyLed() {
  digitalWrite(LED_PIN, g_ledState ? HIGH : LOW);
}

static void handleNotFound(AsyncWebServerRequest* request) {
  if (LittleFS.exists(request->url())) {
    request->send(LittleFS, request->url());
    return;
  }
  if (g_apMode) {  // captive portal: send everything to the setup page
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
    return;
  }
  sendError(request, 404, "not_found");
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
static void broadcastTelemetry() {
  if (ws.count() == 0) return;
  JsonDocument doc;
  buildInfo(doc);
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void*, uint8_t*, size_t) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument doc;
    buildInfo(doc);
    String out;
    serializeJson(doc, out);
    client->text(out);
  }
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------
static void scheduleReboot(uint32_t ms) {
  g_reboot = true;
  g_rebootAt = millis() + ms;
}

static void setupWifi() {
  if (connectFirstAvailableKnown(CONNECT_TIMEOUT_MS)) {
    g_apMode = false;
    configTime(0, 0, NTP_SERVER1, NTP_SERVER2);  // UTC; populates time(nullptr)
  } else {
    Serial.println("WiFi not connected; starting setup portal.");
    startApPortal();
  }
}

static void setupMdns() {
  if (WiFi.isConnected() && MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
  }
}

static void setupFs() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    return;
  }
  Serial.println("LittleFS mounted. Files:");
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    Serial.printf("  %s (%u bytes)\n", f.name(), (unsigned)f.size());
  }
}

static void setupRoutes() {
  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Read-only API
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_GET, handleWifiList);
  server.on("/api/wifi", HTTP_DELETE, handleWifiDelete);
  server.on("/api/led", HTTP_GET, handleLedGet);

  // POST /api/wifi {ssid, pass} -> save + reboot to connect
  auto* wifiPost = new AsyncCallbackJsonWebHandler(
      "/api/wifi", [](AsyncWebServerRequest* request, JsonVariant& json) {
        if (!authOk(request)) { sendError(request, 401, "unauthorized"); return; }
        JsonObject o = json.as<JsonObject>();
        String ssid = o["ssid"] | "";
        String pass = o["pass"] | "";
        if (!ssid.length()) { sendError(request, 400, "ssid_required"); return; }
        int idx = credIndex(ssid);
        if (idx >= 0) g_creds[idx].pass = pass;
        else g_creds.push_back(Cred{ssid, pass});
        saveCreds();
        JsonDocument doc;
        doc["ok"] = true;
        doc["saved"] = ssid;
        doc["reboot"] = true;
        sendJson(request, 200, doc);
        scheduleReboot(1500);  // reboot to (re)connect with the new credential
      });
  wifiPost->setMethod(HTTP_POST);
  server.addHandler(wifiPost);

  // POST /api/led {on}
  auto* ledPost = new AsyncCallbackJsonWebHandler(
      "/api/led", [](AsyncWebServerRequest* request, JsonVariant& json) {
        if (!authOk(request)) { sendError(request, 401, "unauthorized"); return; }
        g_ledState = json["on"] | false;
        applyLed();
        JsonDocument doc;
        doc["ok"] = true;
        doc["on"] = g_ledState;
        sendJson(request, 200, doc);
      });
  ledPost->setMethod(HTTP_POST);
  server.addHandler(ledPost);

  // OTA firmware update: POST /update (multipart "update" field)
  server.on(
      "/update", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (!authOk(request)) { sendError(request, 401, "unauthorized"); return; }
        bool ok = !Update.hasError();
        JsonDocument doc;
        doc["ok"] = ok;
        if (!ok) doc["error"] = Update.errorString();
        sendJson(request, ok ? 200 : 500, doc);
        if (ok) scheduleReboot(1500);
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          if (!authOk(request)) return;  // POST handler will report 401
          Serial.printf("OTA start: %s\n", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        }
        if (Update.isRunning() && len) {
          if (Update.write(data, len) != len) Update.printError(Serial);
        }
        if (final && Update.isRunning()) {
          if (Update.end(true)) Serial.printf("OTA done: %u bytes\n", (unsigned)(index + len));
          else Update.printError(Serial);
        }
      });

  // Root + static files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("max-age=300");

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");

  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  applyLed();

  setupFs();
  loadCreds();
  setupWifi();
  setupMdns();
  setupRoutes();

  server.begin();
  Serial.println("Async web server started on port 80");
}

void loop() {
  if (g_apMode) dnsServer.processNextRequest();

  uint32_t now = millis();
  if (now - g_lastTelemetry >= TELEMETRY_PERIOD_MS) {
    g_lastTelemetry = now;
    broadcastTelemetry();
  }
  ws.cleanupClients();

  if (g_reboot && (int32_t)(now - g_rebootAt) >= 0) {
    Serial.println("Rebooting...");
    delay(50);
    ESP.restart();
  }
}
