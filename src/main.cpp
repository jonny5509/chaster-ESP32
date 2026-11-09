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
String wifiSsid, wifiPassword, wearerToken, holderToken, lockId, dashboardPassword, wearerUserId, holderUserId;
String role = "keyholder", lastJson = "{}", lastError;
uint32_t lastRefresh = 0;
uint32_t lastMatchAttempt = 0;
bool handoverPending = false;
bool handoverRestartPending = false;

// Wearer token is read-only; controls are keyholder-gated.
String activeToken() { return handoverPending ? "" : (role == "wearer" ? wearerToken : holderToken); }
bool requireAuth() {
  if (dashboardPassword.length()) {
    if (server.authenticate("admin", dashboardPassword.c_str())) return true;
    server.requestAuthentication(); return false;
  }
  // Without a dashboard password, permit only initial setup and password provisioning.
  // Once either role has been configured, controls require authentication.
  String uri = server.uri();
  bool provisioned = wearerToken.length() || holderToken.length();
  bool allowedWithoutPassword = !provisioned || uri == "/" || uri == "/api/state" ||
                                uri == "/api/set-password" || (handoverPending && (uri == "/api/config" || uri == "/api/keyholder-setup"));
  if (allowedWithoutPassword) return true;
  server.send(423, "application/json", "{\"ok\":false,\"error\":\"Set a dashboard password before using device controls.\"}");
  return false;
}
void sendError(int code, const String &message);
bool requireKeyholder() {
  if (role == "keyholder" && holderToken.length() && !handoverPending) return true;
  sendError(403, "This action is available only after the keyholder completes handover.");
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
bool apiRequestWithToken(const String &method, const String &path, const String &body, String &response, String token, const String &requestRole) {
  if (WiFi.status() != WL_CONNECTED) { lastError = "Wi-Fi is not connected."; return false; }
  token.trim();
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  if (!token.length()) { lastError = "No " + requestRole + " Chaster API token configured."; return false; }
  WiFiClientSecure client;
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
    if (code == 401) lastError = "Chaster rejected the " + requestRole + " API token (401). Check that it belongs to the correct account and is not expired or revoked. Response: " + response;
    else if (code == 403) {
      String scope = path == "/auth/profile" ? "profile" : (requestRole == "keyholder" ? "keyholder:read" : "locks:read");
      lastError = "Chaster denied this request (403). Check the " + requestRole + " token has the " + scope + " scope. Response: " + response;
    }
    return false;
  }
  lastError = ""; return true;
}
bool apiRequest(const String &method, const String &path, const String &body, String &response) {
  return apiRequestWithToken(method, path, body, response, activeToken(), role);
}
bool getAccountUserId(const String &token, const String &accountRole, String &userId) {
  String response;
  if (!apiRequestWithToken("GET", "/auth/profile", "", response, token, accountRole)) return false;
  JsonDocument profile;
  if (deserializeJson(profile, response)) { lastError = "Chaster returned invalid profile JSON."; return false; }
  userId = profile["_id"] | "";
  if (!userId.length()) userId = profile["id"] | "";
  if (!userId.length()) userId = profile["userId"] | "";
  if (!userId.length() && profile["user"].is<JsonObject>()) {
    userId = profile["user"]["_id"] | "";
    if (!userId.length()) userId = profile["user"]["id"] | "";
  }
  userId.trim();
  if (!userId.length()) { lastError = "Chaster profile response did not include a user ID."; return false; }
  return true;
}
String findLockIdInEntry(JsonVariantConst entry) {
  if (!entry.is<JsonObjectConst>()) return "";
  JsonObjectConst o = entry.as<JsonObjectConst>();
  JsonVariantConst lock = o["lock"];
  if (lock.is<JsonObjectConst>()) {
    String id = lock["_id"] | "";
    if (!id.length()) id = lock["id"] | "";
    if (!id.length()) id = lock["lockId"] | "";
    if (id.length()) return id;
  }
  String id = o["lockId"] | o["lock_id"] | "";
  if (id.length()) return id;
  if (o["status"].is<const char*>() || o["isFrozen"].is<bool>() || o["startDate"].is<const char*>() || o["startDate"].is<long>()) {
    id = o["_id"] | o["id"] | "";
    if (id.length()) return id;
  }
  return "";
}
bool findWearerLock(JsonVariantConst node, const String &wearerId, String &foundLockId, uint8_t depth = 0) {
  if (depth > 12) return false;
  if (node.is<JsonObjectConst>()) {
    JsonObjectConst o = node.as<JsonObjectConst>();
    String relatedId = o["userId"] | "";
    if (!relatedId.length()) relatedId = o["wearerId"] | "";
    if (!relatedId.length()) relatedId = o["lockedUserId"] | "";
    JsonVariantConst user = o["user"];
    if (!relatedId.length() && user.is<JsonObjectConst>()) relatedId = user["_id"] | "";
    if (!relatedId.length()) relatedId = user["id"] | "";
    JsonVariantConst wearer = o["wearer"];
    if (!relatedId.length() && wearer.is<JsonObjectConst>()) relatedId = wearer["_id"] | "";
    if (!relatedId.length()) relatedId = wearer["id"] | "";
    JsonVariantConst lockedUser = o["lockedUser"];
    if (!relatedId.length() && lockedUser.is<JsonObjectConst>()) relatedId = lockedUser["_id"] | "";
    if (!relatedId.length()) relatedId = lockedUser["id"] | "";
    if (relatedId == wearerId) {
      foundLockId = findLockIdInEntry(node);
      if (!foundLockId.length()) { String candidate = o["_id"] | "";
        if (!candidate.length()) candidate = o["id"] | ""; if (candidate != wearerId) foundLockId = candidate; }
      if (foundLockId.length()) return true;
    }
    for (JsonPairConst pair : o) if (findWearerLock(pair.value(), wearerId, foundLockId, depth + 1)) return true;
  } else if (node.is<JsonArrayConst>()) {
    for (JsonVariantConst item : node.as<JsonArrayConst>()) if (findWearerLock(item, wearerId, foundLockId, depth + 1)) return true;
  }
  return false;
}
bool discoverWearerLock(const String &keyholderToken, const String &expectedKeyholderId, String &foundLockId) {
  // Verify the supplied keyholder token against the keyholder profile ID,
  // then search that account's locks for the saved wearer profile ID.
  if (!wearerUserId.length()) { lastError = "Saved wearer account ID is missing. Reset and repeat wearer setup."; return false; }
  if (!expectedKeyholderId.length()) { lastError = "Keyholder account ID is missing. Complete keyholder profile validation first."; return false; }
  String verifiedKeyholderId;
  if (!getAccountUserId(keyholderToken, "keyholder", verifiedKeyholderId)) return false;
  if (verifiedKeyholderId != expectedKeyholderId) {
    lastError = "The keyholder token does not belong to the supplied keyholder account ID. Both account identities must match before session discovery.";
    return false;
  }
  for (uint8_t page = 0; page < 10; ++page) {
    JsonDocument request; request["limit"] = 50; request["page"] = page; request["status"] = "locked";
    String payload; serializeJson(request, payload); String response;
    if (!apiRequestWithToken("POST", "/keyholder/locks/search", payload, response, keyholderToken, "keyholder")) return false;
    JsonDocument results;
    if (deserializeJson(results, response)) { lastError = "Chaster returned invalid keyholder lock-search JSON."; return false; }
    if (findWearerLock(results.as<JsonVariantConst>(), wearerUserId, foundLockId)) return true;
    JsonVariantConst list = results["items"];
    if (!list.is<JsonArrayConst>()) list = results["data"];
    if (!list.is<JsonArrayConst>()) list = results["results"];
    // Keep paging until a match is found or the bounded search window is exhausted.
  }
  lastError = "No active locked session was found for the saved wearer account under this keyholder. Confirm that this keyholder currently holds the wearer's lock, then retry handover.";
  return false;
}
bool findActiveWearerLock(JsonVariantConst node, String &foundId, uint8_t depth = 0) {
  if (depth > 12) return false;
  if (node.is<JsonObjectConst>()) {
    JsonObjectConst o = node.as<JsonObjectConst>();
    String status = o["status"] | o["state"] | "";
    bool active = status.equalsIgnoreCase("locked") || o["isLocked"] == true;
    if (active) {
      String candidate = findLockIdInEntry(node);
      if (!candidate.length()) candidate = o["_id"] | o["id"] | "";
      if (candidate.length()) { foundId = candidate; return true; }
    }
    for (JsonPairConst pair : o) if (findActiveWearerLock(pair.value(), foundId, depth + 1)) return true;
  } else if (node.is<JsonArrayConst>()) {
    for (JsonVariantConst item : node.as<JsonArrayConst>()) if (findActiveWearerLock(item, foundId, depth + 1)) return true;
  }
  return false;
}
void refreshLock() {
  lastRefresh = millis();
  String response;
  if (role == "wearer" && !lockId.length()) {
    // Wearers see their own active lock via their own token; no keyholder
    // credentials or handover discovery are involved in this read-only view.
    if (!apiRequestWithToken("GET", "/locks", "", response, wearerToken, "wearer")) return;
    JsonDocument locks;
    if (deserializeJson(locks, response)) { lastError = "Chaster returned invalid wearer lock-list JSON."; return; }
    if (!findActiveWearerLock(locks.as<JsonVariantConst>(), lockId)) {
      lastError = "No active locked session was found for this wearer account yet.";
      lastJson = "{}";
      return;
    }
  }
  if (!lockId.length()) { lastError = "No matching wearer session has been discovered yet."; return; }
  if (apiRequest("GET", "/locks/" + lockId, "", response)) {
    lastJson = response;
    lastRefresh = millis();
  }
}
String dashboardPage() {
  return R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0b1020"><title>Chaster Control</title><style>
:root{color-scheme:dark;--bg:#090e1b;--panel:#111a2d;--panel2:#151f35;--line:#26344f;--text:#edf3ff;--muted:#94a3bd;--accent:#8b9cff;--accent2:#6077ed;--good:#46d6a0;--warn:#ffbf69;--bad:#ff7485;--field:#0b1425}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 15% -10%,#25365c 0,transparent 42%),var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
main{width:min(1100px,100%);margin:auto;padding:28px 22px 60px}.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:24px}
.brand{display:flex;align-items:center;gap:13px}.logo{width:44px;height:44px;border-radius:14px;background:linear-gradient(145deg,#a5b2ff,#596ee5);display:grid;place-items:center;color:#10172a;font-size:23px;font-weight:900;box-shadow:0 8px 28px #647cff40}
.eyebrow{font-size:11px;letter-spacing:2px;text-transform:uppercase;color:var(--accent);font-weight:800}.brand h1{font-size:22px;letter-spacing:-.5px;margin:0}.muted{color:var(--muted);font-size:13px}
.pill{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border:1px solid var(--line);border-radius:999px;background:#111a2dcc;color:var(--muted);font-size:12px}.dot{width:8px;height:8px;border-radius:50%;background:var(--warn)}.dot.good{background:var(--good);box-shadow:0 0 12px #46d6a080}.dot.bad{background:var(--bad)}
.hero{background:linear-gradient(120deg,#1a2947,#121b30 68%);border:1px solid #344666;border-radius:24px;padding:26px;margin-bottom:16px;box-shadow:0 18px 60px #0003}
.herohead{display:flex;justify-content:space-between;gap:16px;align-items:flex-start}.hero h2{font-size:25px;letter-spacing:-.7px;margin:4px 0}.hero p{margin:4px 0;color:#aab8d1}.lockid{font:12px ui-monospace,monospace;color:#aab8d1;overflow-wrap:anywhere}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:20px 0 14px}.stat{padding:16px;border-radius:16px;background:#0b142866;border:1px solid #34425f}.statlabel{font-size:11px;letter-spacing:1.2px;text-transform:uppercase;color:var(--muted);font-weight:700}.statvalue{font-size:clamp(21px,3vw,30px);font-weight:750;letter-spacing:-.8px;font-variant-numeric:tabular-nums;margin-top:5px}
.progress-track{height:8px;background:#080e1b;border-radius:99px;overflow:hidden;margin-top:15px}.progress-fill{height:100%;width:0;background:linear-gradient(90deg,#6c83ff,#55d6b0);border-radius:99px;transition:width .35s}.progress-caption{display:flex;justify-content:space-between;gap:10px;margin-top:7px;color:var(--muted);font-size:12px}
.layout{display:grid;grid-template-columns:minmax(0,1.2fr) minmax(280px,.8fr);gap:16px;align-items:start}.stack{display:grid;gap:16px}.card{background:linear-gradient(160deg,#141f34,#10192b);border:1px solid var(--line);border-radius:20px;padding:20px;box-shadow:0 10px 32px #0002}.cardhead{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}.card h2{font-size:16px;margin:0;letter-spacing:-.2px}.tag{font-size:11px;color:var(--accent);background:#647cff1a;border:1px solid #647cff38;padding:4px 8px;border-radius:99px}
.details{display:grid;grid-template-columns:1fr 1fr;gap:10px}.detail{padding:12px;border-radius:12px;background:#0b1425;border:1px solid #202e47;min-width:0}.detail span{display:block;color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.8px}.detail strong{display:block;margin-top:4px;font-size:14px;overflow-wrap:anywhere}
button,input,select{font:inherit}button{border:0;border-radius:11px;padding:11px 14px;background:var(--accent2);color:white;font-weight:700;cursor:pointer;transition:filter .15s,transform .15s}button:hover{filter:brightness(1.12)}button:active{transform:translateY(1px)}button.secondary{background:#253653;color:#e4ebff;border:1px solid #344765}button.danger{background:#7e3448;color:#ffe9ed}button:disabled{opacity:.5;cursor:not-allowed}
input{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #34445f;background:var(--field);color:var(--text);outline:none}input:focus{border-color:var(--accent);box-shadow:0 0 0 3px #8b9cff20}label{display:block;color:var(--muted);font-size:12px;margin:12px 0 6px}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:9px}.fieldrow{display:grid;grid-template-columns:1fr 1fr;gap:9px}.message{padding:11px 13px;background:#0b1425;border:1px solid #263650;border-radius:11px;color:var(--muted);font-size:13px;margin-top:12px;overflow-wrap:anywhere}.message.error{border-color:#814050;color:#ffb6c0}.message.success{border-color:#28644f;color:#a4f2d3}
.history{max-height:320px;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere;background:#0a1323;border:1px solid #22314a;padding:13px;border-radius:12px;font:12px/1.55 ui-monospace,monospace;color:#c5d2eb}
details{margin-top:14px}summary{cursor:pointer;color:var(--muted);font-size:12px}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:300px;overflow:auto;background:#0a1323;padding:12px;border-radius:10px;font-size:11px;color:#c5d2eb}
form .submit{width:100%;margin-top:14px}.hidden{display:none!important}.notice{padding:13px;border:1px solid #3a4d70;background:#101c31;border-radius:12px;margin:12px 0;color:#bdcbe4;font-size:13px}
.footer{text-align:center;color:#62718b;font-size:11px;margin-top:24px}
@media(max-width:760px){main{padding:18px 14px 40px}.layout{grid-template-columns:1fr}.hero{padding:20px}.stats{gap:8px}.stat{padding:12px}.statvalue{font-size:22px}}
@media(max-width:460px){.topbar{align-items:flex-start}.brand h1{font-size:19px}.pill{font-size:11px;padding:7px 9px}.herohead{display:block}.stats{grid-template-columns:1fr}.statvalue{font-size:25px}.details{grid-template-columns:1fr 1fr}.card{padding:16px}}
 .permission-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px;margin-top:12px}.permission-card{border:1px solid var(--line);border-radius:12px;padding:12px;background:var(--panel,rgba(127,127,127,.05));min-width:0}.permission-name{font-weight:700;font-size:13px;overflow-wrap:anywhere;margin-bottom:10px}.permission-row{display:flex;align-items:flex-start;justify-content:space-between;gap:10px;padding-top:8px;margin-top:8px;border-top:1px solid var(--line);font-size:12px}.permission-row>span:first-child{font-weight:600}.permission-value{border-radius:7px;padding:4px 7px;text-align:right;overflow-wrap:anywhere}.permission-value.granted{background:rgba(34,160,90,.13);color:var(--success,#23834f)}.permission-value.denied{background:rgba(130,130,130,.13);color:var(--muted,#777)}</style></head><body><main>
<header class="topbar"><div class="brand"><div class="logo">C</div><div><div class="eyebrow">Chaster device</div><h1>Control centre</h1></div></div><div class="pill"><i id="connectionDot" class="dot"></i><span id="connectionText">Connecting…</span></div></header>
<div id="status" class="notice">Connecting to your ESP32…</div>
<section id="setup" class="card hidden"><div class="eyebrow">First-time setup</div><h2 id="setupTitle" style="font-size:22px;margin:5px 0 8px">Wearer device setup</h2><div id="handoverInfo" class="notice">Connect Wi-Fi and enter the wearer Chaster token. The device identifies the wearer account automatically. Then hand it to the keyholder to finish activation.</div>
<form id="config"><label>Wi-Fi network name</label><input name="ssid" required autocomplete="off"><label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password"><label>Wearer Chaster developer token (read-only)</label><input name="token" type="password" required autocomplete="new-password"><label>Dashboard password (minimum 10 characters)</label><input name="dashboardPassword" type="password" minlength="10" autocomplete="new-password"><button class="submit" type="submit" id="saveBtn">Save wearer setup</button></form><p id="wearerSetupNote" class="muted">The wearer token is retained on the device after keyholder handover. The session is matched using both account IDs.</p><form id="keyholderSetup" class="hidden"><div class="notice">Enter your Chaster developer token to verify your account and find the wearer's active lock. Then set up your own Wi-Fi and dashboard password.</div><label>Keyholder Chaster developer token</label><input name="token" type="password" required autocomplete="new-password"><label>Keyholder Wi-Fi network name</label><input name="ssid" required autocomplete="off"><label>Keyholder Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password"><label>New dashboard password (minimum 10 characters)</label><input name="dashboardPassword" type="password" minlength="10" required autocomplete="new-password"><button class="submit" type="submit">Save keyholder device setup</button></form></section>
<section id="dashboard" class="hidden">
<section id="handoverPanel" class="card hidden"><div class="eyebrow">Device handover</div><h2 style="font-size:21px;margin:5px 0 8px">Ready for the keyholder</h2><p class="muted">Wearer setup is complete. Press the button to clear the wearer's Wi-Fi and dashboard password. The wearer Chaster token and account ID stay on the device. The keyholder will enter their own Chaster token and settings on the keyholder setup page.</p><form id="handoverForm"><button class="submit" type="submit">Hand over to keyholder</button></form><div id="handoverMessage" class="message">Keep the device with the keyholder until activation finishes.</div></section>
<div class="hero"><div class="herohead"><div><div class="eyebrow">Keyholder dashboard</div><h2 id="heroTitle">Your selected lock</h2><p id="heroSubtitle">Live overview from Chaster</p></div><span class="tag" id="lockStatus">WAITING FOR DATA</span></div>
<div class="lockid" id="heroLockId">No lock ID selected</div>
<div class="stats"><div class="stat"><div class="statlabel">Time elapsed</div><div class="statvalue" id="elapsed">—</div></div><div class="stat"><div class="statlabel">Time remaining</div><div class="statvalue" id="remaining">—</div></div><div class="stat"><div class="statlabel">Lock state</div><div class="statvalue" id="frozenState">—</div></div></div>
<div class="progress-track"><div class="progress-fill" id="timeProgress"></div></div><div class="progress-caption"><span id="progressLeft">Lock progress</span><span id="progressRight">Waiting for lock dates</span></div></div>
<div class="layout"><div class="stack">
<section class="card"><div class="cardhead"><h2>Session</h2><span class="tag">LOCK DETAILS</span></div><div class="details" id="sessionDetails"></div><details><summary>View raw Chaster response</summary><pre id="lockData">Waiting for Chaster data…</pre></details></section>
<section class="card"><div class="cardhead"><h2>Wearer</h2><span class="tag">ACCOUNT</span></div><div class="details" id="wearerDetails"></div></section>
<section class="card"><div class="cardhead"><h2>Keyholder</h2><span class="tag">ACCOUNT & PERMISSIONS</span></div><div class="details" id="keyholderDetails"></div><details><summary>View keyholder permissions</summary><div id="permissionData" class="permission-grid"><div class="permission-card muted">Waiting for permission data…</div></div></details></section>
<section class="card"><div class="cardhead"><h2>Lock history</h2><span class="tag">RECENT EVENTS</span></div><p class="muted">History for the selected lock, fetched directly from Chaster.</p><button class="secondary" onclick="loadHistory()">Load recent history</button><pre class="history" id="history">Select a lock to view its history.</pre></section>

</div><div class="stack">
<section class="card keyholderOnly"><div class="cardhead"><h2>Lock controls</h2><span class="tag">ACTIONS</span></div><div class="actions"><button onclick="freeze(true)">Freeze lock</button><button class="secondary" onclick="freeze(false)">Unfreeze lock</button></div><label>Adjust lock time</label><div class="fieldrow"><div><label>Hours</label><input id="hours" type="number" min="0" max="8760" value="1"></div><div><label>Minutes</label><input id="minutes" type="number" min="0" max="59" value="0"></div></div><div class="actions" style="margin-top:10px"><button onclick="changeTime(true)">Add time</button><button class="danger" onclick="changeTime(false)">Remove time</button></div><div id="actionMsg" class="message">Choose an action. Chaster will confirm whether it is permitted for this lock.</div></section>
<section class="card"><div class="cardhead"><h2>Connection</h2><span class="tag">DEVICE</span></div><div class="detail"><span>Wi-Fi network</span><strong id="networkName">—</strong></div><div class="detail" style="margin-top:9px"><span>Device address</span><strong id="deviceIp">—</strong></div><form id="wifiForm"><label>Change Wi-Fi network</label><input id="wifiSsid" name="ssid" required autocomplete="off" placeholder="Network name"><label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password" placeholder="Leave blank for open network"><button class="submit" type="submit">Save Wi-Fi settings</button></form></section>
<div id="passwordSetup" class="card hidden"><h2>Secure dashboard</h2><p class="muted">Set a unique password to protect dashboard controls.</p><form id="passwordForm"><label>New dashboard password</label><input name="password" type="password" minlength="10" required autocomplete="new-password"><button class="submit" type="submit">Save password</button></form></div>

</div></div></section><div class="footer">Chaster Control Centre · ESP32 keyholder edition</div></main><script>
const $=id=>document.getElementById(id);let selectedLockId='',lockStart=0,lockEnd=0,lockFrozen=false,lockFrozenAt=0,lastData={};
async function call(url,opts={}){const r=await fetch(url,opts);const t=await r.text();let d;try{d=JSON.parse(t)}catch(e){throw new Error(t||('HTTP '+r.status))}if(!r.ok)throw new Error(d.error||('HTTP '+r.status));return d}
function msg(id,s,kind=''){const el=$(id);if(!el)return;el.textContent=s;el.classList.toggle('error',kind==='error');el.classList.toggle('success',kind==='success')}
function duration(ms){if(!Number.isFinite(ms))return '—';let n=Math.max(0,Math.floor(ms/1000)),d=Math.floor(n/86400);n%=86400;let h=Math.floor(n/3600);n%=3600;return (d?d+'d ':'')+(h?h+'h ':'')+Math.floor(n/60)+'m '+(n%60)+'s'}
function dateValue(v){if(v===undefined||v===null||v==='')return 0;if(typeof v==='number')return v<1e12?v*1000:v;const n=Date.parse(v);return Number.isFinite(n)?n:0}
function pick(o,keys){for(const k of keys){if(o&&o[k]!==undefined&&o[k]!==null&&o[k]!=='')return o[k]}return null}
function unwrap(d){if(!d||typeof d!=='object')return {};for(const k of ['lock','item'])if(d[k]&&typeof d[k]==='object')return d[k];if(d.data&&typeof d.data==='object'&&!Array.isArray(d.data)){for(const k of ['lock','item'])if(d.data[k]&&typeof d.data[k]==='object')return d.data[k];return d.data}return d}
function formatDate(t){return t?new Date(t).toLocaleString():'—'}
function renderDetails(id,entries){const box=$(id);box.textContent='';for(const [label,value] of entries){const item=document.createElement('div');item.className='detail';const name=document.createElement('span');name.textContent=label;const val=document.createElement('strong');val.textContent=value===undefined||value===null||value===''?'—':String(value);item.append(name,val);box.appendChild(item)}}
function yesNo(v){return v===true?'Yes':v===false?'No':'Unknown'}
function accountState(a){if(!a)return [['Account','Not supplied']];return [['Username',a.username],['User ID',a._id],['Online',a.online===true?'Online':a.online===false?'Offline':'Unknown'],['Role',a.role],['Joined',a.joinedAt],['Premium',yesNo(a.isPremium)],['Disabled',yesNo(a.isDisabled)],['Suspended',yesNo(a.isSuspended)]]}
function renderData(raw){lastData=raw||{};const d=unwrap(lastData);const id=pick(d,['_id','id','lockId'])||selectedLockId;const title=pick(d,['title','name','lockName'])||'Selected Chaster lock';const status=pick(d,['status','state'])||'Unknown';const frozen=pick(d,['isFrozen','frozen']);const start=dateValue(pick(d,['startDate','startAt','startedAt','createdAt','start']));const end=dateValue(pick(d,['endDate','endAt','endsAt','expectedEndDate','end']));lockStart=start;lockEnd=end;lockFrozen=frozen===true;lockFrozenAt=lockFrozen?dateValue(pick(d,['frozenAt','freezeDate','frozenAtDate'])):0;
msg('heroTitle',String(title));msg('heroSubtitle',String(pick(d,['description','message'])||'Live overview from your Chaster keyholder account'));msg('heroLockId',id?'LOCK ID · '+id:'No lock ID selected');msg('lockStatus',String(status).toUpperCase());msg('frozenState',frozen===true?'Frozen':frozen===false?'Active':'Unknown');
renderDetails('sessionDetails',[
['Lock ID',id],['Title',title],['Status',status],['Lock type',pick(d,['lockType'])],
['Started',formatDate(start)],['Ends',formatDate(end)],['Minimum date',formatDate(dateValue(pick(d,['minDate'])))],
['Maximum date',formatDate(dateValue(pick(d,['maxDate'])))],['Minimum limit',formatDate(dateValue(pick(d,['minLimitDate'])))],
['Maximum limit',formatDate(dateValue(pick(d,['maxLimitDate'])))],['Frozen',yesNo(frozen)],
['Frozen at',formatDate(dateValue(pick(d,['frozenAt'])))],['Unlocked at',formatDate(dateValue(pick(d,['unlockedAt'])))],
['Unlock ready',yesNo(pick(d,['isReadyToUnlock']))],['Can be unlocked',yesNo(pick(d,['canBeUnlocked']))],
['Time visible',yesNo(pick(d,['isAllowedToViewTime']))],['Timer shown',yesNo(pick(d,['displayRemainingTime']))],
['Time limits enabled',yesNo(pick(d,['limitLockTime']))],['Last API update',formatDate(dateValue(pick(d,['updatedAt'])))],
['Created',formatDate(dateValue(pick(d,['createdAt'])))],['Total duration',pick(d,['totalDuration'])!==null?duration(Number(pick(d,['totalDuration']))):'—']
]);
renderDetails('wearerDetails',accountState(d.user));
renderDetails('keyholderDetails',accountState(d.keyholder));
const grants=d.permissions&&Array.isArray(d.permissions.grants)?d.permissions.grants:[];
const permissionBox=$('permissionData'); permissionBox.textContent='';
if(grants.length){grants.forEach(g=>{const subjects=g.subjects||{};const card=document.createElement('div');card.className='permission-card';
const title=document.createElement('div');title.className='permission-name';title.textContent=g.resource||'Permission';card.appendChild(title);
[['Keyholder',subjects.keyholder],['Wearer',subjects.wearer]].forEach(([who,rights])=>{const row=document.createElement('div');row.className='permission-row';const label=document.createElement('span');label.textContent=who;const value=document.createElement('span');const granted=Array.isArray(rights)&&rights.length>0;value.className='permission-value '+(granted?'granted':'denied');value.textContent=granted?rights.join(', '):'Not granted';row.append(label,value);card.appendChild(row)});permissionBox.appendChild(card)})}
else{const empty=document.createElement('div');empty.className='permission-card muted';empty.textContent='No permission grants supplied by Chaster.';permissionBox.appendChild(empty)}
$('lockData').textContent=JSON.stringify(lastData,null,2);
const now=Date.now(),effectiveNow=lockFrozen?(lockFrozenAt||now):now,total=end&&start?end-start:0,elapsed=start?Math.max(0,effectiveNow-start):0,remaining=end?Math.max(0,end-effectiveNow):0;const pct=total>0?Math.max(0,Math.min(100,elapsed/total*100)):0;$('timeProgress').style.width=pct+'%';msg('progressLeft',total>0?Math.round(pct)+'% elapsed':'Lock progress');msg('progressRight',total>0?duration(remaining)+' left':'Waiting for lock dates');timer()}
function timer(){const now=Date.now(),effectiveNow=lockFrozen?(lockFrozenAt||now):now;msg('elapsed',lockStart?duration(Math.max(0,effectiveNow-lockStart)):'—');msg('remaining',lockEnd?duration(Math.max(0,lockEnd-effectiveNow)):'—')}
async function loadState(){try{const x=await call('/api/state');selectedLockId=x.lockId||'';if(!x.configured){$('setup').classList.remove('hidden');$('dashboard').classList.add('hidden');const keyholderSetup=x.role==='keyholder';$('setupTitle').textContent=keyholderSetup?'Keyholder device setup':'Wearer device setup';$('config').classList.toggle('hidden',keyholderSetup);$('wearerSetupNote').classList.toggle('hidden',keyholderSetup);$('keyholderSetup').classList.toggle('hidden',!keyholderSetup||!!x.keyholderTokenSaved);$('handoverInfo').textContent=keyholderSetup?(x.keyholderTokenSaved?'Keyholder details are saved. The device is checking Chaster for the wearer’s active lock every 60 seconds. Keep the device powered on and online; it will switch to the keyholder dashboard automatically when the lock appears.':'Enter your Chaster developer token, Wi-Fi network and password, and a new dashboard password. The device will keep checking for the wearer’s active lock every 60 seconds after setup.'):'Connect Wi-Fi and enter the wearer Chaster token. The device identifies the wearer account automatically. Then hand it to the keyholder to finish activation.';msg('status',keyholderSetup?(x.keyholderTokenSaved?'Waiting for wearer lock · automatic retry every 60s':'Keyholder setup · connect to Chaster-ESP32'):'Wearer setup · connect to Chaster-ESP32');$('connectionText').textContent=x.keyholderTokenSaved?'Waiting for lock':'Setup required';$('connectionDot').className='dot';return}
$('setup').classList.add('hidden');$('dashboard').classList.remove('hidden');const wearerMode=x.role==='wearer';$('handoverPanel').classList.toggle('hidden',!wearerMode);document.querySelectorAll('.keyholderOnly').forEach(el=>el.classList.toggle('hidden',wearerMode));$('passwordSetup').classList.toggle('hidden',!x.needsPassword);$('networkName').textContent=x.wifiSsid||'—';$('deviceIp').textContent=x.ip||'—';if(!$('wifiSsid').value)$('wifiSsid').value=x.wifiSsid||'';const connected=!!x.connected;$('connectionText').textContent=connected?'Wi-Fi connected':'Wi-Fi offline';$('connectionDot').className='dot '+(connected?'good':'bad');msg('status',x.apiError?'Chaster API · '+x.apiError:(connected?'Connected to '+(x.wifiSsid||'Wi-Fi')+' · '+x.ip:'Wi-Fi disconnected'),x.apiError?'error':'success');document.querySelector('.hero .eyebrow').textContent=wearerMode?'Wearer dashboard':'Keyholder dashboard';document.querySelector('.footer').textContent=wearerMode?'Chaster Control Centre · wearer read-only mode':'Chaster Control Centre · keyholder edition';renderData(x.data||{});}catch(e){$('setup').classList.add('hidden');$('dashboard').classList.add('hidden');$('connectionText').textContent='Device state unavailable';$('connectionDot').className='dot bad';msg('status','Could not read device state: '+e.message+'. Check that you are connected to the ESP32 network, then reload this page. No setup details have been changed.','error')}}
async function refresh(){try{await call('/api/refresh',{method:'POST'});await loadState()}catch(e){msg('actionMsg',e.message,'error')}}

async function freeze(value){if(!selectedLockId){msg('actionMsg','Waiting for the wearer session to be matched.','error');return}if(!confirm((value?'Freeze':'Unfreeze')+' this lock?'))return;try{const x=await call('/api/freeze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({isFrozen:value})});msg('actionMsg',x.message||'Chaster accepted the request.','success');await loadState()}catch(e){msg('actionMsg',e.message,'error')}}
async function changeTime(add){if(!selectedLockId){msg('actionMsg','Waiting for the wearer session to be matched.','error');return}let h=Number($('hours').value),m=Number($('minutes').value);if(!Number.isInteger(h)||!Number.isInteger(m)||h<0||m<0||m>59||(!h&&!m)){msg('actionMsg','Enter a positive duration.','error');return}let sec=(h*3600+m*60)*(add?1:-1);if(!confirm((add?'Add ':'Remove ')+h+'h '+m+'m?'))return;try{const x=await call('/api/update-time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({duration:sec})});msg('actionMsg',x.message||'Chaster accepted the request.','success');await refresh()}catch(e){msg('actionMsg',e.message,'error')}}
async function loadHistory(){if(!selectedLockId){msg('history','Waiting for the wearer session to be matched.');return}msg('history','Loading recent history for '+selectedLockId+'…');try{const x=await call('/api/history',{method:'POST'});msg('history',JSON.stringify(x.data,null,2))}catch(e){msg('history','Could not load history: '+e.message)}}
$('keyholderSetup').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target),p={token:f.get('token'),ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),dashboardPassword:f.get('dashboardPassword')};try{await call('/api/keyholder-setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});msg('status','Your Wi-Fi and dashboard settings saved. Restarting…','success')}catch(err){msg('status',err.message,'error')}});
$('config').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target),p={ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),token:f.get('token'),dashboardPassword:f.get('dashboardPassword')};try{await call('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});msg('status','Wearer setup saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('wifiForm').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target);try{await call('/api/update-wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword')})});msg('status','Wi-Fi settings saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('passwordForm').addEventListener('submit',async e=>{e.preventDefault();const password=new FormData(e.target).get('password');try{await call('/api/set-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password})});msg('status','Dashboard password saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('handoverForm').addEventListener('submit',async e=>{e.preventDefault();if(!confirm('Hand the device over now? The wearer Wi-Fi and dashboard password will be cleared. The wearer Chaster token and account ID will be retained.'))return;try{await call('/api/handover',{method:'POST'});msg('status','Handover started. The keyholder can now connect to Chaster-ESP32 and complete setup.','success')}catch(err){msg('handoverMessage',err.message,'error')}});
loadState();setInterval(()=>{loadState()},15000);setInterval(timer,1000);
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
  bool configured = (holderToken.length() > 0 || wearerToken.length() > 0) && wifiSsid.length() > 0 && dashboardPassword.length() > 0 && !handoverPending;
  d["ok"] = true; d["configured"] = configured;
  d["role"] = role; d["connected"] = WiFi.status() == WL_CONNECTED;
  d["handoverPending"] = handoverPending; d["keyholderTokenSaved"] = holderToken.length() > 0;
  d["wearerUserId"] = wearerUserId; d["keyholderUserId"] = holderUserId; d["lockId"] = lockId; d["apiError"] = lastError; d["needsPassword"] = !dashboardPassword.length(); d["wifiSsid"] = wifiSsid;
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
  if (!requireAuth() || !requireKeyholder()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["isFrozen"].is<bool>()) { sendError(400, "isFrozen must be true or false."); return; }
  JsonDocument body; body["isFrozen"] = input["isFrozen"].as<bool>();
  String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/freeze", payload, response)) { sendError(502, lastError); return; }
  // Return the control result before making another HTTPS request. The main loop
  // will refresh lock data once this HTTP response has been sent.
  lastRefresh = millis() - REFRESH_MS;
  JsonDocument out; out["ok"] = true;
  String message = input["isFrozen"].as<bool>() ? "Chaster accepted the freeze request." : "Chaster accepted the unfreeze request.";
  out["message"] = message + " Updating lock details…";
  String result; serializeJson(out, result); sendJson(200, result);
}
void handleUpdateTime() {
  if (!requireAuth() || !requireKeyholder()) return;
  if (!lockId.length()) { sendError(400, "Select a specific lock first."); return; }
  JsonDocument input; if (deserializeJson(input, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  if (!input["duration"].is<long>()) { sendError(400, "duration must be signed seconds."); return; }
  long duration = input["duration"].as<long>();
  if (!duration || duration < -31536000L || duration > 31536000L) { sendError(400, "Duration must be between -365 and 365 days."); return; }
  JsonDocument body; body["duration"] = duration; String payload; serializeJson(body, payload); String response;
  if (!apiRequest("POST", "/locks/" + lockId + "/update-time", payload, response)) { sendError(502, lastError); return; }
  // Defer the follow-up GET until after the control response has been delivered.
  lastRefresh = millis() - REFRESH_MS;
  JsonDocument out; out["ok"] = true;
  String message = duration > 0 ? "Chaster accepted the add-time request." : "Chaster accepted the remove-time request.";
  out["message"] = message + " Updating lock details…";
  String result; serializeJson(out, result); sendJson(200, result);
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
  String ssid = d["ssid"] | ""; String pass = d["wifiPassword"] | ""; String token = d["token"] | "";
  ssid.trim(); token.trim();
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  String newDashPassword = d["dashboardPassword"] | ""; newDashPassword.trim();
  if (!ssid.length() || !token.length()) { sendError(400, "Wi-Fi SSID and wearer Chaster token are required."); return; }
  if (!dashboardPassword.length() && newDashPassword.length() < 10) { sendError(400, "Set a dashboard password of at least 10 characters."); return; }
  if (holderToken.length() || wearerToken.length()) { sendError(409, "This device is already configured. Use handover or reset."); return; }
  // First connect the station interface to the supplied Wi-Fi; the setup AP can remain active during this step.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t wifiStarted = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStarted < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) { sendError(502, "Could not connect to the supplied Wi-Fi network. Check the network name and password, then retry."); return; }
  String discoveredWearerId;
  if (!getAccountUserId(token, "wearer", discoveredWearerId)) { sendError(502, lastError); return; }
  prefs.begin("chaster", false);
  prefs.putString("wearer_token", token); prefs.putString("wearer_user_id", discoveredWearerId); prefs.remove("holder_lock_id"); prefs.putString("ssid", ssid);
  if (!dashboardPassword.length()) prefs.putString("dash_pw", newDashPassword);
  if (pass.length()) prefs.putString("wifi_pw", pass); else prefs.remove("wifi_pw");
  prefs.putString("role", "wearer"); prefs.putBool("handover", false);
  prefs.remove("holder_token"); prefs.remove("token"); prefs.remove("wearer_lock_id"); prefs.remove("lock_id");
  prefs.end();
  wearerToken = token; holderToken = ""; role = "wearer"; handoverPending = false; wearerUserId = discoveredWearerId; holderUserId = "";
  wifiSsid = ssid; wifiPassword = pass; lockId = ""; lastJson = "{}"; lastError = "";
  sendJson(200, "{\"ok\":true}");
  delay(500); ESP.restart();
}
void handleHandover() {
  if (!requireAuth()) return;
  if (role != "wearer" || !wearerToken.length() || holderToken.length()) { sendError(409, "Handover is available only after wearer setup and before keyholder activation."); return; }
  prefs.begin("chaster", false);
  prefs.putString("role", "keyholder"); prefs.putBool("handover", true);
  prefs.remove("ssid"); prefs.remove("wifi_pw"); prefs.remove("dash_pw");
  prefs.remove("wearer_lock_id"); prefs.remove("token"); prefs.remove("lock_id");
  prefs.end();
  holderToken = ""; role = "keyholder"; handoverPending = true;
  wifiSsid = ""; wifiPassword = ""; dashboardPassword = "";
  // Start the handover AP immediately, then start it again during boot after
  // restart. This avoids relying on stale station-mode state from wearer Wi-Fi.
  startSetupAP();
  // Send the HTTP response first. The main loop performs the restart after
  // the response has had time to leave the ESP32's TCP stack.
  sendJson(200, "{\"ok\":true}");
  handoverRestartPending = true;
}
void handleKeyholderSetup() {
  if (!requireAuth()) return;
  if (role != "keyholder" || !handoverPending || !wearerToken.length()) { sendError(409, "Keyholder setup is available only after the wearer starts handover."); return; }
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String token = d["token"] | ""; String ssid = d["ssid"] | ""; String pass = d["wifiPassword"] | ""; String password = d["dashboardPassword"] | "";
  token.trim(); ssid.trim(); password.trim();
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  if (!token.length()) { sendError(400, "Your Chaster developer token is required."); return; }
  if (!ssid.length()) { sendError(400, "Your Wi-Fi network name is required."); return; }
  if (password.length() < 10) { sendError(400, "Set a dashboard password of at least 10 characters."); return; }
  // Connect using the keyholder's supplied network before calling Chaster;
  // the wearer network was deliberately cleared during handover.
  WiFi.mode(WIFI_AP_STA); WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) { sendError(502, "Could not connect to your Wi-Fi network. Check the network name and password, then retry."); return; }
  String discoveredHolderId;
  if (!getAccountUserId(token, "keyholder", discoveredHolderId)) { sendError(502, lastError); return; }
  // Save the validated keyholder identity and settings now. The wearer may
  // not yet have transferred the lock; background polling will keep looking.
  prefs.begin("chaster", false);
  prefs.putString("holder_token", token); prefs.putString("holder_user_id", discoveredHolderId); prefs.remove("holder_lock_id");
  prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("wifi_pw", pass); else prefs.remove("wifi_pw");
  prefs.putString("dash_pw", password);
  prefs.putString("role", "keyholder"); prefs.putBool("handover", true);
  prefs.end();
  holderToken = token; holderUserId = discoveredHolderId; lockId = "";
  wifiSsid = ssid; wifiPassword = pass; dashboardPassword = password; handoverPending = true;
  lastMatchAttempt = 0;
  sendJson(200, "{\"ok\":true,\"pending\":true}"); delay(500); ESP.restart();
}
void handleUpdateToken() {
  if (!requireAuth()) return;
  if (role != "keyholder" || handoverPending) { sendError(403, "Keyholder token replacement is available only in keyholder mode."); return; }
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Invalid JSON."); return; }
  String token = d["token"] | ""; token.trim();
  if (token.startsWith("Bearer ")) { token.remove(0, 7); token.trim(); }
  if (!token.length()) { sendError(400, "A Chaster developer token is required."); return; }
  String replacementHolderId;
  if (!getAccountUserId(token, "keyholder", replacementHolderId)) { sendError(502, lastError); return; }
  if (replacementHolderId != holderUserId) { sendError(409, "The replacement token belongs to a different Chaster account. The keyholder identity cannot be changed without a full reset and new wearer handover."); return; }
  prefs.begin("chaster", false); prefs.putString("holder_token", token); prefs.end();
  sendJson(200, "{\"ok\":true}"); delay(500); ESP.restart();
}
void handleUpdateWiFi() {
  if (!requireAuth()) return;
  if (!holderToken.length() && !wearerToken.length()) { sendError(409, "Complete wearer setup before changing Wi-Fi."); return; }
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
void stopSetupAP() {
  if (!setupAP) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  setupAP = false;
  Serial.println("Setup AP disabled; device is fully configured and connected to Wi-Fi.");
  WiFi.mode(WIFI_STA);
}
void startSetupAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  if (setupAP) return;
  // Retry AP startup rather than leaving the device silently unreachable if
  // the radio is not ready immediately after a reboot.
  for (uint8_t attempt = 0; attempt < 3 && !setupAP; ++attempt) {
    setupAP = WiFi.softAP(AP_NAME, AP_PASSWORD, 1, false, 4);
    if (!setupAP) delay(500);
  }
  if (setupAP) {
    dnsServer.start(53, "*", AP_IP);
    Serial.print("Setup AP ready: "); Serial.println(AP_NAME);
    Serial.print("Setup AP IP: "); Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: Setup AP failed to start after 3 attempts.");
  }
}
void connectWiFi() {
  // Keep the device access point running permanently, including while the
  // station interface is connected to the home/keyholder network. This keeps
  // the dashboard reachable at http://192.168.4.1 and avoids losing the browser
  // connection when the station connection changes or briefly drops.
  startSetupAP();
  if (!wifiSsid.length()) return;
  if (handoverPending && !holderToken.length()) return;
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi station connection failed; setup AP remains available.");
  } else {
    Serial.print("Wi-Fi connected. Dashboard also available at setup AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("Home network IP: ");
    Serial.println(WiFi.localIP());
  }
}
void loadConfig() {
  prefs.begin("chaster", true);
  wifiSsid = prefs.getString("ssid", ""); wifiPassword = prefs.getString("wifi_pw", ""); dashboardPassword = prefs.getString("dash_pw", "");
  holderToken = prefs.getString("holder_token", "");
  wearerToken = prefs.getString("wearer_token", "");
  wearerUserId = prefs.getString("wearer_user_id", ""); holderUserId = prefs.getString("holder_user_id", "");
  role = prefs.getString("role", holderToken.length() ? "keyholder" : "wearer");
  lockId = prefs.getString("holder_lock_id", "");
  handoverPending = prefs.getBool("handover", false);
  if (handoverPending) { role = "keyholder"; if (!holderToken.length()) { wifiSsid = ""; wifiPassword = ""; dashboardPassword = ""; } }
  else if (holderToken.length()) { role = "keyholder"; handoverPending = prefs.getBool("handover", false) || !wifiSsid.length() || !dashboardPassword.length(); }
  else if (wearerToken.length()) role = "wearer";
  else { handoverPending = true; wifiSsid = ""; wifiPassword = ""; }
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
  
  server.on("/api/history", HTTP_POST, handleHistory);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/handover", HTTP_POST, handleHandover);
  server.on("/api/keyholder-setup", HTTP_POST, handleKeyholderSetup);
  server.on("/api/set-password", HTTP_POST, handleSetPassword);
  server.on("/api/update-wifi", HTTP_POST, handleUpdateWiFi);
  server.on("/api/update-token", HTTP_POST, handleUpdateToken);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound(captive);
  server.begin();
  if (WiFi.status() == WL_CONNECTED && activeToken().length()) refreshLock();
  if (handoverPending && holderToken.length()) lastMatchAttempt = millis() - 60000;
  Serial.println("Chaster ESP32 dashboard ready.");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}
void loop() {
  dnsServer.processNextRequest(); server.handleClient();
  if (handoverRestartPending) {
    delay(1200); // Allow the browser to receive the successful handover response.
    ESP.restart();
  }
  // Keep the setup AP available as a recovery path if the configured Wi-Fi drops later.
  static uint32_t lastWifiAttempt = 0;
  if (wifiSsid.length() && (!handoverPending || holderToken.length()) && WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt >= 15000) {
    if (!setupAP) startSetupAP();
    WiFi.reconnect();
    lastWifiAttempt = millis();
  }
  // The setup AP is only needed during provisioning or as a recovery path.
  // Once credentials are saved and station Wi-Fi is connected, shut it down.
  bool fullyConfigured = (wearerToken.length() || holderToken.length()) &&
                         wifiSsid.length() && dashboardPassword.length() &&
                         WiFi.status() == WL_CONNECTED;
  if (fullyConfigured) {
    if (setupAP) stopSetupAP();
  } else if (!setupAP) {
    startSetupAP();
  }
  if (handoverPending && holderToken.length() && WiFi.status() == WL_CONNECTED && millis() - lastMatchAttempt >= 60000) {
    lastMatchAttempt = millis();
    String foundLockId;
    if (discoverWearerLock(holderToken, holderUserId, foundLockId)) {
      lockId = foundLockId;
      prefs.begin("chaster", false);
      prefs.putString("holder_lock_id", lockId);
      prefs.putBool("handover", false);
      prefs.end();
      handoverPending = false;
      lastError = "";
      refreshLock();
    } else {
      Serial.println("Handover pending: active wearer lock not found yet; retrying in 60 seconds.");
    }
  }
  if (WiFi.status() == WL_CONNECTED && activeToken().length() && !handoverPending && millis() - lastRefresh >= REFRESH_MS) refreshLock();
  delay(2);
}
