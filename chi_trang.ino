#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include <Arduino.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>

#include <math.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>


#define HOLD_TIME_MS 1000  // Giữ nút 1s để reset EEPROM

// Time
static uint64_t nowUnixMs();

// MQTT
static void setupMQTT();
static bool mqttConnectOnce();
static void reconnectMQTT();
static void handleMQTTCommand(DynamicJsonDocument &doc);


// Publishers
static void publishDeviceStatus();
static void publishTemperature();
static void publishOutputs(bool force = false);
static void publishAllOnce();
static bool publishLogged(const String &topic, const String &payload, bool retained = false);


// Subsystems
static void startWiFi();
static void startWeb();
static void startOTA();



#define DHTTYPE DHT22  // Hoặc DHT11 nếu dùng DHT11
// ===================== CONFIG / CONSTANTS =====================
// Defaults (overridden via Web UI and saved in NVS)
static const char *WIFI_SSID_DEFAULT = "YOUR_SSID";
static const char *WIFI_PASS_DEFAULT = "YOUR_PASSWORD";
static const char *AP_SSID = "TRANG_ESP";
static const char *AP_PASS = "12345678";
static const char *MDNS_HOST = "trang";

// NTP: GMT+7 (VN)
static const long TZ_OFFSET = 7 * 3600;  // seconds
static const int TZ_DST = 0;

Preferences prefsNet;  // "net": SSID/PASS

Preferences prefsMqtt;  // "mqtt": host/port/user/pass/device

Preferences prefsMode;  // "mode": auto/manual

WebServer server(80);
WiFiClientSecure espClient;
PubSubClient client(espClient);

// MQTT config (TLS via port 8883 by default)
struct MqttCfg {
  String host;
  uint16_t port = 8883;
  String user;
  String pass;
  String device;
} mqttCfg;



// ===================== PINS =====================
static const int RESET_BUTTON = 0;
static const int LED = 2;
static const int BT1 = 13, BT2 = 14;
static const int OUT1 = 32, OUT2 = 33;
static const int DHT22_pin = 27;



DHT dht(DHT22_pin, DHTTYPE);

float g_tmax = 80.0f;                // °C default
float g_t1 = NAN, g_humidity = NAN;  // °C

float g_t_protec = 30.0;

bool g_mode = false;
float g_hyst = 0.5f;  // hysteresis °C để tránh nhấp nháy

static unsigned long lastMqttTry = 0;

static void loadMqttCfg() {
  prefsMqtt.begin("mqtt", true);
  mqttCfg.host = prefsMqtt.getString("host", "");
  mqttCfg.port = prefsMqtt.getUShort("port", 8883);
  mqttCfg.user = prefsMqtt.getString("user", "");
  mqttCfg.pass = prefsMqtt.getString("pass", "");
  mqttCfg.device = prefsMqtt.getString("device", MDNS_HOST);
  prefsMqtt.end();
  if (mqttCfg.device.isEmpty()) mqttCfg.device = MDNS_HOST;
}
static void saveMqttCfg() {
  prefsMqtt.begin("mqtt", false);
  prefsMqtt.putString("host", mqttCfg.host);
  prefsMqtt.putUShort("port", mqttCfg.port);
  prefsMqtt.putString("user", mqttCfg.user);
  prefsMqtt.putString("pass", mqttCfg.pass);
  prefsMqtt.putString("device", mqttCfg.device);
  prefsMqtt.end();
}

static void saveModeCfg() {
  prefsMode.begin("mode", false);
  prefsMode.putBool("control", g_mode);

  prefsMode.putFloat("threshold", g_tmax);
  prefsMode.putFloat("hyst", g_hyst);
  prefsMode.end();
}

static void loadModeCfg() {
  prefsMode.begin("mode", true);
  bool mode = prefsMode.getBool("control", false);
  prefsMode.end();
  if (isfinite(mode)) g_mode = mode;
}

// Topics root (NO device prefix to match app spec exactly)
static const char *T_ROOT = "esp32";
static String tStatus, tCmd, tTemp, thumidity, tOut;
static void buildTopics() {
  String r = T_ROOT;
  tStatus = r + "/status";
  tCmd = r + "/commands";
  tTemp = r + "/temperature";
  thumidity = r + "/humidity";
  tOut = r + "/outputs";
}


void resetESP() {
  Serial.println("Đang xóa cấu hình ...");

  prefsNet.begin("net", false);
  prefsNet.clear();  // "net": SSID/PASS
  prefsNet.end();


  prefsMqtt.begin("mqtt", false);
  prefsMqtt.clear();  // "mqtt": host/port/user/pass/device
  prefsMqtt.end();

  prefsMode.begin("mode", false);
  prefsMode.clear();  // "mqtt": host/port/user/pass/device
  prefsMode.end();



  delay(1000);
  ESP.restart();
}




