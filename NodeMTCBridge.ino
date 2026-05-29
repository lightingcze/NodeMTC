#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
 
#include <DNSServer.h>
#include <WiFiManager.h>
 
#include <AppleMIDI.h>
#include <MIDI.h>
 
// ─────────────────────────────────────────────────────────────────────────────
//  NodeMTCBridge (ESP8266 / NodeMCU)
//  RTP-MIDI (AppleMIDI) -> forward only MTC Quarter Frame -> DIN MIDI OUT
//  - Auto captive portal when no known Wi-Fi available
//  - Portal lets you set Wi-Fi + device name (persistent)
//  - After connect: rich status web UI + JSON API
// ─────────────────────────────────────────────────────────────────────────────
 
// MIDI OUT: ESP8266 UART1 is TX-only on GPIO2 (NodeMCU: D4)
static constexpr uint32_t MIDI_BAUD = 31250;
 
static const char* CONFIG_PATH = "/config.json";
static constexpr size_t DEVICE_NAME_MAX = 32;
 
static char g_deviceName[DEVICE_NAME_MAX + 1] = "NodeMTC";
static bool g_shouldSaveConfig = false;
 
static ESP8266WebServer g_server(80);
static bool g_mdnsOk = false;
 
// Note: AppleMIDI session name is fixed at compile time by this macro.
// The user-configurable "device name" is used for web UI + mDNS hostname.
APPLEMIDI_CREATE_INSTANCE(WiFiUDP, MIDI, "NodeMTC", DEFAULT_CONTROL_PORT);
 
struct MTCState {
  uint8_t frame = 0, sec = 0, min = 0, hour = 0;
  uint8_t rateBits = 0; // 0..3
  bool valid = false;
  uint32_t lastUpdateMs = 0;
  uint32_t qfCount = 0;
} g_mtc;
 
static uint8_t g_qf[8] = {0};
static bool g_qfSeen[8] = {false};
 
static uint32_t g_midiTotalIn = 0;
static uint32_t g_midiTotalForwarded = 0;
static uint32_t g_bootMs = 0;
 
static const char* rateLabel(uint8_t rb) {
  switch (rb & 0x03) {
    case 0: return "24";
    case 1: return "25";
    case 2: return "29.97d";
    case 3: return "30";
  }
  return "?";
}
 
static String two(uint8_t v) { return (v < 10 ? "0" : "") + String(v); }
 
static String sanitizeHostname(const char* name) {
  String out;
  out.reserve(32);
  for (size_t i = 0; name[i] && out.length() < 24; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += (char)tolower(c);
    } else if (c == ' ' || c == '_' || c == '-') {
      if (out.length() && out[out.length() - 1] != '-') out += '-';
    }
  }
  if (!out.length()) out = "nodemtc";
  return out;
}
 
static void saveConfigIfNeeded(const char* deviceName) {
  if (!g_shouldSaveConfig) return;
  g_shouldSaveConfig = false;
 
  if (!LittleFS.begin()) return;
 
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return;
 
  // Minimal JSON; keep it simple to avoid extra deps.
  f.print("{\"device_name\":\"");
  for (size_t i = 0; deviceName[i]; i++) {
    char c = deviceName[i];
    if (c == '\\' || c == '"') f.print('\\');
    f.print(c);
  }
  f.print("\"}\n");
  f.close();
}
 
static void loadConfig() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists(CONFIG_PATH)) return;
 
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
 
  String s = f.readString();
  f.close();
 
  // Very small JSON extractor: look for "device_name":"...".
  int keyPos = s.indexOf("\"device_name\"");
  if (keyPos < 0) return;
  int colon = s.indexOf(':', keyPos);
  if (colon < 0) return;
  int q1 = s.indexOf('"', colon + 1);
  if (q1 < 0) return;
  int q2 = s.indexOf('"', q1 + 1);
  if (q2 < 0) return;
 
  String name = s.substring(q1 + 1, q2);
  name.trim();
  if (!name.length()) return;
 
  name.toCharArray(g_deviceName, sizeof(g_deviceName));
}
 
static void onSaveConfig() { g_shouldSaveConfig = true; }
 
// AppleMIDI session events (optional but nice for the UI)
static uint32_t g_sessions = 0;
static uint32_t g_sessionDisconnects = 0;
static void onAppleMidiConnected(const APPLEMIDI_NAMESPACE::ssrc_t& ssrc, const char* name) {
  (void)ssrc; (void)name;
  g_sessions++;
}
static void onAppleMidiDisconnected(const APPLEMIDI_NAMESPACE::ssrc_t& ssrc) {
  (void)ssrc;
  g_sessionDisconnects++;
}
 
