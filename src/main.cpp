#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace {
constexpr char AP_NAME[] = "Chaster-ESP32";
constexpr char AP_PASSWORD[] = "chaster32";
constexpr char API_BASE[] = "https://api.chaster.app";
constexpr uint32_t REFRESH_MS = 30000;

WebServer server(80);
Preferences prefs;
String wifiSsid, wifiPassword, wearerToken, holderToken, lockId, dashboardPassword;
String role = "wearer", lastJson = "{}", lastError;
uint32_t lastRefresh = 0;
bool handoverPending = false;

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;"); return s;
}
String activeToken() { return role == "keyholder" ? holderToken : wearerToken; }

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
  String token = activeToken();
  if (!token.length()) { lastError = "No Chaster API token configured for this role."; return false; }
  WiFiClientSecure client;
  // TODO: replace setInsecure() with Chaster's validated CA chain before production use.
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(API_BASE) + path)) { lastError = "Could not start HTTPS connection to Chaster."; return false; }
  http.setTimeout(15000);
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/json");
  int code = -1;
  if (method == "GET") code = http.GET();
  else { http.addHeader("Content-Type", "application/json"); code = http.POST(body); }
  response = http.getString(); http.end();
  if (code < 200 || code >= 300) { lastError = "Chaster API HTTP " + String(code) + ": " + response; return false; }
  lastError = ""; return true;
}
void refreshLock() {
  lastRefresh = millis();
  String response;
  bool ok = false;
  if (lockId.length()) ok = apiRequest("GET", "/locks/" + lockId, "", response);
  else if (role == "keyholder") ok = apiRequest("POST", "/keyholder/locks/search", R"({"limit":50,"page":0,"status":"locked"})", response);
  else ok = apiRequest("GET", "/locks", "", response);
  if (ok) { lastJson = response; lastRefresh = millis(); }
}
String dashboardPage() {
  return R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101827"><title>Chaster ESP32</title><style>
:root{color-scheme:dark;--bg:#0b1220;--card:#131f33;--line:#283953;--text:#edf3ff;--muted:#9eacc4;--accent:#647cff;--field:#0d1729}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px system-ui;padding:20px}
main{max-width:850px;margin:auto}h1{font-size:30px;letter-spacing:-1px;margin:8px 0 20px}h2{font-size:18px;margin:0 0 14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:20px;margin:14px 0}
label{display:block;color:var(--muted);font-size:13px;margin:12px 0 5px}
input,select,button{width:100%;padding:12px;border-radius:9px;border:1px solid #40516d;background:var(--field);color:var(--text);font:inherit;margin:4px 0}
button{background:var(--accent);border:0;font-weight:700;cursor:pointer}button.secondary{background:#263750}button.danger{background:#853b4b}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:9px}.muted{color:var(--muted);font-size:13px}.notice{padding:12px;background:#0d192b;border:1px solid var(--line);border-radius:10px;margin-top:10px}
.status{padding:12px;background:#0d192b;border-radius:10px;margin:8px 0}.time{font-size:29px;font-weight:700;font-variant-numeric:tabular-nums}
.hidden{display:none!important}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:300px;overflow:auto;background:#0d192b;padding:12px;border-radius:10px}
@media(max-width:560px){body{padding:12px}.grid{grid-template-columns:1fr}.card{padding:16px}}
</style></head><body><main><h1>Chaster ESP32</h1><div id="status" class="status">Connecting…</div>
<section id="setup" class="card hidden"><h2 id="setupTitle">Device setup</h2><div id="handoverInfo" class="notice hidden">Handover is ready. Your wearer's Chaster token remains stored privately on this device. Enter your own Wi-Fi and Chaster API token below.</div>
<form id="config"><label>Wi-Fi network name</label><input name="ssid" required autocomplete="off">
<label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password">
<label id="tokenLabel">Chaster API token</label><input name="token" type="password" required autocomplete="new-password">
<div id="dashPasswordField"><label>Local dashboard password (10+ characters)</label><input name="dashboardPassword" type="password" minlength="10" autocomplete="new-password"></div>
<button type="submit" id="saveBtn">Save and connect</button></form><p class="muted">The token is stored in ESP32 NVS and is never displayed again.</p></section>

<section id="dashboard" class="hidden">
<div class="card"><div class="muted" id="roleName">WEARER MODE</div><div class="muted">TIME ELAPSED</div><div class="time" id="elapsed">—</div><div class="muted">TIME REMAINING</div><div class="time" id="remaining">—</div><div id="lockSummary" class="status">No lock selected.</div></div>
<div class="card"><h2>Lock selection</h2><select id="lockSelect"><option value="">All locks</option></select><div class="grid"><button class="secondary" onclick="chooseLock()">Open selected lock</button><button class="secondary" onclick="showLockList()">Back to lock list</button></div><button class="secondary" onclick="refresh()">Refresh now</button><pre id="lockData">Loading Chaster data…</pre></div>
<div class="card"><h2>Lock controls</h2><div class="grid"><button onclick="freeze(true)">Freeze</button><button onclick="freeze(false)">Unfreeze</button></div><label>Hours</label><input id="hours" type="number" min="0" max="8760" value="1"><label>Minutes</label><input id="minutes" type="number" min="0" max="59" value="0"><div class="grid"><button onclick="changeTime(true)">Add time</button><button class="danger" onclick="changeTime(false)">Remove time</button></div><div id="actionMsg" class="notice">Actions are sent to Chaster using the token for the active role.</div></div>
<div class="card"><h2>Lock history</h2><button class="secondary" onclick="loadHistory()">Load recent history</button><pre id="history">Choose a lock to view its history.</pre></div>
<div id="handoverCard" class="card"><h2>Hand Over</h2><p class="muted">Preserve the wearer token privately, clear the current Wi-Fi and previous keyholder token, and restart the setup access point so the keyholder can enter their own Wi-Fi and Chaster token.</p><button class="danger" onclick="handover()">Hand Over to Keyholder</button></div>
<div class="card"><h2>Device</h2><p class="muted">ESP32 dashboard settings and credential reset.</p><button class="danger" id="resetButton" onclick="resetDevice()">Clear all credentials</button></div>
</section></main><script>
const $=id=>document.getElementById(id);let role='wearer',lockStart=0,lockEnd=0;
async function call(url,opts={}){const r=await fetch(url,opts);const t=await r.text();let d;try{d=JSON.parse(t)}catch(e){throw new Error(t||('HTTP '+r.status))}if(!r.ok)throw new Error(d.error||('HTTP '+r.status));return d}
function msg(id,s){$(id).textContent=s}
function dur(ms){if(!Number.isFinite(ms))return '—';let n=Math.max(0,Math.floor(ms/1000));let d=Math.floor(n/86400);n%=86400;let h=Math.floor(n/3600);n%=3600;return d+'d '+h+'h '+Math.floor(n/60)+'m '+(n%60)+'s'}
function timer(){msg('elapsed',lockStart?dur(Date.now()-lockStart):'—');msg('remaining',lockEnd?dur(lockEnd-Date.now()):'—')}
function lockArray(d){return Array.isArray(d)?d:(d&&Array.isArray(d.locks)?d.locks:null)}
function renderData(d){$('lockData').textContent=JSON.stringify(d,null,2);const arr=lockArray(d),sel=$('lockSelect'),old=sel.value; if(arr){sel.innerHTML='<option value="">All locks</option>';arr.forEach(x=>{if(!x||!x._id)return;let o=document.createElement('option');o.value=x._id;o.textContent=(x.title||x._id)+' · '+(x.status||'');sel.appendChild(o)});sel.value=old;lockStart=0;lockEnd=0;msg('lockSummary','Found '+arr.length+' lock(s). Select one to manage it.');}else if(d&&typeof d==='object'){const id=d._id||d.id||'';lockStart=Date.parse(d.startDate||d.startAt||'')||0;lockEnd=Date.parse(d.endDate||d.endAt||'')||0;msg('lockSummary',(d.title||id||'Selected lock')+' · '+(d.status||'status unknown')+' · Frozen: '+(d.isFrozen===true?'yes':d.isFrozen===false?'no':'unknown'));if(id){sel.innerHTML='';let o=document.createElement('option');o.value=id;o.textContent=d.title||id;sel.appendChild(o)}}timer()}
async function loadState(){try{const x=await call('/api/state');role=x.role||'wearer';if(!x.configured||x.handoverPending){$('setup').classList.remove('hidden');$('dashboard').classList.add('hidden');$('handoverInfo').classList.toggle('hidden',!x.handoverPending);$('setupTitle').textContent=x.handoverPending?'Keyholder setup':'Wearer setup';$('tokenLabel').textContent=x.handoverPending?'Your Chaster keyholder API token':'Wearer Chaster API token';$('dashPasswordField').classList.toggle('hidden',!!x.handoverPending);msg('status',x.handoverPending?'Handover setup · connect to Chaster-ESP32':'Setup access point active · connect to Chaster-ESP32');return}
$('setup').classList.add('hidden');$('dashboard').classList.remove('hidden');$('roleName').textContent=role==='keyholder'?'KEYHOLDER MODE':'WEARER MODE';$('handoverCard').classList.toggle('hidden',role!=='wearer');$('resetButton').classList.toggle('hidden',role==='keyholder');msg('status',x.connected?'Connected · '+x.ip:(x.apiError||'Wi-Fi disconnected'));renderData(x.data||{});}catch(e){msg('status',e.message)}}
async function refresh(){try{await call('/api/refresh',{method:'POST'});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function chooseLock(){let id=$('lockSelect').value;if(!id){msg('actionMsg','Select a specific lock first.');return}try{await call('/api/select-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({lockId:id})});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function showLockList(){try{await call('/api/select-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({lockId:''})});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function freeze(value){if(!confirm((value?'Freeze':'Unfreeze')+' this lock?'))return;try{const x=await call('/api/freeze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({isFrozen:value})});msg('actionMsg',x.message||'Chaster accepted the request.');await refresh()}catch(e){msg('actionMsg',e.message)}}
async function changeTime(add){let h=Number($('hours').value),m=Number($('minutes').value);if(!Number.isInteger(h)||!Number.isInteger(m)||h<0||m<0||m>59||(!h&&!m)){msg('actionMsg','Enter a positive duration.');return}let sec=(h*3600+m*60)*(add?1:-1);if(!confirm((add?'Add ':'Remove ')+h+'h '+m+'m?'))return;try{const x=await call('/api/update-time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration:sec})});msg('actionMsg',x.message||'Chaster accepted the request.');await refresh()}catch(e){msg('actionMsg',e.message)}}
async function loadHistory(){try{let x=await call('/api/history',{method:'POST'});msg('history',JSON.stringify(x.data,null,2))}catch(e){msg('history',e.message)}}
async function handover(){if(!confirm('Hand over this device? The wearer token will remain stored privately. Wi-Fi and any keyholder token will be cleared.'))return;try{await call('/api/handover',{method:'POST'});msg('status','Handover started. Connect to the Chaster-ESP32 setup network.');setTimeout(loadState,1000)}catch(e){msg('actionMsg',e.message)}}
async function resetDevice(){if(!confirm('Erase all Chaster credentials and Wi-Fi settings?'))return;try{await call('/api/reset',{method:'POST'});msg('status','Credentials cleared. Restarting…')}catch(e){msg('status',e.message)}}
$('config').addEventListener('submit',async e=>{e.preventDefault();let f=new FormData(e.target),p={ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),token:f.get('token'),dashboardPassword:f.get('dashboardPassword')};try{await call('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});msg('status','Saved. Restarting…')}catch(err){msg('status',err.message)}});
loadState();setInterval(()=>{if(!$('dashboard').classList.contains('hidden'))loadState()},30000);setInterval(timer,1000);
</script></body></html>)HTML";
}

void handleRoot() {
  if (!requireLogin()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", dashboardPage());
}
void handleState() {
  if (!requireLogin()) return;
  JsonDocument d;
  bool configured = role == "wearer" ? wearerToken.length() > 0 : holderToken.length() > 0;
  configured = configured && wifiSsid.length() > 0 && !handoverPending;
  d["ok"] = true; d["configured"] = configured; d["handoverPending"] = handoverPending;
  d["role"] = role; d["connected"] = WiFi.status() == WL_CONNECTED;
  d["lockId"] = lockId; d["apiError"] = lastError;
  d["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  JsonDocument data; if (deserializeJson(data, lastJson)) data["raw"] = lastJson;
  d["data"].set(data.as<JsonVariantConst>());
  String out; serializeJson(d, out); sendJson(200, out);
}
void handleRefresh() {
  if (!requireLogin()) return;
  refreshLock(); if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}
void handleFreeze() {
  if (!requireLogin()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["isFrozen"].is<bool>()) { sendError(400, "isFrozen must be true or false."); return; }
  JsonDocument body; body["isFrozen"] = input["isFrozen"].as<bool>();
  String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/freeze", payload, response)) { sendError(502, lastError); return; }
  refreshLock(); sendJson(200, "{\"ok\":true,\"message\":\"Chaster accepted the freeze-state request.\"}");
}
void handleUpdateTime() {
  if (!requireLogin()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["duration"].is<long>()) { sendError(400, "duration must be signed seconds."); return; }
  long duration = input["duration"].as<long>();
  if (!duration || duration < -31536000L || duration > 31536000L) { sendError(400, "Duration must be between -365 and 365 days."); return; }
  JsonDocument body; body["duration"] = duration; String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/update-time", payload, response)) { sendError(502, lastError); return; }
  refreshLock(); JsonDocument out; out["ok"] = true;
  out["message"] = duration > 0 ? "Chaster accepted the add-time request." : "Chaster accepted the remove-time request.";
  String result; serializeJson(out, result); sendJson(200, result);
}
void handleSelectLock() {
  if (!requireLogin()) return;
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  lockId = d["lockId"] | "";
  prefs.begin("chaster", false); prefs.putString("lock_id", lockId); prefs.end();
  refreshLock(); if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}
void handleHistory() {
  if (!requireLogin()) return;
  if (!lockId.length()) { sendError(400, "Choose a specific lock first."); return; }
  String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/history", "{\"limit\":20}", response)) { sendError(502, lastError); return; }
  JsonDocument result; if (deserializeJson(result, response)) { sendError(502, "Chaster returned invalid history JSON."); return; }
  JsonDocument out; out["ok"] = true; out["data"].set(result.as<JsonVariantConst>());
  String body; serializeJson(out, body); sendJson(200, body);
}
void handleConfig() {
  if (!requireLogin()) return;
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String ssid = d["ssid"] | ""; String pass = d["wifiPassword"] | ""; String token = d["token"] | "";
  if (!ssid.length() || !token.length()) { sendError(400, "Wi-Fi SSID and Chaster token are required."); return; }
  prefs.begin("chaster", false);
  if (handoverPending) {
    prefs.putString("holder_token", token); prefs.putString("ssid", ssid);
    if (pass.length()) prefs.putString("wifi_pw", pass);
    prefs.putString("role", "keyholder"); prefs.putBool("handover", false);
  } else {
    String dash = d["dashboardPassword"] | "";
    if (dash.length() < 10) { prefs.end(); sendError(400, "Dashboard password must be at least 10 characters."); return; }
    prefs.putString("wearer_token", token); prefs.putString("ssid", ssid);
    if (pass.length()) prefs.putString("wifi_pw", pass);
    prefs.putString("dash_pw", dash); prefs.putString("role", "wearer");
  }
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}
void handleHandover() {
  if (!requireLogin()) return;
  if (role != "wearer" || !wearerToken.length() || handoverPending) { sendError(409, "Hand Over is available after wearer setup."); return; }
  prefs.begin("chaster", false);
  prefs.putString("role", "keyholder"); prefs.remove("ssid"); prefs.remove("wifi_pw");
  prefs.remove("holder_token"); prefs.putBool("handover", true); prefs.remove("lock_id");
  prefs.end();
  sendJson(200, "{\"ok\":true,\"handoverPending\":true}");
  delay(500); ESP.restart();
}
void handleReset() {
  if (!requireLogin()) return;
  prefs.begin("chaster", false); prefs.clear(); prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void startSetupAP() { WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME, AP_PASSWORD); }
void connectWiFi() {
  if (!wifiSsid.length()) return;
  WiFi.mode(WIFI_STA); WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t started = millis(); while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) delay(250);
  if (WiFi.status() != WL_CONNECTED) startSetupAP();
}
void loadConfig() {
  prefs.begin("chaster", true);
  wifiSsid = prefs.getString("ssid", ""); wifiPassword = prefs.getString("wifi_pw", "");
  wearerToken = prefs.getString("wearer_token", "");
  holderToken = prefs.getString("holder_token", "");
  // Migrate installations made by the earlier single-token firmware.
  if (!wearerToken.length()) wearerToken = prefs.getString("token", "");
  lockId = prefs.getString("lock_id", ""); dashboardPassword = prefs.getString("dash_pw", "");
  role = prefs.getString("role", "wearer"); handoverPending = prefs.getBool("handover", false);
  prefs.end();
}
} // namespace

void setup() {
  Serial.begin(115200); delay(250); loadConfig(); connectWiFi();
  if (WiFi.status() != WL_CONNECTED) startSetupAP();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/freeze", HTTP_POST, handleFreeze);
  server.on("/api/update-time", HTTP_POST, handleUpdateTime);
  server.on("/api/select-lock", HTTP_POST, handleSelectLock);
  server.on("/api/history", HTTP_POST, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/handover", HTTP_POST, handleHandover);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  if (WiFi.status() == WL_CONNECTED && activeToken().length()) refreshLock();
  Serial.println("Chaster ESP32 dashboard ready.");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}
void loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && activeToken().length() && !handoverPending && millis() - lastRefresh >= REFRESH_MS) refreshLock();
  delay(2);
}