// ===================== Time =====================
static uint64_t nowUnixMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return ((uint64_t)tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
}

// ===================== MQTT =====================
static void setupMQTT() {
  espClient.setInsecure();  // For quick test; setCACert() in production
  client.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  client.setCallback([](char *topic, byte *payload, unsigned int length) {
    String t = String(topic);
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.printf("MQTT <- [%s] %s\n", topic, msg.c_str());

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      Serial.printf("JSON parse err: %s\n", err.c_str());
      return;
    }
    if (t == tCmd) handleMQTTCommand(doc);
  });
  client.setBufferSize(4096);
  client.setKeepAlive(30);
}


static bool publishLogged(const String &topic, const String &payload, bool retained) {
  bool ok = client.publish(topic.c_str(), payload.c_str(), retained);
  if (!ok) Serial.printf("MQTT publish failed -> %s (len=%d, retained=%d, state=%d)\n", topic.c_str(), payload.length(), (int)retained, client.state());
  return ok;
}

static bool mqttConnectOnce() {
  if (mqttCfg.host.isEmpty()) return false;
  String lwt = "{\"status\":\"offline\"}";
  String clientId = mqttCfg.device + String("-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = client.connect(clientId.c_str(),
                           mqttCfg.user.length() ? mqttCfg.user.c_str() : nullptr,
                           mqttCfg.pass.length() ? mqttCfg.pass.c_str() : nullptr,
                           tStatus.c_str(), 1, true, lwt.c_str());
  if (ok) {
    client.subscribe(tCmd.c_str(), 1);
    publishAllOnce();
    publishOutputs(true);  // <-- thêm dòng này
    Serial.printf("MQTT connected, subs: %s\n", tCmd.c_str());
  }
  return ok;
}

static int outPinFromCh(uint8_t ch) {
  switch (ch) {
    case 1: return OUT1;
    case 2: return OUT2;
    default: return -1;
  }
}

static void setOut(uint8_t ch, bool on) {
  int pin = outPinFromCh(ch);
  if (pin < 0) return;
  bool prev = (digitalRead(pin) == HIGH);
  if (prev == on) return;  // no change -> no publish
  digitalWrite(pin, on ? HIGH : LOW);


  // publish immediately on any change so the app syncs right away
  publishOutputs(true);
}

static bool getOut(uint8_t ch) {
  int pin = outPinFromCh(ch);
  if (pin >= 0) return (digitalRead(pin) == HIGH);
  return false;
}

static void reconnectMQTT() {
  if (client.connected()) return;
  if (millis() - lastMqttTry < 3000) return;
  lastMqttTry = millis();
  Serial.print("MQTT connecting... ");
  if (mqttConnectOnce()) Serial.println("OK");
  else Serial.printf("fail rc=%d\n", client.state());
}

static void publishDeviceStatus() {
  if (!client.connected()) return;
  DynamicJsonDocument d(256);
  d["device"] = mqttCfg.device;
  d["status"] = "online";
  d["uptime"] = (uint32_t)(millis() / 1000);
  d["wifi_rssi"] = (int)WiFi.RSSI();
  d["free_heap"] = (uint32_t)ESP.getFreeHeap();
  d["timestamp"] = nowUnixMs();
  String s;
  serializeJson(d, s);
  publishLogged(tStatus, s, true);
}


static void publishTemperature() {
  if (!client.connected()) return;
  DynamicJsonDocument t(192);
  if (isfinite(g_t1)) t["Temperature"] = g_t1;
  else t["temperature"] = nullptr;
  if (isfinite(g_humidity)) t["Humidity"] = g_humidity;
  else t["humidity"] = nullptr;
  t["maxTemp"] = g_tmax;
  t["autoMode"] = g_mode;  // tiện cho UI hiển thị
  t["unit"] = "celsius";
  t["timestamp"] = nowUnixMs();
  String s;
  serializeJson(t, s);
  publishLogged(tTemp, s, true);
}


static void publishOutputs(bool force) {
  static bool last1 = false, last2 = false;
  bool c1 = getOut(1), c2 = getOut(2);
  bool changed = (c1 != last1) || (c2 != last2);
  if (!force && !changed) return;
  if (!client.connected()) return;
  DynamicJsonDocument o(256);
  JsonObject oo = o.createNestedObject("outputs");
  oo["out1"] = c1;
  oo["out2"] = c2;

  o["timestamp"] = nowUnixMs();
  String s;
  serializeJson(o, s);
  publishLogged(tOut, s, true);
  last1 = c1;
  last2 = c2;
}