static void onMTCQuarterFrame(byte data) {
  g_midiTotalIn++;
  g_mtc.qfCount++;
 
  // Forward only MTC Quarter Frame to DIN MIDI OUT
  Serial1.write(0xF1);
  Serial1.write(data);
  g_midiTotalForwarded++;
 
  uint8_t msg = (data >> 4) & 0x07;
  uint8_t val = data & 0x0F;
 
  g_qf[msg] = val;
  g_qfSeen[msg] = true;
 
  // When type 7 arrives, we likely have a whole frame's worth.
  if (msg == 7) {
    bool complete = true;
    for (int i = 0; i < 8; i++) complete &= g_qfSeen[i];
    if (!complete) return;
 
    uint8_t frame = (uint8_t)(((g_qf[1] & 0x01) << 4) | (g_qf[0] & 0x0F));
    uint8_t sec   = (uint8_t)(((g_qf[3] & 0x03) << 4) | (g_qf[2] & 0x0F));
    uint8_t min   = (uint8_t)(((g_qf[5] & 0x03) << 4) | (g_qf[4] & 0x0F));
    uint8_t hour  = (uint8_t)(((g_qf[7] & 0x01) << 4) | (g_qf[6] & 0x0F));
    uint8_t rateB = (uint8_t)((g_qf[7] >> 1) & 0x03);
 
    g_mtc.frame = frame;
    g_mtc.sec = sec;
    g_mtc.min = min;
    g_mtc.hour = hour;
    g_mtc.rateBits = rateB;
    g_mtc.valid = true;
    g_mtc.lastUpdateMs = millis();
 
    for (int i = 0; i < 8; i++) g_qfSeen[i] = false;
  }
}
 
static String formatUptime(uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t days = s / 86400; s %= 86400;
  uint32_t hours = s / 3600; s %= 3600;
  uint32_t mins = s / 60; s %= 60;
  char buf[48];
  if (days) {
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", (unsigned long)days, (unsigned long)hours,
             (unsigned long)mins, (unsigned long)s);
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)hours, (unsigned long)mins, (unsigned long)s);
  }
  return String(buf);
}
 
