#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace {
constexpr char AP_NAME[] = "Chaster-ESP32";
constexpr char AP_PASSWORD[] = "chaster32";
constexpr char API_BASE[] = "https://api.chaster.app";
constexpr uint32_t REFRESH_MS = 30000;
const IPAddress AP_IP(192,168,4,1), AP_GATEWAY(192,168,4,1), AP_SUBNET(255,255,255,0);
bool setupAP = false;
void startSetupAP();

WebServer server(80);
DNSServer dnsServer;
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
bool requireAuth() {
  if (dashboardPassword.length()) {
    if (server.authenticate("admin", dashboardPassword.c_str())) return true;
    server.requestAuthentication(); return false;
  }
  // Legacy devices without a dashboard password may only view setup/state and set one.
  // Block every control/configuration endpoint until the password is provisioned.
  String uri = server.uri();
  bool provisioned = wearerToken.length() || holderToken.length();
  bool allowedWithoutPassword = !provisioned || uri == "/" || uri == "/api/state" ||
                                uri == "/api/set-password" || (handoverPending && uri == "/api/config");
  if (allowedWithoutPassword) return true;
  server.send(423, "application/json", "{\"ok\":false,\"error\":\"Set a dashboard password before using device controls.\"}");
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
  token.trim();
  // Accept either the raw developer token or a copied "Bearer <token>" value.
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
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
  if (code < 200 || code >= 300) {
    lastError = "Chaster API HTTP " + String(code) + ": " + response;
    if (code == 401) lastError = "Chaster rejected the " + role + " API token (401). Generate a fresh Chaster Public API developer token while signed into the account that owns this role, then re-enter it in setup. Check that the token is not expired or revoked. Response: " + response;
    else if (code == 403) {
      bool readOnly = method == "GET" || path.endsWith("/history") || path == "/keyholder/locks/search";
      String scope = role == "keyholder" ? (readOnly ? "keyholder:read" : "keyholder:write") : (readOnly ? "locks:read" : "locks:write");
      lastError = "Chaster denied this request (403). Check the active account token has the " + scope + " scope and that this lock permits the requested action for this role. Response: " + response;
    }
    else if (code == 404) lastError = "Chaster could not find this lock or endpoint (404). Check the selected lock ID and confirm it is accessible to the active Chaster account. Response: " + response;
    return false;
  }
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
<section id="setup" class="card hidden"><h2 id="setupTitle">Device setup</h2><div id="handoverInfo" class="notice hidden">Handover is ready. The wearer's developer token remains stored privately. Enter your Wi-Fi, developer token and the lock ID to use for BOTH wearer and keyholder below. This replaces the wearer's saved lock ID.</div>
<form id="config"><label>Wi-Fi network name</label><input name="ssid" required autocomplete="off">
<label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password">
<label id="tokenLabel">Chaster developer token</label><input name="token" type="password" required autocomplete="new-password">
<label>Chaster lock ID</label><input name="lockId" required autocomplete="off" placeholder="Keyholder setup sets the shared lock ID for both roles">
<label>Dashboard password (minimum 10 characters)</label><input name="dashboardPassword" type="password" minlength="10" autocomplete="new-password">

<button type="submit" id="saveBtn">Save and connect</button></form><p class="muted">Developer tokens are stored separately for each role. The keyholder's lock ID is shared by both roles and replaces the wearer's previous lock ID.</p></section>

<section id="dashboard" class="hidden">
<div class="card"><div class="muted" id="roleName">WEARER MODE</div><div class="muted">TIME ELAPSED</div><div class="time" id="elapsed">—</div><div class="muted">TIME REMAINING</div><div class="time" id="remaining">—</div><div id="lockSummary" class="status">No lock selected.</div></div>
<div class="card"><h2>Lock selection</h2><select id="lockSelect"><option value="">All locks</option></select><div class="grid"><button class="secondary" onclick="chooseLock()">Open selected lock</button><button class="secondary" onclick="showLockList()">Back to lock list</button></div><button class="secondary" onclick="refresh()">Refresh now</button><pre id="lockData">Loading Chaster data…</pre></div>
<div class="card"><h2>Lock controls</h2><div class="grid"><button onclick="freeze(true)">Freeze</button><button onclick="freeze(false)">Unfreeze</button></div><label>Hours</label><input id="hours" type="number" min="0" max="8760" value="1"><label>Minutes</label><input id="minutes" type="number" min="0" max="59" value="0"><div class="grid"><button onclick="changeTime(true)">Add time</button><button class="danger" onclick="changeTime(false)">Remove time</button></div><div id="actionMsg" class="notice">Actions are sent to Chaster using the token for the active role.</div></div>
<div class="card"><h2>Lock history</h2><button class="secondary" onclick="loadHistory()">Load recent history</button><pre id="history">Choose a lock to view its history.</pre></div>
<div id="handoverCard" class="card"><h2>Hand Over</h2><p class="muted">Preserve the wearer token privately, clear the current Wi-Fi and previous keyholder token, and restart setup so the keyholder can enter their own Wi-Fi and Chaster token. The keyholder will set the shared lock ID used by both roles.</p><button class="danger" onclick="handover()">Hand Over to Keyholder</button></div>
<div class="card"><h2>Wi-Fi connection</h2><p class="muted">Update the network if the router or password changes. The device will restart and reconnect.</p><form id="wifiForm"><label>Wi-Fi network name</label><input id="wifiSsid" name="ssid" required autocomplete="off"><label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password" placeholder="Leave blank for an open network or to clear the saved password"><button type="submit">Save Wi-Fi settings</button></form></div>
<div id="passwordSetup" class="card hidden"><h2>Secure dashboard</h2><p class="muted">Set a unique password of at least 10 characters to protect dashboard controls.</p><form id="passwordForm"><label>New dashboard password</label><input name="password" type="password" minlength="10" required autocomplete="new-password"><button type="submit">Save dashboard password</button></form></div>
<div class="card"><h2>Device</h2><p class="muted">ESP32 dashboard settings and credential reset.</p><form id="tokenUpdate" class="hidden"><label>Replace keyholder Chaster developer token</label><input name="token" type="password" required autocomplete="new-password" placeholder="Paste a fresh keyholder token"><button type="submit">Save keyholder token</button></form><button class="danger" id="resetButton" onclick="resetDevice()">Clear all credentials</button></div>
</section></main><script>
const $=id=>document.getElementById(id);let role='wearer',lockStart=0,lockEnd=0;
async function call(url,opts={}){const r=await fetch(url,opts);const t=await r.text();let d;try{d=JSON.parse(t)}catch(e){throw new Error(t||('HTTP '+r.status))}if(!r.ok)throw new Error(d.error||('HTTP '+r.status));return d}
function msg(id,s){$(id).textContent=s}
function dur(ms){if(!Number.isFinite(ms))return '—';let n=Math.max(0,Math.floor(ms/1000));let d=Math.floor(n/86400);n%=86400;let h=Math.floor(n/3600);n%=3600;return d+'d '+h+'h '+Math.floor(n/60)+'m '+(n%60)+'s'}
function timer(){msg('elapsed',lockStart?dur(Date.now()-lockStart):'—');msg('remaining',lockEnd?dur(lockEnd-Date.now()):'—')}
function lockArray(d){if(Array.isArray(d))return d;if(!d||typeof d!=='object')return null;for(const k of ['locks','items','results','data']){const v=d[k];if(Array.isArray(v))return v;if(v&&typeof v==='object'){const a=lockArray(v);if(a)return a}}return null}
function renderData(d){$('lockData').textContent=JSON.stringify(d,null,2);const arr=lockArray(d),sel=$('lockSelect'),old=sel.value; if(arr){sel.innerHTML='<option value="">All locks</option>';arr.forEach(x=>{if(!x)return;const id=x._id||x.id||x.lockId;if(!id)return;let o=document.createElement('option');o.value=id;o.textContent=(x.title||x.name||id)+' · '+(x.status||'');sel.appendChild(o)});sel.value=old;lockStart=0;lockEnd=0;msg('lockSummary','Found '+arr.length+' lock(s). Select one to manage it.');}else if(d&&typeof d==='object'){const id=d._id||d.id||d.lockId||'';lockStart=Date.parse(d.startDate||d.startAt||d.startedAt||'')||0;lockEnd=Date.parse(d.endDate||d.endAt||d.endsAt||'')||0;msg('lockSummary',(d.title||id||'Selected lock')+' · '+(d.status||'status unknown')+' · Frozen: '+(d.isFrozen===true?'yes':d.isFrozen===false?'no':'unknown'));if(id){sel.innerHTML='';let o=document.createElement('option');o.value=id;o.textContent=d.title||id;sel.appendChild(o)}}timer()}
async function loadState(){try{const x=await call('/api/state');role=x.role||'wearer';if(!x.configured||x.handoverPending){$('setup').classList.remove('hidden');$('dashboard').classList.add('hidden');$('handoverInfo').classList.toggle('hidden',!x.handoverPending);$('setupTitle').textContent=x.handoverPending?'Keyholder setup':'Wearer setup';$('tokenLabel').textContent=x.handoverPending?'Keyholder Chaster developer token':'Wearer Chaster developer token';msg('status',x.handoverPending?'Handover setup · connect to Chaster-ESP32':'Setup access point active · connect to Chaster-ESP32');return}
$('setup').classList.add('hidden');$('dashboard').classList.remove('hidden');$('roleName').textContent=role==='keyholder'?'KEYHOLDER MODE':'WEARER MODE';$('handoverCard').classList.toggle('hidden',role!=='wearer');$('resetButton').classList.toggle('hidden',role==='keyholder');$('tokenUpdate').classList.toggle('hidden',role!=='keyholder');$('passwordSetup').classList.toggle('hidden',!x.needsPassword);if(!$('wifiSsid').value)$('wifiSsid').value=x.wifiSsid||'';msg('status',x.apiError?((x.connected?'Wi-Fi connected · '+x.ip:'Wi-Fi disconnected')+' · Chaster: '+x.apiError):(x.connected?'Connected · '+x.ip:'Wi-Fi disconnected'));renderData(x.data||{});}catch(e){msg('status',e.message)}}
async function refresh(){try{await call('/api/refresh',{method:'POST'});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function chooseLock(){let id=$('lockSelect').value;if(!id){msg('actionMsg','Select a specific lock first.');return}try{await call('/api/select-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({lockId:id})});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function showLockList(){try{await call('/api/select-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({lockId:''})});await loadState()}catch(e){msg('actionMsg',e.message)}}
async function freeze(value){if(!confirm((value?'Freeze':'Unfreeze')+' this lock?'))return;try{const x=await call('/api/freeze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({isFrozen:value})});msg('actionMsg',x.message||'Chaster accepted the request.');await refresh()}catch(e){msg('actionMsg',e.message)}}
async function changeTime(add){let h=Number($('hours').value),m=Number($('minutes').value);if(!Number.isInteger(h)||!Number.isInteger(m)||h<0||m<0||m>59||(!h&&!m)){msg('actionMsg','Enter a positive duration.');return}let sec=(h*3600+m*60)*(add?1:-1);if(!confirm((add?'Add ':'Remove ')+h+'h '+m+'m?'))return;try{const x=await call('/api/update-time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration:sec})});msg('actionMsg',x.message||'Chaster accepted the request.');await refresh()}catch(e){msg('actionMsg',e.message)}}
async function loadHistory(){try{let x=await call('/api/history',{method:'POST'});msg('history',JSON.stringify(x.data,null,2))}catch(e){msg('history',e.message)}}
async function handover(){if(!confirm('Hand over this device? The wearer developer token and lock ID will remain stored privately. Wi-Fi and any keyholder credentials will be cleared.'))return;try{await call('/api/handover',{method:'POST'});msg('status','Handover started. Connect to the Chaster-ESP32 setup network.');setTimeout(loadState,1000)}catch(e){msg('actionMsg',e.message)}}
async function resetDevice(){if(!confirm('Erase all Chaster credentials and Wi-Fi settings?'))return;try{await call('/api/reset',{method:'POST'});msg('status','Credentials cleared. Restarting…')}catch(e){msg('status',e.message)}}
$('config').addEventListener('submit',async e=>{e.preventDefault();let f=new FormData(e.target),p={ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),token:f.get('token'),lockId:f.get('lockId'),dashboardPassword:f.get('dashboardPassword')};try{await call('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});msg('status','Saved. Restarting…')}catch(err){msg('status',err.message)}});
$('wifiForm').addEventListener('submit',async e=>{e.preventDefault();let f=new FormData(e.target);try{await call('/api/update-wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword')})});msg('status','Wi-Fi settings saved. Restarting…')}catch(err){msg('status',err.message)}});
$('passwordForm').addEventListener('submit',async e=>{e.preventDefault();let password=new FormData(e.target).get('password');try{await call('/api/set-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password})});msg('status','Dashboard password saved. Restarting…')}catch(err){msg('status',err.message)}});
$('tokenUpdate').addEventListener('submit',async e=>{e.preventDefault();let token=new FormData(e.target).get('token');try{await call('/api/update-token',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token})});msg('status','Keyholder token saved. Restarting…')}catch(err){msg('status',err.message)}});
loadState();setInterval(()=>{if(!$('dashboard').classList.contains('hidden'))loadState()},30000);setInterval(timer,1000);
</script></body></html>)HTML";
}

void handleScan() {
  if (!requireAuth()) return; JsonDocument d; JsonArray a=d["networks"].to<JsonArray>(); int n=WiFi.scanNetworks(false,true); for(int i=0;i<n;i++){ JsonObject o=a.add<JsonObject>(); o["ssid"]=WiFi.SSID(i); o["rssi"]=WiFi.RSSI(i); } WiFi.scanDelete(); String out; serializeJson(d,out); sendJson(200,out); }
void captive() { server.sendHeader("Location","http://192.168.4.1/",true); server.send(302,"text/plain",""); }
void handleRoot() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", dashboardPage());
}
void handleState() {
  if (!requireAuth()) return;
  JsonDocument d;
  bool configured = role == "wearer" ? wearerToken.length() > 0 : holderToken.length() > 0;
  configured = configured && wifiSsid.length() > 0 && !handoverPending;
  d["ok"] = true; d["configured"] = configured; d["handoverPending"] = handoverPending;
  d["role"] = role; d["connected"] = WiFi.status() == WL_CONNECTED;
  d["lockId"] = lockId; d["apiError"] = lastError; d["needsPassword"] = !dashboardPassword.length(); d["wifiSsid"] = wifiSsid;
  d["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  JsonDocument data; if (deserializeJson(data, lastJson)) data["raw"] = lastJson;
  if (dashboardPassword.length()) d["data"].set(data.as<JsonVariantConst>()); else d["data"] = JsonObject();
  String out; serializeJson(d, out); sendJson(200, out);
}
void handleRefresh() {
  if (!requireAuth()) return;
  refreshLock(); if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}
void handleFreeze() {
  if (!requireAuth()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["isFrozen"].is<bool>()) { sendError(400, "isFrozen must be true or false."); return; }
  JsonDocument body; body["isFrozen"] = input["isFrozen"].as<bool>();
  String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/freeze", payload, response)) { sendError(502, lastError); return; }
  refreshLock();
  JsonDocument out; out["ok"] = true;
  String message = input["isFrozen"].as<bool>() ? "Chaster accepted the freeze request." : "Chaster accepted the unfreeze request.";
  if (lastError.length()) message += " The action was accepted, but the lock refresh failed: " + lastError;
  else message += " Lock data refreshed.";
  out["message"] = message;
  String result; serializeJson(out, result); sendJson(200, result);
}
void handleUpdateTime() {
  if (!requireAuth()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["duration"].is<long>()) { sendError(400, "duration must be signed seconds."); return; }
  long duration = input["duration"].as<long>();
  if (!duration || duration < -31536000L || duration > 31536000L) { sendError(400, "Duration must be between -365 and 365 days."); return; }
  JsonDocument body; body["duration"] = duration; String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/update-time", payload, response)) { sendError(502, lastError); return; }
  refreshLock(); JsonDocument out; out["ok"] = true;
  String message = duration > 0 ? "Chaster accepted the add-time request." : "Chaster accepted the remove-time request.";
  if (lastError.length()) message += " The action was accepted, but the lock refresh failed: " + lastError;
  else message += " Lock data refreshed.";
  out["message"] = message;
  String result; serializeJson(out, result); sendJson(200, result);
}
void handleSelectLock() {
  if (!requireAuth()) return;
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  lockId = d["lockId"] | "";
  prefs.begin("chaster", false);
  if (role == "keyholder") {
    // A keyholder lock selection is shared with the wearer on the next handover/view.
    prefs.putString("holder_lock_id", lockId); prefs.putString("wearer_lock_id", lockId);
  } else {
    prefs.putString("wearer_lock_id", lockId);
    // Prevent the legacy lock_id migration from restoring a cleared selection after reboot.
    prefs.remove("lock_id");
  }
  prefs.end();
  refreshLock(); if (lastError.length()) { sendError(502, lastError); return; }
  sendJson(200, "{\"ok\":true}");
}
void handleHistory() {
  if (!requireAuth()) return;
  if (!lockId.length()) { sendError(400, "Choose a specific lock first."); return; }
  String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/history", "{\"limit\":20}", response)) { sendError(502, lastError); return; }
  JsonDocument result; if (deserializeJson(result, response)) { sendError(502, "Chaster returned invalid history JSON."); return; }
  JsonDocument out; out["ok"] = true; out["data"].set(result.as<JsonVariantConst>());
  String body; serializeJson(out, body); sendJson(200, body);
}
void handleConfig() {
  if (!requireAuth()) return;
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String ssid = d["ssid"] | ""; String pass = d["wifiPassword"] | ""; String token = d["token"] | ""; String newLockId = d["lockId"] | "";
  token.trim();
  // Avoid accidentally storing a pasted Authorization header as the token itself.
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  String newDashPassword = d["dashboardPassword"] | ""; newDashPassword.trim();
  if (!ssid.length() || !token.length() || !newLockId.length()) { sendError(400, "Wi-Fi SSID, Chaster developer token and lock ID are required."); return; }
  if (!dashboardPassword.length() && newDashPassword.length() < 10) { sendError(400, "Set a dashboard password of at least 10 characters."); return; }
  // Once wearer or keyholder credentials are provisioned, role and API credentials stay locked.
  // Only the explicit handover-pending state may accept the keyholder token.
  if (!handoverPending && (wearerToken.length() || holderToken.length())) {
    sendError(409, "Credentials and role are locked. Use the credential reset flow before setting up again."); return;
  }
  prefs.begin("chaster", false);
  if (handoverPending) {
    // The keyholder chooses the canonical lock ID for both wearer and keyholder views.
    prefs.putString("holder_token", token); prefs.putString("holder_lock_id", newLockId); prefs.putString("wearer_lock_id", newLockId); prefs.putString("ssid", ssid);
    if (!dashboardPassword.length()) prefs.putString("dash_pw", newDashPassword);
    if (pass.length()) prefs.putString("wifi_pw", pass);
    prefs.putString("role", "keyholder"); prefs.putBool("handover", false);
  } else {
    prefs.putString("wearer_token", token); prefs.putString("wearer_lock_id", newLockId); prefs.putString("ssid", ssid); prefs.putString("dash_pw", newDashPassword);
    if (pass.length()) prefs.putString("wifi_pw", pass);
    prefs.putString("role", "wearer");
  }
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500); ESP.restart();
}
void handleHandover() {
  if (!requireAuth()) return;
  if (role != "wearer" || !wearerToken.length() || handoverPending) { sendError(409, "Hand Over is available after wearer setup."); return; }
  prefs.begin("chaster", false);
  prefs.putString("role", "keyholder"); prefs.remove("ssid"); prefs.remove("wifi_pw");
  prefs.remove("holder_token"); prefs.remove("holder_lock_id"); prefs.putBool("handover", true);
  prefs.end();
  sendJson(200, "{\"ok\":true,\"handoverPending\":true}");
  delay(500); ESP.restart();
}
void handleUpdateToken() {
  if (!requireAuth()) return;
  if (role != "keyholder" || handoverPending) { sendError(403, "Keyholder token replacement is available only in keyholder mode."); return; }
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String token = d["token"] | ""; token.trim();
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  if (!token.length()) { sendError(400, "A Chaster developer token is required."); return; }
  prefs.begin("chaster", false); prefs.putString("holder_token", token); prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void handleUpdateWiFi() {
  if (!requireAuth()) return;
  if (!wearerToken.length() && !holderToken.length()) { sendError(409, "Complete Chaster setup before changing Wi-Fi."); return; }
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String ssid = d["ssid"] | ""; String pass = d["wifiPassword"] | ""; ssid.trim();
  if (!ssid.length()) { sendError(400, "Wi-Fi network name is required."); return; }
  prefs.begin("chaster", false); prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("wifi_pw", pass); else prefs.remove("wifi_pw");
  prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void handleSetPassword() {
  if (!requireAuth()) return;
  if (dashboardPassword.length()) { sendError(409, "Dashboard password is already set. Use authenticated access to reset all credentials if it is lost."); return; }
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String password = d["password"] | ""; password.trim();
  if (password.length() < 10) { sendError(400, "Password must be at least 10 characters."); return; }
  prefs.begin("chaster", false); prefs.putString("dash_pw", password); prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void handleReset() {
  if (!requireAuth()) return;
  prefs.begin("chaster", false); prefs.clear(); prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void startSetupAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  if (!setupAP) {
    setupAP = WiFi.softAP(AP_NAME, AP_PASSWORD);
    dnsServer.start(53, "*", AP_IP);
  }
}
void connectWiFi() {
  // Setup AP is only active during first-time setup, handover, or Wi-Fi failure.
  if (!wifiSsid.length() || handoverPending) { startSetupAP(); return; }
  // Switching from AP+STA to STA tears down the setup AP; clear the flag so fallback recreates it.
  WiFi.mode(WIFI_STA); setupAP = false; WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) startSetupAP();
  else { dnsServer.stop(); WiFi.softAPdisconnect(true); setupAP = false; }
}
void loadConfig() {
  prefs.begin("chaster", true);
  wifiSsid = prefs.getString("ssid", ""); wifiPassword = prefs.getString("wifi_pw", ""); dashboardPassword = prefs.getString("dash_pw", "");
  wearerToken = prefs.getString("wearer_token", "");
  holderToken = prefs.getString("holder_token", "");
  // Migrate installations made by the earlier single-token firmware.
  if (!wearerToken.length()) wearerToken = prefs.getString("token", "");
  role = prefs.getString("role", "wearer"); handoverPending = prefs.getBool("handover", false);
  lockId = role == "keyholder" ? prefs.getString("holder_lock_id", "") : prefs.getString("wearer_lock_id", "");
  // Migrate the legacy shared lock ID only for the original wearer setup.
  if (role == "wearer" && !lockId.length()) lockId = prefs.getString("lock_id", "");
  prefs.end();
}
} // namespace

void setup() {
  Serial.begin(115200); delay(250); loadConfig(); connectWiFi();
  if (WiFi.status() != WL_CONNECTED) startSetupAP();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/generate_204", HTTP_GET, captive); server.on("/hotspot-detect.html", HTTP_GET, captive); server.on("/connecttest.txt", HTTP_GET, captive); server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/freeze", HTTP_POST, handleFreeze);
  server.on("/api/update-time", HTTP_POST, handleUpdateTime);
  server.on("/api/select-lock", HTTP_POST, handleSelectLock);
  server.on("/api/history", HTTP_POST, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/set-password", HTTP_POST, handleSetPassword);
  server.on("/api/update-wifi", HTTP_POST, handleUpdateWiFi);
  server.on("/api/update-token", HTTP_POST, handleUpdateToken);
  server.on("/api/handover", HTTP_POST, handleHandover);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound(captive);
  server.begin();
  if (WiFi.status() == WL_CONNECTED && activeToken().length()) refreshLock();
  Serial.println("Chaster ESP32 dashboard ready.");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}
void loop() {
  dnsServer.processNextRequest(); server.handleClient();
  // Keep the setup AP available as a recovery path if the configured Wi-Fi drops later.
  static uint32_t lastWifiAttempt = 0;
  if (wifiSsid.length() && !handoverPending && WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt >= 15000) {
    if (!setupAP) startSetupAP();
    WiFi.reconnect();
    lastWifiAttempt = millis();
  }
  if (WiFi.status() == WL_CONNECTED && setupAP && !handoverPending) {
    dnsServer.stop(); WiFi.softAPdisconnect(true); setupAP = false;
  }
  if (WiFi.status() == WL_CONNECTED && activeToken().length() && !handoverPending && millis() - lastRefresh >= REFRESH_MS) refreshLock();
  delay(2);
}