static void publishAllOnce() {
  publishDeviceStatus();

  publishTemperature();
}


// ===================== Commands =====================
static void handleMQTTCommand(DynamicJsonDocument &doc) {
  const char *command = doc["command"] | "";
  if (!strcmp(command, "setOutput")) {
    int output = doc["value"]["output"] | 0;
    bool state = doc["value"]["state"] | false;
    if (output >= 1 && output <= 2) {
      setOut((uint8_t)output, state);  // 
    }
  } else if (!strcmp(command, "getStatus")) {
    publishAllOnce();
  } else if (!strcmp(command, "restart")) {
    Serial.println("Restart requested via MQTT");
    delay(500);
    ESP.restart();
  } else if (!strcmp(command, "setMode")) {
    // value: { auto: boolean, threshold?: number, hysteresis?: number }
    g_mode = doc["value"]["auto"] | false;

    if (doc["value"].containsKey("threshold")) {
      g_tmax = doc["value"]["threshold"];
    }
    if (doc["value"].containsKey("hysteresis")) {
      g_hyst = doc["value"]["hysteresis"];
    }
    Serial.print("mode");
    Serial.println(g_mode);
    saveModeCfg();

    // Phản hồi nhanh trạng thái mới cho app
    publishTemperature();  // để app thấy maxTemp/autoMode mới
    publishOutputs(true);
  }
}


struct Debounce {
  bool state = false;
  bool lastRead = false;
  uint32_t lastChange = 0;
} db1, db2, db3, db4;
static bool debounceRead(int pin, Debounce &db, uint16_t ms = 20) {
  bool r = (digitalRead(pin) == LOW);
  if (r != db.lastRead) {
    db.lastRead = r;
    db.lastChange = millis();
  }
  if (millis() - db.lastChange > ms) { db.state = db.lastRead; }
  return db.state;
}
static bool edgePress(bool now, bool &prev) {
  bool rising = (now && !prev);
  prev = now;
  return rising;
}

void mode_main() {
  // --- AUTO CONTROL: OUT1 theo ngưỡng ---
  if (g_mode && isfinite(g_t1)) {
    bool curOn = getOut(1);
    // Bật nếu >= ngưỡng, tắt khi <= ngưỡng - hysteresis
    if (g_t1 >= g_tmax && !curOn) {
      setOut(1, true);
    } else if (g_t1 <= (g_tmax - g_hyst) && curOn) {
      setOut(1, false);
    }
  }
}