static void handleRoot() {
  static const char PAGE[] PROGMEM =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>NodeMTC</title>"
    "<style>"
    ":root{--bg:#0b1020;--card:#121a33;--muted:#9fb0ff;--text:#eef2ff;--ok:#3ddc97;--bad:#ff5c7a;--warn:#ffc857}"
    "body{margin:0;font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;background:radial-gradient(1200px 800px at 10% 0%,#17214a 0%,var(--bg) 55%);color:var(--text)}"
    ".wrap{max-width:980px;margin:0 auto;padding:18px}"
    ".top{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap}"
    ".brand{display:flex;flex-direction:column;gap:2px}"
    ".title{font-size:18px;font-weight:700;letter-spacing:.3px}"
    ".sub{font-size:12px;color:var(--muted)}"
    ".grid{display:grid;grid-template-columns:repeat(12,1fr);gap:12px;margin-top:14px}"
    ".card{grid-column:span 12;background:rgba(18,26,51,.72);border:1px solid rgba(159,176,255,.18);backdrop-filter: blur(10px);border-radius:14px;padding:14px}"
    ".card h3{margin:0 0 10px 0;font-size:13px;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.08em}"
    ".row{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px solid rgba(159,176,255,.12)}"
    ".row:last-child{border-bottom:none}"
    ".k{color:var(--muted);font-size:12px}"
    ".v{font-size:12px;word-break:break-word}"
    ".pill{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border-radius:999px;border:1px solid rgba(159,176,255,.18);background:rgba(11,16,32,.35);font-size:12px}"
    "select{appearance:none;-webkit-appearance:none;-moz-appearance:none;padding:4px 28px 4px 10px;border-radius:10px;"
    "border:1px solid rgba(159,176,255,.22);background:rgba(11,16,32,.70);color:var(--text);}"
    "select:focus{outline:none;border-color:rgba(159,176,255,.55)}"
    "option{background:var(--card);color:var(--text)}"
    ".dot{width:8px;height:8px;border-radius:50%}"
    ".btns{display:flex;gap:8px;flex-wrap:wrap}"
    "button,a.btn{cursor:pointer;appearance:none;border:1px solid rgba(159,176,255,.22);background:rgba(11,16,32,.55);color:var(--text);padding:8px 10px;border-radius:10px;font-size:12px;text-decoration:none}"
    "button:hover,a.btn:hover{border-color:rgba(159,176,255,.45)}"
    ".big{font-size:30px;font-weight:800;letter-spacing:.04em}"
    ".tc-small .big{font-size:22px}"
    ".tc-medium .big{font-size:30px}"
    ".tc-large .big{font-size:44px}"
    ".tc-huge .big{font-size:64px}"
    ".style-mono .big{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}"
    ".style-rounded .big{font-family:ui-rounded,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial}"
    ".style-digital .big{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;text-shadow:0 0 10px rgba(159,176,255,.35),0 0 22px rgba(61,220,151,.18);letter-spacing:.08em}"
    ".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace}"
    "@media(min-width:840px){.card.w6{grid-column:span 6}.card.w4{grid-column:span 4}.card.w8{grid-column:span 8}}"
    "</style></head><body><div class='wrap'>"
    "<div class='top'>"
    "<div class='brand'><div class='title' id='devName'>NodeMTC</div><div class='sub'>RTP-MIDI → MTC → DIN MIDI OUT</div></div>"
    "<div class='btns'>"
    "<a class='btn' href='/wifi'>Wi‑Fi setup</a>"
    "<a class='btn' href='/api/status' target='_blank'>JSON</a>"
    "<button onclick='reboot()'>Reboot</button>"
    "<button onclick='wipe()'>Wi‑Fi reset</button>"
    "</div>"
    "</div>"
    "<div style='margin-top:12px;display:flex;gap:10px;flex-wrap:wrap;align-items:center'>"
    "<div class='pill'>Timecode size:"
    "<select id='tcSize' style='margin-left:8px;background:transparent;color:var(--text);border:none;outline:none'>"
    "<option value='tc-small'>Small</option>"
    "<option value='tc-medium' selected>Medium</option>"
    "<option value='tc-large'>Large</option>"
    "<option value='tc-huge'>Huge</option>"
    "</select></div>"
    "<div class='pill'>Style:"
    "<select id='tcStyle' style='margin-left:8px;background:transparent;color:var(--text);border:none;outline:none'>"
    "<option value='style-mono' selected>Mono</option>"
    "<option value='style-rounded'>Rounded</option>"
    "<option value='style-digital'>Digital</option>"
    "</select></div>"
    "<div class='pill'>Refresh:"
    "<select id='tcHz' style='margin-left:8px;background:transparent;color:var(--text);border:none;outline:none'>"
    "<option value='100'>100ms</option>"
    "<option value='250' selected>250ms</option>"
    "<option value='500'>500ms</option>"
    "<option value='1000'>1000ms</option>"
    "</select></div>"
    "</div>"
    "<div style='margin-top:10px;display:flex;gap:10px;flex-wrap:wrap'>"
    "<div class='pill'><span class='dot' id='wifiDot' style='background:var(--bad)'></span><span id='wifiPill'>Wi‑Fi: -</span></div>"
    "<div class='pill'><span class='dot' id='mtcDot' style='background:var(--warn)'></span><span id='mtcPill'>MTC: --:--:--:--</span></div>"
    "<div class='pill'><span class='dot' id='rtpDot' style='background:var(--warn)'></span><span id='rtpPill'>RTP-MIDI: -</span></div>"
    "</div>"
    "<div class='grid'>"
    "<div class='card w8'><h3>Timecode</h3>"
    "<div class='big mono' id='mtcBig'>--:--:--:--</div>"
    "<div style='margin-top:10px' class='row'><div class='k'>FPS</div><div class='v mono' id='fps'>-</div></div>"
    "<div class='row'><div class='k'>Last update</div><div class='v mono' id='age'>-</div></div>"
    "<div class='row'><div class='k'>Quarter frames</div><div class='v mono' id='qf'>-</div></div>"
    "<div class='row'><div class='k'>QF rate</div><div class='v mono' id='qfr'>-</div></div>"
    "<div class='row'><div class='k'>Forwarded</div><div class='v mono' id='fwd'>-</div></div>"
    "</div>"
    "<div class='card w4'><h3>System</h3>"
    "<div class='row'><div class='k'>Uptime</div><div class='v mono' id='up'>-</div></div>"
    "<div class='row'><div class='k'>Heap free</div><div class='v mono' id='heap'>-</div></div>"
    "<div class='row'><div class='k'>Flash</div><div class='v mono' id='flash'>-</div></div>"
    "<div class='row'><div class='k'>SDK / Core</div><div class='v mono' id='sdk'>-</div></div>"
    "<div class='row'><div class='k'>Chip ID</div><div class='v mono' id='chip'>-</div></div>"
    "</div>"
    "<div class='card w6'><h3>Network</h3>"
    "<div class='row'><div class='k'>SSID</div><div class='v mono' id='ssid'>-</div></div>"
    "<div class='row'><div class='k'>IP</div><div class='v mono' id='ip'>-</div></div>"
    "<div class='row'><div class='k'>Gateway</div><div class='v mono' id='gw'>-</div></div>"
    "<div class='row'><div class='k'>DNS</div><div class='v mono' id='dns'>-</div></div>"
    "<div class='row'><div class='k'>MAC</div><div class='v mono' id='mac'>-</div></div>"
    "<div class='row'><div class='k'>RSSI / Channel</div><div class='v mono' id='rssi'>-</div></div>"
    "<div class='row'><div class='k'>mDNS</div><div class='v mono' id='mdns'>-</div></div>"
    "</div>"
    "<div class='card w6'><h3>RTP-MIDI</h3>"
    "<div class='row'><div class='k'>Session name</div><div class='v mono' id='sess'>-</div></div>"
    "<div class='row'><div class='k'>Connect events</div><div class='v mono' id='conn'>-</div></div>"
    "<div class='row'><div class='k'>Disconnect events</div><div class='v mono' id='disc'>-</div></div>"
    "</div>"
    "</div>"
    "<script>"
    "async function api(path,method){return fetch(path,{method:method||'GET'});}"
    "function dot(el,ok){el.style.background=ok?'var(--ok)':'var(--bad)';}"
    "async function reboot(){await api('/api/reboot','POST');}"
    "async function wipe(){if(!confirm('Reset Wi-Fi credentials?'))return;await api('/api/wifi_reset','POST');}"
    "function applyPrefs(){"
    " const size=localStorage.getItem('tcSize')||'tc-medium';"
    " const style=localStorage.getItem('tcStyle')||'style-mono';"
    " document.body.classList.remove('tc-small','tc-medium','tc-large','tc-huge');"
    " document.body.classList.remove('style-mono','style-rounded','style-digital');"
    " document.body.classList.add(size);"
    " document.body.classList.add(style);"
    " const s=document.getElementById('tcSize'); const st=document.getElementById('tcStyle');"
    " if(s) s.value=size; if(st) st.value=style;"
    "}"
    "let tickTimer=null;"
    "function setRefresh(){"
    " const ms=parseInt(localStorage.getItem('tcHz')||'250',10);"
    " const sel=document.getElementById('tcHz'); if(sel) sel.value=String(ms);"
    " if(tickTimer) clearInterval(tickTimer);"
    " tickTimer=setInterval(tick,ms);"
    "}"
    "function bindPrefs(){"
    " document.getElementById('tcSize').addEventListener('change',e=>{localStorage.setItem('tcSize',e.target.value);applyPrefs();});"
    " document.getElementById('tcStyle').addEventListener('change',e=>{localStorage.setItem('tcStyle',e.target.value);applyPrefs();});"
    " document.getElementById('tcHz').addEventListener('change',e=>{localStorage.setItem('tcHz',e.target.value);setRefresh();});"
    " applyPrefs(); setRefresh();"
    "}"
    "let lastQf=0; let lastT=0;"
    "async function tick(){"
    " const r=await fetch('/api/status'); const j=await r.json();"
    " document.getElementById('devName').textContent=j.device_name;"
    " const wifiOk=j.wifi_connected;"
    " dot(document.getElementById('wifiDot'),wifiOk);"
    " document.getElementById('wifiPill').textContent='Wi‑Fi: '+(wifiOk?j.ssid:'disconnected');"
    " const mtcOk=(j.mtc_state==='running');"
    " document.getElementById('mtcBig').textContent=j.mtc;"
    " document.getElementById('mtcPill').textContent='MTC: '+j.mtc+' @'+j.fps;"
    " document.getElementById('mtcDot').style.background=(j.mtc_state==='running')?'var(--ok)':((j.mtc_state==='stale')?'var(--warn)':'var(--bad)');"
    " document.getElementById('rtpDot').style.background=(j.rtp_connects>0)?'var(--ok)':'var(--warn)';"
    " document.getElementById('rtpPill').textContent='RTP-MIDI: '+(j.rtp_connects>0?'active':'waiting');"
    " document.getElementById('fps').textContent=j.fps;"
    " document.getElementById('age').textContent=(j.mtc_age_ms<0?'-':(j.mtc_age_ms+' ms'));"
    " document.getElementById('qf').textContent=j.mtc_qf;"
    " const now=Date.now();"
    " if(lastT){"
    "  const dt=(now-lastT)/1000.0; const dq=j.mtc_qf-lastQf; const rate=(dt>0)?(dq/dt):0;"
    "  document.getElementById('qfr').textContent=rate.toFixed(1)+' /s';"
    " } else { document.getElementById('qfr').textContent='-'; }"
    " lastT=now; lastQf=j.mtc_qf;"
    " document.getElementById('fwd').textContent=j.midi_forwarded;"
    " document.getElementById('up').textContent=j.uptime;"
    " document.getElementById('heap').textContent=j.heap_free+' bytes';"
    " document.getElementById('flash').textContent=j.flash_size+' bytes';"
    " document.getElementById('sdk').textContent=j.sdk+' / '+j.core;"
    " document.getElementById('chip').textContent=j.chip_id;"
    " document.getElementById('ssid').textContent=j.ssid||'-';"
    " document.getElementById('ip').textContent=j.ip||'-';"
    " document.getElementById('gw').textContent=j.gw||'-';"
    " document.getElementById('dns').textContent=j.dns||'-';"
    " document.getElementById('mac').textContent=j.mac||'-';"
    " document.getElementById('rssi').textContent=j.rssi+' dBm / ch '+j.channel;"
    " document.getElementById('mdns').textContent=j.mdns||'-';"
    " document.getElementById('sess').textContent=j.session_name;"
    " document.getElementById('conn').textContent=j.rtp_connects;"
    " document.getElementById('disc').textContent=j.rtp_disconnects;"
    "} "
    "window.addEventListener('load',()=>{bindPrefs(); tick();});"
    "</script>"
    "</div></body></html>";
 
  g_server.send_P(200, "text/html", PAGE);
}
 
