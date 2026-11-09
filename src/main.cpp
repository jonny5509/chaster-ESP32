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
String wifiSsid, wifiPassword, holderToken, lockId, dashboardPassword;
String role = "keyholder", lastJson = "{}", lastError;
uint32_t lastRefresh = 0;
bool handoverPending = false;

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;"); return s;
}
// Only the keyholder account is ever used for Chaster API requests.
String activeToken() { return handoverPending ? "" : holderToken; }
bool requireAuth() {
  if (dashboardPassword.length()) {
    if (server.authenticate("admin", dashboardPassword.c_str())) return true;
    server.requestAuthentication(); return false;
  }
  // Legacy devices without a dashboard password may only view setup/state and set one.
  // Block every control/configuration endpoint until the password is provisioned.
  String uri = server.uri();
  bool provisioned = holderToken.length();
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
  if (!token.length()) { lastError = "No keyholder Chaster API token configured."; return false; }
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
  else ok = apiRequest("POST", "/keyholder/locks/search", R"({"limit":50,"page":0,"status":"locked"})", response);
  if (ok) { lastJson = response; lastRefresh = millis(); }
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
</style></head><body><main>
<header class="topbar"><div class="brand"><div class="logo">C</div><div><div class="eyebrow">Chaster device</div><h1>Control centre</h1></div></div><div class="pill"><i id="connectionDot" class="dot"></i><span id="connectionText">Connecting…</span></div></header>
<div id="status" class="notice">Connecting to your ESP32…</div>
<section id="setup" class="card"><div class="eyebrow">First-time setup</div><h2 style="font-size:22px;margin:5px 0 8px">Connect your wearer account</h2><div id="handoverInfo" class="notice hidden">Enter the wearer's Wi-Fi details, Chaster developer token and lock ID.</div>
<form id="config"><label>Wi-Fi network name</label><input name="ssid" required autocomplete="off"><label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password"><label>Wearer Chaster developer token</label><input name="token" type="password" required autocomplete="new-password"><label>Chaster lock ID</label><input name="lockId" required autocomplete="off" placeholder="Paste the lock ID"><label>Dashboard password (minimum 10 characters)</label><input name="dashboardPassword" type="password" minlength="10" autocomplete="new-password"><button class="submit" type="submit" id="saveBtn">Save and connect</button></form><p class="muted">All Chaster requests are made using this wearer account.</p></section>
<section id="dashboard" class="hidden">
<div class="hero"><div class="herohead"><div><div class="eyebrow">Wearer dashboard</div><h2 id="heroTitle">Your selected lock</h2><p id="heroSubtitle">Live overview from Chaster</p></div><span class="tag" id="lockStatus">WAITING FOR DATA</span></div>
<div class="lockid" id="heroLockId">No lock ID selected</div>
<div class="stats"><div class="stat"><div class="statlabel">Time elapsed</div><div class="statvalue" id="elapsed">—</div></div><div class="stat"><div class="statlabel">Time remaining</div><div class="statvalue" id="remaining">—</div></div><div class="stat"><div class="statlabel">Lock state</div><div class="statvalue" id="frozenState">—</div></div></div>
<div class="progress-track"><div class="progress-fill" id="timeProgress"></div></div><div class="progress-caption"><span id="progressLeft">Lock progress</span><span id="progressRight">Waiting for lock dates</span></div></div>
<div class="layout"><div class="stack">
<section class="card"><div class="cardhead"><h2>Session</h2><span class="tag">LOCK DETAILS</span></div><div class="details" id="sessionDetails"></div><details><summary>View raw Chaster response</summary><pre id="lockData">Waiting for Chaster data…</pre></details></section>
<section class="card"><div class="cardhead"><h2>Wearer</h2><span class="tag">ACCOUNT</span></div><div class="details" id="wearerDetails"></div></section>
<section class="card"><div class="cardhead"><h2>Keyholder</h2><span class="tag">ACCOUNT & PERMISSIONS</span></div><div class="details" id="keyholderDetails"></div><details><summary>View keyholder permissions</summary><pre id="permissionData">Waiting for permission data…</pre></details></section>
<section class="card"><div class="cardhead"><h2>Lock history</h2><span class="tag">RECENT EVENTS</span></div><p class="muted">History for the selected lock, fetched directly from Chaster.</p><button class="secondary" onclick="loadHistory()">Load recent history</button><pre class="history" id="history">Select a lock to view its history.</pre></section>

</div><div class="stack">

<section class="card"><div class="cardhead"><h2>Connection</h2><span class="tag">DEVICE</span></div><div class="detail"><span>Wi-Fi network</span><strong id="networkName">—</strong></div><div class="detail" style="margin-top:9px"><span>Device address</span><strong id="deviceIp">—</strong></div><form id="wifiForm"><label>Change Wi-Fi network</label><input id="wifiSsid" name="ssid" required autocomplete="off" placeholder="Network name"><label>Wi-Fi password</label><input name="wifiPassword" type="password" autocomplete="new-password" placeholder="Leave blank for open network"><button class="submit" type="submit">Save Wi-Fi settings</button></form></section>
<div id="passwordSetup" class="card hidden"><h2>Secure dashboard</h2><p class="muted">Set a unique password to protect dashboard controls.</p><form id="passwordForm"><label>New dashboard password</label><input name="password" type="password" minlength="10" required autocomplete="new-password"><button class="submit" type="submit">Save password</button></form></div>
<section class="card"><div class="cardhead"><h2>Configuration</h2><span class="tag">DEVICE & KEYHOLDER</span></div><div class="eyebrow" style="margin:4px 0 8px">Selected lock</div><p class="muted">Choose which lock this keyholder device manages.</p><label>Chaster lock ID</label><input id="lockIdInput" autocomplete="off" spellcheck="false" placeholder="Paste the Chaster lock ID"><button style="width:100%;margin-top:10px" onclick="saveLockId()">Save selected lock</button><div id="lockMessage" class="message">The dashboard refreshes the selected lock automatically.</div><div style="height:1px;background:var(--line);margin:20px 0"></div><div class="eyebrow" style="margin:4px 0 8px">Account & device</div><form id="tokenUpdate"><label>Replace keyholder Chaster token</label><input name="token" type="password" required autocomplete="new-password" placeholder="Paste a fresh keyholder token"><button class="submit" type="submit">Update Chaster token</button></form><button class="danger" style="width:100%;margin-top:12px" onclick="resetDevice()">Clear all credentials</button></section>
</div></div></section><div class="footer">Chaster Control Centre · ESP32 keyholder edition</div></main><script>
const $=id=>document.getElementById(id);let selectedLockId='',lockStart=0,lockEnd=0,lastData={};
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
function renderData(raw){lastData=raw||{};const d=unwrap(lastData);const id=pick(d,['_id','id','lockId'])||selectedLockId;const title=pick(d,['title','name','lockName'])||'Selected Chaster lock';const status=pick(d,['status','state'])||'Unknown';const frozen=pick(d,['isFrozen','frozen']);const start=dateValue(pick(d,['startDate','startAt','startedAt','createdAt','start']));const end=dateValue(pick(d,['endDate','endAt','endsAt','expectedEndDate','end']));lockStart=start;lockEnd=end;
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
$('permissionData').textContent=grants.length?grants.map(g=>{const subjects=g.subjects||{};return (g.resource||'Permission')+'\\n  Keyholder: '+((subjects.keyholder||[]).join(', ')||'Not granted')+'\\n  Wearer: '+((subjects.wearer||[]).join(', ')||'Not granted')}).join('\\n\\n'):'No permission grants supplied by Chaster.';
$('lockData').textContent=JSON.stringify(lastData,null,2);
const total=end&&start?end-start:0,elapsed=start?Date.now()-start:0,remaining=end?end-Date.now():0;const pct=total>0?Math.max(0,Math.min(100,elapsed/total*100)):0;$('timeProgress').style.width=pct+'%';msg('progressLeft',total>0?Math.round(pct)+'% elapsed':'Lock progress');msg('progressRight',total>0?duration(Math.max(0,remaining))+' left':'Waiting for lock dates');timer()}
function timer(){msg('elapsed',lockStart?duration(Date.now()-lockStart):'—');msg('remaining',lockEnd?duration(lockEnd-Date.now()):'—')}
async function loadState(){try{const x=await call('/api/state');selectedLockId=x.lockId||'';if(!$('lockIdInput').matches(':focus'))$('lockIdInput').value=selectedLockId;if(!x.configured){$('setup').classList.remove('hidden');$('dashboard').classList.add('hidden');$('handoverInfo').classList.remove('hidden');msg('status','Wearer setup · connect to Chaster-ESP32');$('connectionText').textContent='Setup required';$('connectionDot').className='dot';return}
$('setup').classList.add('hidden');$('dashboard').classList.remove('hidden');$('passwordSetup').classList.toggle('hidden',!x.needsPassword);$('networkName').textContent=x.wifiSsid||'—';$('deviceIp').textContent=x.ip||'—';if(!$('wifiSsid').value)$('wifiSsid').value=x.wifiSsid||'';const connected=!!x.connected;$('connectionText').textContent=connected?'Wi-Fi connected':'Wi-Fi offline';$('connectionDot').className='dot '+(connected?'good':'bad');msg('status',x.apiError?'Chaster API · '+x.apiError:(connected?'Connected to '+(x.wifiSsid||'Wi-Fi')+' · '+x.ip:'Wi-Fi disconnected'),x.apiError?'error':'success');renderData(x.data||{});if(!selectedLockId)msg('lockMessage','Enter and save a lock ID to load its details.')}catch(e){$('setup').classList.remove('hidden');$('dashboard').classList.add('hidden');$('handoverInfo').classList.remove('hidden');$('connectionText').textContent='Setup mode';$('connectionDot').className='dot bad';msg('status','Could not read device state: '+e.message+'. You can still try entering your setup details below.','error')}}
async function refresh(){try{await call('/api/refresh',{method:'POST'});await loadState()}catch(e){msg('actionMsg',e.message,'error')}}
async function saveLockId(){const id=$('lockIdInput').value.trim();if(!id){msg('lockMessage','Enter a Chaster lock ID.','error');return}try{await call('/api/select-lock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({lockId:id})});msg('lockMessage','Selected lock saved. Loading its Chaster details.','success');await refresh()}catch(e){msg('lockMessage',e.message,'error')}}
async function loadHistory(){if(!selectedLockId){msg('history','Enter and save a Chaster lock ID first.');return}msg('history','Loading recent history for '+selectedLockId+'…');try{const x=await call('/api/history',{method:'POST'});msg('history',JSON.stringify(x.data,null,2))}catch(e){msg('history','Could not load history: '+e.message)}}
async function resetDevice(){if(!confirm('Erase all Chaster credentials and Wi-Fi settings?'))return;try{await call('/api/reset',{method:'POST'});msg('status','Credentials cleared. Restarting…')}catch(e){msg('status',e.message,'error')}}
$('config').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target),p={ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword'),token:f.get('token'),lockId:f.get('lockId'),dashboardPassword:f.get('dashboardPassword')};try{await call('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});msg('status','Saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('wifiForm').addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(e.target);try{await call('/api/update-wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:f.get('ssid'),wifiPassword:f.get('wifiPassword')})});msg('status','Wi-Fi settings saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('passwordForm').addEventListener('submit',async e=>{e.preventDefault();const password=new FormData(e.target).get('password');try{await call('/api/set-password',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password})});msg('status','Dashboard password saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
$('tokenUpdate').addEventListener('submit',async e=>{e.preventDefault();const token=new FormData(e.target).get('token');try{await call('/api/update-token',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token})});msg('status','Keyholder token saved. Restarting…')}catch(err){msg('status',err.message,'error')}});
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
  bool configured = holderToken.length() > 0 && wifiSsid.length() > 0;
  d["ok"] = true; d["configured"] = configured;
  d["role"] = "keyholder"; d["connected"] = WiFi.status() == WL_CONNECTED;
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
  String requestedId = d["lockId"] | "";
  requestedId.trim();
  if (requestedId.length() > 128 || requestedId.indexOf("/") >= 0 || requestedId.indexOf("\\") >= 0 || requestedId.indexOf("?") >= 0 || requestedId.indexOf("#") >= 0) {
    sendError(400, "Lock ID is invalid."); return;
  }
  lockId = requestedId;
  // Persist the one lock selected for this keyholder device.
  prefs.begin("chaster", false);
  prefs.putString("holder_lock_id", lockId);
  // Prevent legacy migration from restoring a stale selection after reboot.
  prefs.remove("lock_id");
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
  // The device is configured only with the keyholder account.
  if (holderToken.length()) { sendError(409, "A keyholder token is already configured. Replace it from the dashboard."); return; }
  prefs.begin("chaster", false);
  prefs.putString("holder_token", token); prefs.putString("holder_lock_id", newLockId); prefs.putString("ssid", ssid);
  if (!dashboardPassword.length()) prefs.putString("dash_pw", newDashPassword);
  if (pass.length()) prefs.putString("wifi_pw", pass); else prefs.remove("wifi_pw");
  prefs.putString("role", "keyholder"); prefs.putBool("handover", false);
  prefs.remove("wearer_token"); prefs.remove("token"); prefs.remove("wearer_lock_id"); prefs.remove("lock_id");
  prefs.end();
  holderToken = token; role = "keyholder"; handoverPending = false;
  wifiSsid = ssid; wifiPassword = pass; lockId = newLockId; lastJson = "{}"; lastError = "";
  server.send(200, "application/json", "{\"ok\":true}");
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
  if (!holderToken.length()) { sendError(409, "Complete keyholder setup before changing Wi-Fi."); return; }
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
  holderToken = prefs.getString("holder_token", "");
  role = "keyholder"; handoverPending = false;
  lockId = prefs.getString("holder_lock_id", "");
  // A previous wearer-only installation must be provisioned with a keyholder token.
  if (!holderToken.length()) { handoverPending = true; wifiSsid = ""; wifiPassword = ""; }
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