void dht_main() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 2000) return;  // Đọc tối đa 0.5 Hz
  lastMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();  // °C

  if (isfinite(h)) g_humidity = h;
  if (isfinite(t)) g_t1 = t;
}
// ===================== Web UI (minimal) =====================
static const char WIFI_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi‑Fi Setup</title>
<style>
  :root{
    --bg:#0b1020; --card:#111726; --muted:#9aa4b2; --text:#e6eaf0; --accent:#4f8cff; --border:#233047; --ok:#22c55e; --warn:#f59e0b; --err:#ef4444;
  }
  *{box-sizing:border-box}
  body{margin:0;background:linear-gradient(180deg,#0b1020 0%,#0e1424 100%);font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;color:var(--text)}
  .wrap{max-width:980px;margin:24px auto;padding:0 16px}
  .nav{position:sticky;top:0;z-index:2;background:rgba(10,15,30,.8);backdrop-filter:blur(8px);border:1px solid var(--border);border-radius:14px;display:flex;gap:8px;align-items:center;padding:10px;margin-bottom:16px}
  .nav a{color:var(--text);text-decoration:none;padding:8px 12px;border:1px solid var(--border);border-radius:10px;opacity:.85}
  .nav a:hover{opacity:1}
  .nav a.active{background:var(--accent);border-color:transparent}
  .grid{display:grid;grid-template-columns:1fr;gap:16px}
  @media(min-width:860px){.grid{grid-template-columns:1.2fr .8fr}}
  .card{background:linear-gradient(180deg,#0f1629 0%,#101a32 100%);border:1px solid var(--border);border-radius:16px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}
  h2{margin:6px 0 12px;font-weight:700;letter-spacing:.2px}
  label{display:block;margin:10px 0 6px;color:var(--muted)}
  input,button,select{width:100%;padding:10px 12px;border-radius:12px;border:1px solid var(--border);background:#0b1326;color:var(--text)}
  button{cursor:pointer;transition:.15s transform ease, .15s opacity ease}
  button:hover{transform:translateY(-1px)}
  .row{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
  .btn{background:var(--accent);border-color:transparent}
  .btn.secondary{background:#0b1326;border-color:var(--border)}
  .muted{color:var(--muted);font-size:13px}
  table{width:100%;border-collapse:collapse;margin-top:8px}
  th,td{padding:10px;border-bottom:1px solid var(--border);text-align:left}
  .ssid{cursor:pointer}
  .badge{font-size:12px;padding:2px 8px;border-radius:999px;border:1px solid var(--border);background:#0b1326;color:var(--muted)}
  .bars{font-family:monospace;letter-spacing:1px}
  .pill{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border:1px solid var(--border);border-radius:999px;background:#0b1326}
</style>
</head>
<body>
  <div class="wrap">
    <nav class="nav">
     <a href="/">Home</a>
      <a href="/wifi" class="active">Wi‑Fi Setup</a>
      <a href="/mqtt">MQTT Setup</a>
      <a href="/ota">OTA Upload</a>
      <span style="margin-left:auto" class="pill"><span id="ip">—</span><span class="badge" id="mdns">mDNS</span></span>
    </nav>

    <div class="grid">
      <section class="card">
        <h2>Connect to Wi‑Fi</h2>
        <form id="wf" method="post" action="/wifi/save">
          <label>SSID</label>
          <div class="row" style="gap:8px">
            <input id="ssid" name="ssid" placeholder="Network name" required style="flex:1">
            <button type="button" id="scan" class="btn secondary" style="flex:0 0 160px">Scan Networks</button>
          </div>
          <label>Password</label>
          <div class="row" style="gap:8px">
            <input id="pass" name="pass" type="password" placeholder="Password" style="flex:1">
            <button type="button" id="togglePass" class="secondary" style="flex:0 0 140px">Show</button>
          </div>
          <div class="row">
            <button type="submit" class="btn">Save & Reboot</button>
            <button type="button" id="forget" class="secondary">Forget Wi‑Fi</button>
          </div>
          <p class="muted">Tip: Chỉ cần chọn một SSID bên bảng "Nearby Networks" để điền nhanh.</p>
        </form>
      </section>

      <aside class="card">
        <h2>Nearby Networks</h2>
        <div id="scanStatus" class="muted">Nhấn "Scan Networks" để quét.</div>
        <div id="scanWrap" style="max-height:320px;overflow:auto"></div>
      </aside>
    </div>
  </div>

<script>
  // Basic info (best effort; mDNS/IP sẽ được firmware điền bên server index nếu muốn)
  document.getElementById('ip').textContent = location.host || '—';

  const ssid = document.getElementById('ssid');
  const pass = document.getElementById('pass');
  const togglePass = document.getElementById('togglePass');
  const scanBtn = document.getElementById('scan');
  const scanWrap = document.getElementById('scanWrap');
  const scanStatus = document.getElementById('scanStatus');
  const forgetBtn = document.getElementById('forget');

  togglePass.addEventListener('click', ()=>{
    const t = pass.getAttribute('type') === 'password' ? 'text' : 'password';
    pass.setAttribute('type', t);
    togglePass.textContent = (t==='text')? 'Hide' : 'Show';
  });

  function rssiToBars(rssi){
    if (rssi >= -50) return '▮▮▮▮';
    if (rssi >= -60) return '▮▮▮▯';
    if (rssi >= -70) return '▮▮▯▯';
    if (rssi >= -80) return '▮▯▯▯';
    return '▯▯▯▯';
  }

  async function doScan(){
    scanBtn.disabled = true; scanBtn.textContent = 'Scanning...';
    scanStatus.textContent = 'Đang quét...';
    scanWrap.innerHTML = '';
    try{
      const r = await fetch('/wifi/scan',{cache:'no-cache'});
      const arr = await r.json();
      if (!Array.isArray(arr) || arr.length===0){
        scanStatus.textContent = 'Không tìm thấy mạng.'; return;
      }
      scanStatus.textContent = `Tìm thấy ${arr.length} mạng:`;
      const table = document.createElement('table');
      table.innerHTML = '<thead><tr><th>SSID</th><th class="muted">RSSI</th><th class="muted">Bars</th></tr></thead>';
      const tb = document.createElement('tbody');
      arr.sort((a,b)=> (b.rssi||-999) - (a.rssi||-999));
      arr.forEach(x=>{
        const tr = document.createElement('tr'); tr.className='ssid';
        tr.innerHTML = `<td>${x.ssid || ''}</td><td>${x.rssi ?? ''}</td><td class="bars">${rssiToBars(x.rssi ?? -100)}</td>`;
        tr.addEventListener('click', ()=>{ ssid.value = x.ssid || ''; ssid.focus(); });
        tb.appendChild(tr);
      });
      table.appendChild(tb); scanWrap.appendChild(table);
    }catch(e){
      scanStatus.textContent = 'Scan lỗi';
    }finally{
      scanBtn.disabled = false; scanBtn.textContent = 'Scan Networks';
    }
  }
  scanBtn.addEventListener('click', doScan);
  forgetBtn.addEventListener('click', async ()=>{
    if(!confirm('Xoá cấu hình Wi‑Fi và khởi động lại?')) return;
    try{
      const r = await fetch('/wifi/forget',{method:'POST'});
      const txt = await r.text();
      alert(txt);
    }catch(e){
      alert('Lỗi xoá Wi‑Fi');
    }
  });
</script>
</body>
</html>
)HTML";

static const char MQTT_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MQTT Setup</title>
<style>
  :root{--bg:#0b1020;--card:#111726;--muted:#9aa4b2;--text:#e6eaf0;--accent:#4f8cff;--border:#233047}
  *{box-sizing:border-box}
  body{margin:0;background:linear-gradient(180deg,#0b1020 0%,#0e1424 100%);font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;color:var(--text)}
  .wrap{max-width:760px;margin:24px auto;padding:0 16px}
  .nav{position:sticky;top:0;background:rgba(10,15,30,.8);backdrop-filter:blur(8px);border:1px solid var(--border);border-radius:14px;display:flex;gap:8px;align-items:center;padding:10px;margin-bottom:16px}
  .nav a{color:var(--text);text-decoration:none;padding:8px 12px;border:1px solid var(--border);border-radius:10px;opacity:.85}
  .nav a:hover{opacity:1}
  .nav a.active{background:var(--accent);border-color:transparent}
  .card{background:linear-gradient(180deg,#0f1629 0%,#101a32 100%);border:1px solid var(--border);border-radius:16px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}
  h2{margin:6px 0 12px;font-weight:700;letter-spacing:.2px}
  label{display:block;margin:10px 0 6px;color:var(--muted)}
  input,button{width:100%;padding:10px 12px;border-radius:12px;border:1px solid var(--border);background:#0b1326;color:var(--text)}
  button{cursor:pointer}
  .row{display:grid;grid-template-columns:1fr;gap:10px}
  @media(min-width:640px){.row{grid-template-columns:1fr 1fr}}
  .btn{background:var(--accent);border-color:transparent}
  .muted{color:var(--muted);font-size:13px}
  code{background:#0b1326;border:1px solid var(--border);padding:2px 6px;border-radius:8px}
</style>
</head>
<body>
  <div class="wrap">
    <nav class="nav">
      <a href="/">Home</a>
      <a href="/wifi">Wi‑Fi Setup</a>
      <a href="/mqtt" class="active">MQTT Setup</a>
      <a href="/ota">OTA Upload</a>
      
    </nav>

    <section class="card">
      <h2>MQTT Broker</h2>
      <form id="mf" method="post" action="/mqtt/save">
        <label>Broker Host</label>
        <input name="host" id="host" required placeholder="example.s1.eu.hivemq.cloud">
        <div class="row">
          <div>
            <label>Port</label>
            <input name="port" id="port" type="number" min="1" max="65535" value="8883" required>
          </div>
          <div>
            <label>Device Id (payload only)</label>
            <input name="device" id="device" required placeholder="esp32meter-001">
          </div>
        </div>
        <div class="row">
          <div>
            <label>Username</label>
            <input name="user" id="user" placeholder="hivemq username">
          </div>
          <div>
            <label>Password</label>
            <input name="pass" id="pass" type="password" placeholder="••••••••">
          </div>
        </div>
        <p class="muted">Dùng <code>8883</code> cho TLS TCP (không phải 8884/WSS). Topics: <code>esp32/status</code>, <code>esp32/commands</code>, <code>esp32/temperature</code>, <code>esp32/outputs</code></p>
        <p><button type="submit" class="btn">Save & Reconnect</button></p>
      </form>
    </section>
  </div>
<script>
(async()=>{
  try{
    const r = await fetch('/mqtt.json',{cache:'no-cache'});
    const j = await r.json();
    host.value=j.host||''; port.value=j.port||8883; user.value=j.user||''; pass.value=j.pass||''; device.value=j.device||'';
  }catch(e){}
})();
</script>
</body>
</html>
)HTML";

static const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Upload</title>
<style>
  :root{--bg:#0b1020;--card:#111726;--muted:#9aa4b2;--text:#e6eaf0;--accent:#4f8cff;--border:#233047;--ok:#22c55e;--err:#ef4444}
  *{box-sizing:border-box}
  body{margin:0;background:linear-gradient(180deg,#0b1020 0%,#0e1424 100%);font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;color:var(--text)}
  .wrap{max-width:760px;margin:24px auto;padding:0 16px}
  .nav{position:sticky;top:0;background:rgba(10,15,30,.8);backdrop-filter:blur(8px);border:1px solid var(--border);border-radius:14px;display:flex;gap:8px;align-items:center;padding:10px;margin-bottom:16px}
  .nav a{color:var(--text);text-decoration:none;padding:8px 12px;border:1px solid var(--border);border-radius:10px;opacity:.85}
  .nav a:hover{opacity:1}
  .nav a.active{background:var(--accent);border-color:transparent}
  .card{background:linear-gradient(180deg,#0f1629 0%,#101a32 100%);border:1px solid var(--border);border-radius:16px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}
  h2{margin:6px 0 12px;font-weight:700;letter-spacing:.2px}
  input,button{width:100%;padding:10px 12px;border-radius:12px;border:1px solid var(--border);background:#0b1326;color:var(--text)}
  button{cursor:pointer}
  .bar{height:10px;border-radius:999px;background:#0b1326;border:1px solid var(--border);overflow:hidden;margin-top:10px}
  .bar>i{display:block;height:100%;width:0%;background:var(--accent);transition:width .15s ease}
  .muted{color:var(--muted);font-size:13px}
  .ok{color:var(--ok)}.err{color:var(--err)}
</style>
</head>
<body>
  <div class="wrap">
    <nav class="nav">
      <a href="/">Home</a>
      <a href="/wifi">Wi‑Fi Setup</a>
      <a href="/mqtt">MQTT Setup</a>
      <a href="/ota" class="active">OTA Upload</a>
    </nav>

    <section class="card">
      <h2>OTA Upload (.bin)</h2>
      <form id="f" method="POST" action="/update" enctype="multipart/form-data">
        <input type="file" name="firmware" id="fw" accept=".bin" required>
        <button type="submit" id="btn">Upload</button>
      </form>
      <div class="bar"><i id="p"></i></div>
      <pre id="log" class="muted"></pre>
      <p class="muted">Sau khi upload thành công, thiết bị sẽ tự khởi động lại.</p>
    </section>
  </div>
<script>
const f=document.getElementById('f');
const log=document.getElementById('log');
const bar=document.getElementById('p');
const btn=document.getElementById('btn');

f.addEventListener('submit', function(e){
  e.preventDefault();
  const file = document.getElementById('fw').files[0];
  if(!file){ log.textContent='Chưa chọn file .bin'; return; }
  log.textContent='Uploading...'; btn.disabled=true; bar.style.width='0%';

  const xhr = new XMLHttpRequest();
  xhr.open('POST','/update',true);
  xhr.upload.onprogress = (ev)=>{
    if(ev.lengthComputable){
      const pct = Math.round((ev.loaded/ev.total)*100);
      bar.style.width = pct + '%';
    }
  };
  xhr.onreadystatechange = ()=>{
    if(xhr.readyState===4){
      btn.disabled=false;
      if(xhr.status===200){ log.textContent=xhr.responseText; bar.style.width='100%'; }
      else{ log.textContent='Upload error ('+xhr.status+')'; }
    }
  };
  const form = new FormData();
  form.append('firmware', file);
  xhr.send(form);
});
</script>
</body>
</html>
)HTML";

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Energy Monitor</title>
<style>
  :root{--bg:#0b1020;--card:#111726;--muted:#9aa4b2;--text:#e6eaf0;--accent:#4f8cff;--border:#233047;--ok:#22c55e;--warn:#f59e0b;--err:#ef4444}
  *{box-sizing:border-box}
  body{margin:0;background:linear-gradient(180deg,#0b1020 0%,#0e1424 100%);font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;color:var(--text)}
  .wrap{max-width:980px;margin:24px auto;padding:0 16px}
  .nav{position:sticky;top:0;background:rgba(10,15,30,.8);backdrop-filter:blur(8px);border:1px solid var(--border);border-radius:14px;display:flex;gap:8px;align-items:center;padding:10px;margin-bottom:16px}
  .nav a{color:var(--text);text-decoration:none;padding:8px 12px;border:1px solid var(--border);border-radius:10px;opacity:.85}
  .nav a:hover{opacity:1}
  .nav a.active{background:var(--accent);border-color:transparent}
  .grid{margin-top:15px;display:grid;grid-template-columns:1fr;gap:16px}
  @media(min-width:860px){.grid{grid-template-columns:repeat(3,1fr)}}
  .card{background:linear-gradient(180deg,#0f1629 0%,#101a32 100%);border:1px solid var(--border);border-radius:16px;padding:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}
  h1{margin:6px 0 6px;font-weight:800;letter-spacing:.2px}
  p.muted{color:var(--muted)}
  a.btn{display:inline-block;margin-top:10px;padding:10px 14px;border-radius:12px;border:1px solid var(--border);text-decoration:none;color:var(--text);background:#0b1326}
  a.btn.primary{background:var(--accent);border-color:transparent}
  .status{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0 20px}
  .pill{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border:1px solid var(--border);border-radius:999px;background:#0b1326;font-size:14px}
  .ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}
</style>
</head>
<body>
  <div class="wrap">
    <nav class="nav">
      <a href="/" class="active">Home</a>
      <a href="/wifi">Wi-Fi Setup</a>
      <a href="/mqtt">MQTT Setup</a>
      <a href="/ota">OTA Upload</a>
    </nav>

    <header class="card">
      <h1>ESP32 Monitor</h1>
      <p class="muted">Quick links & status overview.</p>
      <div class="status">
        <span class="pill">Wi-Fi: <b id="wifiState">—</b></span>
        <span class="pill">IP: <b id="ip">—</b></span>
        <span class="pill">RSSI: <b id="rssi">—</b></span>
        <span class="pill">MQTT: <b id="mqttState">—</b></span>
        <span class="pill">Device: <b id="device">—</b></span>
      </div>
    
    </header>

    <section class="grid">
      <div class="card">
        <h1>Wi-Fi</h1>
        <p class="muted">Scan networks and save credentials. Forget Wi-Fi when needed.</p>
        <a class="btn" href="/wifi">Go to Wi-Fi Setup</a>
      </div>
      <div class="card">
        <h1>MQTT</h1>
        <p class="muted">Set broker (TLS 8883), account & device id. Matches app JSON topics.</p>
        <a class="btn" href="/mqtt">Go to MQTT Setup</a>
      </div>
      <div class="card">
        <h1>OTA</h1>
        <p class="muted">Upload compiled <code>.bin</code>, device will reboot automatically.</p>
        <a class="btn" href="/ota">Go to OTA Upload</a>
      </div>

      
    </section>
  </div>
<script>
(async()=>{
  try{
    const r = await fetch('/sys.json',{cache:'no-cache'});
    const j = await r.json();
    document.getElementById('wifiState').textContent = j.wifi || '—';
    document.getElementById('ip').textContent       = j.ip || '—';
    document.getElementById('rssi').textContent     = (j.rssi!==undefined? j.rssi+' dBm':'—');
    document.getElementById('mqttState').textContent= j.mqtt || '—';
    document.getElementById('device').textContent   = j.device || '—';
  }catch(e){}
})();
</script>
</body>
</html>
)HTML";


static void startWeb() {
  // server.on("/", []() {
  //   server.send(200, "text/html", "<!doctype html><meta charset='utf-8'><title>ESP32</title><h2>ESP32 Energy Monitor</h2><ul><li><a href='/wifi'>Wi-Fi Setup</a></li><li><a href='/mqtt'>MQTT Setup</a></li><li><a href='/ota'>OTA Upload</a></li></ul>");
  // });
  server.on("/", []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/sys.json", []() {
    DynamicJsonDocument j(256);
    bool wifiOK = (WiFi.status() == WL_CONNECTED);
    j["wifi"] = wifiOK ? "connected" : "ap";
    j["ip"] = wifiOK ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    j["rssi"] = wifiOK ? (int)WiFi.RSSI() : 0;
    j["mqtt"] = client.connected() ? "connected" : "disconnected";
    j["device"] = mqttCfg.device;
    String s;
    serializeJson(j, s);
    server.send(200, "application/json", s);
  });
  server.on("/wifi", []() {
    server.send_P(200, "text/html", WIFI_HTML);
  });
  server.on("/wifi/save", HTTP_POST, []() {
    String ssid = server.arg("ssid"), pass = server.arg("pass");
    prefsNet.begin("net", false);
    prefsNet.putString("ssid", ssid);
    prefsNet.putString("pass", pass);
    prefsNet.end();
    server.send(200, "text/html", "<meta charset='utf-8'>Saved. Rebooting...");
    delay(500);
    ESP.restart();
  });
  server.on("/wifi/scan", []() {
    int n = WiFi.scanNetworks(false, true);
    String j = "[";
    for (int i = 0; i < n; i++) {
      if (i) j += ",";
      j += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    j += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", j);
  });
  server.on("/wifi/forget", HTTP_POST, []() {
    prefsNet.begin("net", false);
    prefsNet.putString("ssid", "");
    prefsNet.putString("pass", "");
    prefsNet.end();
    server.send(200, "text/plain", "Đã xoá cấu hình Wi‑Fi. Thiết bị sẽ khởi động lại...");
    delay(500);
    ESP.restart();
  });

  server.on("/mqtt", []() {
    server.send_P(200, "text/html", MQTT_HTML);
  });
  server.on("/mqtt.json", []() {
    DynamicJsonDocument j(256);
    j["host"] = mqttCfg.host;
    j["port"] = mqttCfg.port;
    j["user"] = mqttCfg.user;
    j["pass"] = mqttCfg.pass;
    j["device"] = mqttCfg.device;
    String s;
    serializeJson(j, s);
    server.send(200, "application/json", s);
  });
  server.on("/mqtt/save", HTTP_POST, []() {
    mqttCfg.host = server.arg("host");
    mqttCfg.port = (uint16_t)server.arg("port").toInt();
    mqttCfg.user = server.arg("user");
    mqttCfg.pass = server.arg("pass");
    mqttCfg.device = server.arg("device");
    if (mqttCfg.host.isEmpty() || mqttCfg.port == 0 || mqttCfg.device.isEmpty()) {
      server.send(400, "text/plain", "Missing host/port/device");
      return;
    }
    saveMqttCfg();
    if (client.connected()) client.disconnect();
    client.setServer(mqttCfg.host.c_str(), mqttCfg.port);
    server.send(200, "text/plain", "MQTT saved. Reconnecting...");
  });

  server.on("/ota", []() {
    server.send_P(200, "text/html", OTA_HTML);
  });
  server.on(
    "/update", HTTP_POST, []() {
      if (Update.hasError()) server.send(200, "text/plain", "Update FAILED");
      else {
        server.send(200, "text/plain", "Update OK. Rebooting...");
        delay(500);
        ESP.restart();
      }
    },
    []() {
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        size_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSpace)) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("Update Success: %u bytes\n", up.totalSize);
        else Update.printError(Serial);
      }
    });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}




// ===================== Wi-Fi & OTA =====================
static void startWiFi() {
  prefsNet.begin("net", true);
  String ssid = prefsNet.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = prefsNet.getString("pass", WIFI_PASS_DEFAULT);
  prefsNet.end();

  WiFi.mode(WIFI_STA);
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Wi-Fi STA connecting to \"%s\"\n", ssid.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      digitalWrite(LED, 1);
      delay(200);
      digitalWrite(LED, 0);
      delay(200);
      Serial.print(".");
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_HOST)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: http://%s.local\n", MDNS_HOST);
    }
    configTime(TZ_OFFSET, TZ_DST, "pool.ntp.org", "time.nist.gov");
    digitalWrite(LED, 0);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("AP mode. SSID: %s  PASS: %s\n", AP_SSID, AP_PASS);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    digitalWrite(LED, 1);
  }
}

static void startOTA() {
  ArduinoOTA.setHostname(MDNS_HOST);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA End");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("OTA Progress: %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}




void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(OUT1, OUTPUT);
  pinMode(OUT2, OUTPUT);


  pinMode(LED, OUTPUT);
  pinMode(BT1, INPUT_PULLUP);
  pinMode(BT2, INPUT_PULLUP);

  pinMode(RESET_BUTTON, INPUT_PULLUP);

  digitalWrite(LED, 1);
  delay(1500);

  if (digitalRead(RESET_BUTTON) == LOW) {
    unsigned long t0 = millis();
    while (digitalRead(RESET_BUTTON) == LOW) {
      if (millis() - t0 > HOLD_TIME_MS) {
        resetESP();
      }
    }
  }

  dht.begin();
  startWiFi();
  loadMqttCfg();
  loadModeCfg();
  buildTopics();
  startWeb();
  startOTA();
  setupMQTT();
}

void loop() {
  // put your main code here, to run repeatedly:
  static bool p1 = false, p2 = false;
  bool b1 = debounceRead(BT1, db1);
  bool b2 = debounceRead(BT2, db2);
  if (edgePress(b1, p1)) {
    if (!g_mode) setOut(1, !getOut(1));
  }
  if (edgePress(b2, p2)) setOut(2, !getOut(2));

  dht_main();
  mode_main();
  if (WiFi.status() == WL_CONNECTED){
    reconnectMQTT();
  }

  if (client.connected()) {
    client.loop();
    static uint32_t lastTemp = 0, lastOut = 0;
    uint32_t now = millis();

    if (now - lastTemp >= 2000) {
      lastTemp = now;
      publishTemperature();
    }

    if (now - lastOut >= 2000) {
      lastOut = now;
      publishOutputs(false);
    }
  }

  server.handleClient();
  ArduinoOTA.handle();
}