static void handleStatus() {
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const uint32_t now = millis();
  const int32_t age = g_mtc.valid ? (int32_t)(now - g_mtc.lastUpdateMs) : -1;
  const char* mtcState =
    (!g_mtc.valid || age < 0) ? "none" :
    (age < 400) ? "running" :
    (age < 2000) ? "stale" : "stopped";
 
  const String mtcStr = g_mtc.valid
    ? (two(g_mtc.hour) + ":" + two(g_mtc.min) + ":" + two(g_mtc.sec) + ":" + two(g_mtc.frame))
    : "--:--:--:--";
 
  const String ssid = wifiOk ? WiFi.SSID() : "";
  const String ip = wifiOk ? WiFi.localIP().toString() : "";
  const String gw = wifiOk ? WiFi.gatewayIP().toString() : "";
  const String dns = wifiOk ? WiFi.dnsIP().toString() : "";
  const String mac = WiFi.macAddress();
 
  String json;
  json.reserve(520);
  json += "{";
  json += "\"device_name\":\"" + String(g_deviceName) + "\",";
  json += "\"session_name\":\"NodeMTC\",";
  json += "\"wifi_connected\":" + String(wifiOk ? "true" : "false") + ",";
  json += "\"ssid\":\"" + ssid + "\",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"gw\":\"" + gw + "\",";
  json += "\"dns\":\"" + dns + "\",";
  json += "\"mac\":\"" + mac + "\",";
  json += "\"rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
  json += "\"channel\":" + String(wifiOk ? WiFi.channel() : 0) + ",";
  json += "\"mdns\":\"" + String(g_mdnsOk ? (sanitizeHostname(g_deviceName) + ".local") : String("")) + "\",";
  json += "\"mtc\":\"" + mtcStr + "\",";
  json += "\"fps\":\"" + String(rateLabel(g_mtc.rateBits)) + "\",";
  json += "\"mtc_valid\":" + String(g_mtc.valid ? "true" : "false") + ",";
  json += "\"mtc_age_ms\":" + String(age) + ",";
  json += "\"mtc_state\":\"" + String(mtcState) + "\",";
  json += "\"mtc_qf\":" + String(g_mtc.qfCount) + ",";
  json += "\"midi_in\":" + String(g_midiTotalIn) + ",";
  json += "\"midi_forwarded\":" + String(g_midiTotalForwarded) + ",";
  json += "\"rtp_connects\":" + String(g_sessions) + ",";
  json += "\"rtp_disconnects\":" + String(g_sessionDisconnects) + ",";
  json += "\"uptime\":\"" + formatUptime(now - g_bootMs) + "\",";
  json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"flash_size\":" + String(ESP.getFlashChipRealSize()) + ",";
  json += "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\",";
  json += "\"core\":\"" + String(ESP.getCoreVersion()) + "\",";
  json += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\"";
  json += "}";
 
  g_server.send(200, "application/json", json);
}
 
