#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace {
constexpr char AP_NAME[] = "Chaster-ESP32";
constexpr char AP_PASSWORD[] = "chaster32"; // Change before deployment.
constexpr char API_BASE[] = "https://api.chaster.app";
constexpr uint32_t REFRESH_MS = 30000;

WebServer server(80);
Preferences prefs;
String wifiSsid, wifiPassword, apiToken, lockId, dashboardPassword, viewMode = "wearer";
String lastJson = "{}";
String lastError;
uint32_t lastRefresh = 0;
bool stationConnected = false;

// TLS note: the ESP32 Arduino core does not ship a portable system trust store.
// This starter uses insecure TLS to keep API connectivity functional. Do not expose
// it to untrusted networks; replace this with the current Chaster CA chain before
// production deployment.

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

bool requireLogin() {
  if (dashboardPassword.length() == 0) return true;
  if (server.authenticate("admin", dashboardPassword.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Chaster ESP32", "Login required");
  return false;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &message) {
  JsonDocument d; d["ok"] = false; d["error"] = message;
  String out; serializeJson(d, out); sendJson(code, out);
}

bool apiRequest(const String &method, const String &path, const String &body, String &response) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi is not connected."; return false; }
  if (apiToken.length() == 0) { lastError = "No Chaster token configured."; return false; }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(API_BASE) + path)) {
    lastError = "Could not start HTTPS connection to Chaster.";
    return false;
  }
  http.setTimeout(15000);
  http.addHeader("Authorization", "Bearer " + apiToken);
  http.addHeader("Accept", "application/json");
  int code = -1;
  if (method == "GET") code = http.GET();
  else {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  }
  response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    lastError = "Chaster API HTTP " + String(code) + ": " + response;
    return false;
  }
  lastError = "";
  return true;
}

void refreshLock() {
  String response;
  String path = lockId.length() == 0 ? "/locks" : "/locks/" + lockId;
  bool ok;
  if (lockId.length() == 0 && viewMode == "keyholder") {
    ok = apiRequest("POST", "/keyholder/locks/search", R"({"limit":50,"page":0,"status":"locked"})", response);
  } else {
    ok = apiRequest("GET", path, "", response);
  }
  if (ok) {
    lastJson = response;
    lastRefresh = millis();
  }
}

