#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static const char* WIFI_SSID = "SSID_HERE";
static const char* WIFI_PASS = "PASSWORD_HERE";
static const char* MDNS_NAME = "neonrift";  // http://neonrift.local

AsyncWebServer server(80);


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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi NOT connected (timeout).");
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