static void handleWifi() {
  g_server.send(200, "text/plain", "Starting Wi-Fi config portal... connect to AP and configure, then return to /."); 
  delay(150);
 
  WiFiManager wm;
  wm.setSaveConfigCallback(onSaveConfig);
 
  WiFiManagerParameter p_name("devname", "Device name", g_deviceName, DEVICE_NAME_MAX);
  wm.addParameter(&p_name);
 
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(0); // keep portal running until configured
 
  const String apName = String("NodeMTC-Setup-") + String(ESP.getChipId(), HEX);
  bool ok = wm.startConfigPortal(apName.c_str());
 
  const char* newName = p_name.getValue();
  if (newName && strlen(newName)) {
    strlcpy(g_deviceName, newName, sizeof(g_deviceName));
  }
  saveConfigIfNeeded(g_deviceName);
 
  if (ok) {
    ESP.restart(); // restart so AppleMIDI + MDNS pick up new name cleanly
  }
}
 
static void handleReboot() {
  g_server.send(200, "text/plain", "Rebooting...");
  delay(150);
  ESP.restart();
}
 
static void handleWifiReset() {
  g_server.send(200, "text/plain", "Wiping Wi-Fi credentials and rebooting...");
  delay(200);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}
 
static void startMdns() {
  const String host = sanitizeHostname(g_deviceName);
  g_mdnsOk = MDNS.begin(host.c_str());
  if (g_mdnsOk) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("apple-midi", "udp", DEFAULT_CONTROL_PORT);
  }
}
 