String dashboardPage() {
  String page = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chaster ESP32</title><style>
:root{color-scheme:dark}body{margin:0;background:#0c1220;color:#e9eef8;font:16px system-ui;padding:18px}
main{max-width:780px;margin:auto}section{background:#172238;border:1px solid #2b3b59;border-radius:14px;padding:18px;margin:14px 0}
h1{margin:8px 0 18px}h2{font-size:18px;margin-top:0}label{display:block;margin:10px 0 5px;color:#b7c5dc}
input,button{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #425574;background:#0d1729;color:#fff;margin:5px 0}
button{background:#315fe7;border:0;font-weight:700;cursor:pointer}button.secondary{background:#273650}button.danger{background:#8d3445}
pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:420px;overflow:auto;background:#0b1425;padding:12px;border-radius:8px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}.muted{color:#9eacc4;font-size:13px}.status{padding:10px;border-radius:8px;background:#0d1729}
@media(max-width:520px){.grid{grid-template-columns:1fr}}
</style></head><body><main><h1>Chaster ESP32</h1>
<div class="status" id="status">Loading device status…</div>
<section><h2>Lock dashboard</h2>
<div class="grid"><button onclick="refresh()">Refresh now</button><button class="secondary" onclick="loadState()">Reload dashboard</button></div>
<div id="lockSummary" class="muted">No lock data loaded.</div><div id="timer" class="status">Time passed: — · Time remaining: —</div>
<label for="lockSelect">Choose a lock from your account</label><select id="lockSelect"><option value="">All locks</option></select><button class="secondary" onclick="chooseLock()">Open selected lock</button>
<h3>Raw Chaster response</h3><pre id="json">Loading…</pre>
</section>
<section><h2>Lock history</h2><button class="secondary" onclick="loadHistory()">Load recent history</button><pre id="history">Choose a lock to view its recent history.</pre></section>
<section><h2>Lock controls</h2>
<p class="muted">Freeze and unfreeze use Chaster's documented lock endpoint. Chaster checks your token scope and the lock's permissions.</p>
<div class="grid"><button onclick="setFreeze(true)">Freeze lock</button><button onclick="setFreeze(false)">Unfreeze lock</button></div>
<div id="actionMsg" class="status">Choose a control to send a request.</div>
<p class="muted">Add/remove time is not enabled in this build: the current Public API OpenAPI schema defines UpdateTimeDto with no fields, so it does not document a duration or direction payload.</p>
</section>
<section><h2>Device settings</h2>
<form id="config">
<label>Wi-Fi network name</label><input name="ssid" autocomplete="off" required>
<label>Wi-Fi password (leave blank to keep current)</label><input name="wifiPassword" type="password" autocomplete="new-password">
<label>Chaster developer/access token</label><input name="token" type="password" autocomplete="new-password">
<label>Account view</label><select name="mode"><option value="wearer">My locks</option><option value="keyholder">Locks I keyhold</option></select>
<label>Lock ID (leave blank to show your lock list)</label><input name="lockId" autocomplete="off">
<label>Local dashboard password (required)</label><input name="dashboardPassword" type="password" minlength="10" required>
<button type="submit">Save settings and restart</button>
</form>
<p class="muted">The dashboard uses HTTP Basic Authentication on your local network. The Chaster token is stored in ESP32 NVS and is never displayed back in the page.</p>
</section>
<section><h2>Hand over / reset</h2>
<p class="muted">Reset clears the stored Wi-Fi credentials, Chaster token, lock ID and dashboard password.</p>
<button class="danger" onclick="resetDevice()">Clear all credentials and restart setup</button>
</section></main>
<script>
async function call(url,opts={}){const r=await fetch(url,opts);const t=await r.text();let d;try{d=JSON.parse(t)}catch(e){throw new Error(t||('HTTP '+r.status))}if(!r.ok)throw new Error(d.error||('HTTP '+r.status));return d}
function show(id,msg){document.getElementById(id).textContent=msg}
async function loadState(){try{const x=await call('/api/state');show('status',x.apiError?x.apiError:(x.connected?'Wi-Fi connected · '+x.ip:'Setup access point active · connect to Chaster-ESP32'));show('json',JSON.stringify(x.data,null,2));const d=x.data;let summary='';const sel=document.getElementById('lockSelect');const lockList=Array.isArray(d)?d:(d&&Array.isArray(d.locks)?d.locks:null);const current=lockList?'':(d&&d._id?d._id:'');if(lockList){startMs=0;endMs=0;renderTimer();summary='Found '+lockList.length+' lock(s).';const previous=sel.value;sel.innerHTML='<option value="">All locks</option>';lockList.forEach(lock=>{if(!lock||!lock._id)return;const o=document.createElement('option');o.value=lock._id;o.textContent=(lock.title||lock._id)+' · '+(lock.status||'');sel.appendChild(o)});sel.value=previous;}else if(d&&typeof d==='object'){summary='Lock: '+(d.title||d._id||'loaded')+' · Status: '+(d.status||'unknown')+' · Frozen: '+(d.isFrozen===true?'yes':d.isFrozen===false?'no':'unknown');startMs=Date.parse(d.startDate||'');if(!Number.isFinite(startMs))startMs=0;endMs=Date.parse(d.endDate||'');if(!Number.isFinite(endMs))endMs=0;renderTimer();if(current){sel.innerHTML='';const o=document.createElement('option');o.value=current;o.textContent=(d.title||current)+' · '+(d.status||'');sel.appendChild(o);}}show('lockSummary',summary||'Chaster response received.');}catch(e){show('status',e.message);}}
async function refresh(){try{await call('/api/refresh',{method:'POST'});await loadState();}catch(e){show('actionMsg',e.message)}}
async function setFreeze(value){if(!confirm((value?'Freeze':'Unfreeze')+' this Chaster lock?'))return;try{const x=await call('/api/freeze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({isFrozen:value})});show('actionMsg',x.message||'Chaster accepted the request.');await loadState();}catch(e){show('actionMsg',e.message)}}
document.getElementById('config').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target);const p={ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),token:f.get('token'),lockId:f.get('lockId'),dashboardPassword:f.get('dashboardPassword'),mode:f.get('mode')};try{await call('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});show('status','Settings saved. Restarting device…');}catch(err){show('status',err.message)}});
async function resetDevice(){if(!confirm('Erase all stored credentials and restart?'))return;try{await call('/api/reset',{method:'POST'});show('status','Credentials cleared. Device restarting.');}catch(e){show('status',e.message)}}
loadState();setInterval(loadState,30000);setInterval(renderTimer,1000);
</script></body></html>)HTML";
  return page;
}

void handleRoot() {
  if (!requireLogin()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", dashboardPage());
}

void handleState() {
  if (!requireLogin()) return;
  JsonDocument d;
  d["ok"] = true;
  d["connected"] = WiFi.status() == WL_CONNECTED;
  d["apiError"] = lastError;
  d["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  JsonDocument data;
  if (deserializeJson(data, lastJson)) data["raw"] = lastJson;
  d["data"].set(data.as<JsonVariantConst>());
  String out; serializeJson(d, out); sendJson(200, out);
}

void handleRefresh() {
  if (!requireLogin()) return;
  refreshLock();
  if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}

void handleFreeze() {
  if (!requireLogin()) return;
  if (lockId.length() == 0) { sendError(400, "Set a Lock ID in device settings first."); return; }
  JsonDocument input;
  if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["isFrozen"].is<bool>()) { sendError(400, "isFrozen must be true or false."); return; }
  JsonDocument body;
  body["isFrozen"] = input["isFrozen"].as<bool>();
  String payload; serializeJson(body, payload);
  String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/freeze", payload, response)) {
    sendError(502, lastError); return;
  }
  refreshLock();
  sendJson(200, "{\"ok\":true,\"message\":\"Chaster accepted the freeze-state request.\"}");
}

void handleSelectLock() {
  if (!requireLogin()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String selected = d["lockId"] | "";
  if (selected.length() == 0) { sendError(400, "Lock ID is required."); return; }
  lockId = selected;
  prefs.begin("chaster", false); prefs.putString("lock_id", lockId); prefs.end();
  refreshLock();
  if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}

void handleHistory() {
  if (!requireLogin()) return;
  if (lockId.length() == 0) { sendError(400, "Choose a specific lock first."); return; }
  String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/history", "{\"limit\":20}", response)) {
    sendError(502, lastError); return;
  }
  JsonDocument result;
  if (deserializeJson(result, response)) { sendError(502, "Chaster returned invalid history JSON."); return; }
  JsonDocument out;
  out["ok"] = true;
  out["data"].set(result.as<JsonVariantConst>());
  String body; serializeJson(out, body); sendJson(200, body);
}

void handleConfig() {
  if (!requireLogin()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String ssid = d["ssid"] | "";
  String pass = d["wifiPassword"] | "";
  String token = d["token"] | "";
  String newLock = d["lockId"] | "";
  String newDash = d["dashboardPassword"] | "";
  String newMode = d["mode"] | "wearer";
  if (ssid.length() == 0) { sendError(400, "Wi-Fi SSID is required."); return; }
  if (newMode != "wearer" && newMode != "keyholder") { sendError(400, "Invalid account view."); return; }
  if (token.length() == 0 && apiToken.length() == 0) { sendError(400, "Chaster token is required."); return; }
  if (newDash.length() < 10) { sendError(400, "Dashboard password must be at least 10 characters."); return; }
  prefs.begin("chaster", false);
  prefs.putString("ssid", ssid);
  if (pass.length() > 0) prefs.putString("wifi_pw", pass);
  if (token.length() > 0) prefs.putString("token", token);
  prefs.putString("lock_id", newLock);
  prefs.putString("dash_pw", newDash);
  prefs.putString("mode", newMode);
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(700);
  ESP.restart();
}

void handleReset() {
  if (!requireLogin()) return;
  prefs.begin("chaster", false); prefs.clear(); prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}

void startSetupAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME, AP_PASSWORD);
}

void connectWiFi() {
  if (wifiSsid.length() == 0) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) delay(250);
  stationConnected = WiFi.status() == WL_CONNECTED;
  if (!stationConnected) startSetupAP();
}

void loadConfig() {
  prefs.begin("chaster", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("wifi_pw", "");
  apiToken = prefs.getString("token", "");
  lockId = prefs.getString("lock_id", "");
  dashboardPassword = prefs.getString("dash_pw", "");
  viewMode = prefs.getString("mode", "wearer");
  prefs.end();
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  loadConfig();
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) startSetupAP();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/freeze", HTTP_POST, handleFreeze);
  server.on("/api/select-lock", HTTP_POST, handleSelectLock);
  server.on("/api/history", HTTP_POST, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();

  if (WiFi.status() == WL_CONNECTED && apiToken.length() > 0) refreshLock();
  Serial.println("Chaster ESP32 dashboard ready.");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && !apiToken.length() == 0 && millis() - lastRefresh >= REFRESH_MS) refreshLock();
  delay(2);
}
