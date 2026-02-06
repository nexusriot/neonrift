#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static const char* MDNS_NAME = "neonrift";  // http://neonrift.local


struct WiFiCred {
  const char* ssid;
  const char* pass;
};

static const WiFiCred WIFI_CREDS[] = {
  {"SSID1_HERE", "PASSWORD1_HERE"},
  {"SSID2_HERE", "PASSWORD2_HERE"},
  {"SSID3_HERE", "PASSWORD3_HERE"},
};

static const size_t WIFI_CREDS_COUNT = sizeof(WIFI_CREDS) / sizeof(WIFI_CREDS[0]);


AsyncWebServer server(80);


static int findCredIndex(const String& ssid) {
  for (size_t i = 0; i < WIFI_CREDS_COUNT; i++) {
    if (ssid == WIFI_CREDS[i].ssid) return (int)i;
  }
  return -1;
}

struct Candidate {
  int credIndex;
  int rssi;
  String ssid;
};

static bool connectWithTimeout(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
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

static bool connectFirstAvailableKnown(uint32_t connectTimeoutMsPerTry = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  delay(50);

  Serial.println("Scanning WiFi...");
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);

  if (n < 0) {
    Serial.printf("Scan failed: %d\n", n);
    return false;
  }

  Serial.printf("Scan done: %d networks\n", n);


  Candidate cand[16];
  int m = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int idx = findCredIndex(ssid);
    if (idx >= 0) {
      int rssi = WiFi.RSSI(i);
      Serial.printf("  known: '%s' rssi=%d\n", ssid.c_str(), rssi);
      if (m < (int)(sizeof(cand) / sizeof(cand[0]))) {
        cand[m++] = Candidate{idx, rssi, ssid};
      }
    }
  }

  if (m == 0) {
    Serial.println("No known networks found.");
    return false;
  }

  // Sort candidates by RSSI desc
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
    const WiFiCred& c = WIFI_CREDS[cand[i].credIndex];
    Serial.printf("Trying: '%s' (rssi=%d)\n", cand[i].ssid.c_str(), cand[i].rssi);
    if (connectWithTimeout(c.ssid, c.pass, connectTimeoutMsPerTry)) {
      return true;
    }
  }

  Serial.println("Tried all known networks, none connected.");
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


  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["mode"] = (int)WiFi.getMode();
  wifi["status"] = (int)WiFi.status();
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  wifi["gateway"] = WiFi.isConnected() ? WiFi.gatewayIP().toString() : "";
  wifi["subnet"] = WiFi.isConnected() ? WiFi.subnetMask().toString() : "";
  wifi["mac"] = WiFi.macAddress();

  sendJson(request, 200, doc);
}

static void handleNotFound(AsyncWebServerRequest* request) {
  // If a static file exists, let it be served; otherwise JSON 404.
  if (LittleFS.exists(request->url())) {
    request->send(LittleFS, request->url());
    return;
  }
  sendError(request, 404, "not_found");
}


static void setupWifi() {
    bool ok = connectFirstAvailableKnown(/*connectTimeoutMsPerTry=*/15000);

  if (!ok) {
    Serial.println("WiFi NOT connected (no known networks or all failed).");
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

static void setupFs() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
  } else {
    Serial.println("LittleFS mounted.");
  Serial.println("LittleFS files:");
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    Serial.printf("  %s (%u bytes)\n", f.name(), (unsigned)f.size());
    f = root.openNextFile();
    }
  }
}

static void setupRoutes() {

  server.on("/health", HTTP_GET, handleHealth);
  server.on("/info", HTTP_GET, handleInfo);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });


  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=300");

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  setupFs();
  setupWifi();
  setupMdns();

  setupRoutes();

  server.begin();
  Serial.println("Async web server started on port 80");
}

void loop() {
  // nothing - async server
}