void setup() {
  g_bootMs = millis();
 
  Serial.begin(115200);
  Serial.println();
  Serial.println("NodeMTCBridge booting...");
 
  // DIN MIDI OUT (TX-only) on GPIO2/D4
  Serial1.begin(MIDI_BAUD);
 
  loadConfig();
 
  WiFi.mode(WIFI_STA);
 
  // Auto-connect; if it can't connect, it starts AP portal.
  WiFiManager wm;
  wm.setSaveConfigCallback(onSaveConfig);
 
  WiFiManagerParameter p_name("devname", "Device name", g_deviceName, DEVICE_NAME_MAX);
  wm.addParameter(&p_name);
 
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(0); // requirement: portal should appear if no known Wi-Fi
 
  const String apName = String("NodeMTC-Setup-") + String(ESP.getChipId(), HEX);
  bool ok = wm.autoConnect(apName.c_str());
 
  const char* newName = p_name.getValue();
  if (newName && strlen(newName)) {
    strlcpy(g_deviceName, newName, sizeof(g_deviceName));
  }
  if (ok) saveConfigIfNeeded(g_deviceName);
 
  startMdns();
 
  // AppleMIDI + MIDI callbacks
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleTimeCodeQuarterFrame(onMTCQuarterFrame);
  AppleMIDI.setHandleConnected(onAppleMidiConnected);
  AppleMIDI.setHandleDisconnected(onAppleMidiDisconnected);
 
  // Web UI
  g_server.on("/", handleRoot);
  g_server.on("/api/status", handleStatus);
  g_server.on("/wifi", HTTP_GET, handleWifi);
  g_server.on("/api/reboot", HTTP_POST, handleReboot);
  g_server.on("/api/wifi_reset", HTTP_POST, handleWifiReset);
  g_server.onNotFound([]() { g_server.send(404, "text/plain", "Not found"); });
  g_server.begin();
 
  Serial.print("Wi-Fi: ");
  Serial.print(WiFi.isConnected() ? "connected" : "not connected");
  Serial.print(" SSID=");
  Serial.print(WiFi.SSID());
  Serial.print(" IP=");
  Serial.println(WiFi.localIP());
}
 
void loop() {
  g_server.handleClient();
  MIDI.read();  // drives AppleMIDI parsing + invokes callbacks
  MDNS.update();
  yield();
}
