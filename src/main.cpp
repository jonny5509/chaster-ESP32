#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace {
constexpr char AP_NAME[] = "Chaster-ESP32-Setup";
constexpr char API_BASE[] = "https://api.chaster.app";
constexpr uint32_t REFRESH_MS = 30000;
WebServer server(80);
Preferences prefs;
String wifiSsid, wifiPassword, apiToken, lockId;
String lastApiResponse = "Not queried yet";
String lastError;
uint32_t lastRefresh = 0;
bool connected = false;

String escapeHtml(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String page() {
  String s = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>Chaster ESP32</title><style>body{font:16px system-ui;background:#10151d;color:#edf2f7;margin:0;padding:22px}";
  s += "main{max-width:720px;margin:auto}section{background:#1b2532;padding:18px;border-radius:14px;margin:14px 0}";
  s += "input,button{box-sizing:border-box;width:100%;padding:12px;margin:6px 0;border-radius:8px;border:1px solid #536273;background:#0e1722;color:white}";
  s += "button{background:#2563eb;border:0;font-weight:700}pre{white-space:pre-wrap;overflow-wrap:anywhere}</style></head><body><main>";
  s += "<h1>Chaster ESP32</h1><p>" + String(connected ? "Wi-Fi connected" : "Setup access point") + "</p>";
  s += "<section><h2>Connection</h2><form method='POST' action='/save'>";
  s += "<label>Wi-Fi name</label><input name='ssid' required>";
  s += "<label>Wi-Fi password</label><input name='password' type='password'>";
  s += "<label>Chaster developer/access token</label><input name='token' type='password' required>";
  s += "<label>Lock ID (optional)</label><input name='lock' value='" + escapeHtml(lockId) + "'>";
  s += "<button>Save and connect</button></form><p><small>Token is stored in ESP32 NVS. Keep the setup network private.</small></p></section>";
  s += "<section><h2>Lock data</h2><p>" + escapeHtml(lastError.length() ? lastError : "Latest API response") + "</p><pre>";
  s += escapeHtml(lastApiResponse) + "</pre><form method='POST' action='/refresh'><button>Refresh from Chaster</button></form></section>";
  s += "<section><h2>Hand over device</h2><p>This clears saved Wi-Fi and API credentials.</p>";
  s += "<form method='POST' action='/handover'><button>Clear credentials and restart setup</button></form></section>";
  s += "<p><small>Time-changing controls are not enabled until the current Chaster update-time request schema is verified.</small></p></main></body></html>";
  return s;
}

bool apiGet(const String &path, String &response) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi is not connected."; return false; }
  if (apiToken.isEmpty()) { lastError = "No Chaster API token configured."; return false; }
  WiFiClientSecure client;
  // TODO: replace insecure mode with certificate validation before release.
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(API_BASE) + path)) { lastError = "HTTPS initialisation failed."; return false; }
  http.addHeader("Authorization", "Bearer " + apiToken);
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    lastError = "Chaster API HTTP " + String(code) + ". Check token, scopes and lock ID.";
    return false;
  }
  lastError = "";
  return true;
}

void refreshLock() {
  String response;
  String path = lockId.isEmpty() ? "/locks" : "/locks/" + lockId;
  if (apiGet(path, response)) { lastApiResponse = response; lastRefresh = millis(); }
}

void setupRoutes() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", page()); });
  server.on("/save", HTTP_POST, [](){
    prefs.begin("chaster", false);
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("wifi_pw", server.arg("password"));
    prefs.putString("token", server.arg("token"));
    prefs.putString("lock_id", server.arg("lock"));
    prefs.end();
    server.send(200, "text/plain", "Saved. Restarting...");
    delay(500); ESP.restart();
  });
  server.on("/refresh", HTTP_POST, [](){
    refreshLock(); server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/handover", HTTP_POST, [](){
    prefs.begin("chaster", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "Credentials cleared. Restarting.");
    delay(500); ESP.restart();
  });
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
}

void startAP() { WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME); }
void connectWiFi() {
  if (wifiSsid.isEmpty()) return;
  WiFi.mode(WIFI_STA); WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) delay(250);
  connected = WiFi.status() == WL_CONNECTED;
  if (!connected) startAP();
}
}

void setup() {
  Serial.begin(115200); delay(200);
  prefs.begin("chaster", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("wifi_pw", "");
  apiToken = prefs.getString("token", "");
  lockId = prefs.getString("lock_id", "");
  prefs.end();
  connectWiFi();
  if (!connected) startAP();
  setupRoutes(); server.begin();
  if (connected) refreshLock();
  Serial.println("Chaster ESP32 dashboard ready.");
  Serial.println(connected ? WiFi.localIP() : WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  if (connected && millis() - lastRefresh >= REFRESH_MS) refreshLock();
  delay(2);
}
