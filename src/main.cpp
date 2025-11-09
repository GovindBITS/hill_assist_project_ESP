#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <strings.h>

namespace {

#if defined(LED_BUILTIN)
constexpr uint8_t kStatusLedPin = LED_BUILTIN;
#else
constexpr uint8_t kStatusLedPin = 2;  // Most ESP32 dev boards
#endif

constexpr uint8_t kStmRxPin = 16;  // ESP32 RX2 (connect to STM TX)
constexpr uint8_t kStmTxPin = 17;  // ESP32 TX2 (connect to STM RX)
constexpr uint32_t kStmBaudRate = 115200;
constexpr uint32_t kHeartbeatIntervalMs = 1000;
constexpr uint32_t kLedPulseMs = 80;

constexpr const char* kWifiSsid = "HillAssist-ESP32";
constexpr const char* kWifiPassword = "hillassist";

HardwareSerial& stmSerial = Serial2;
WebServer server(80);

uint32_t gLastHeartbeat = 0;
uint32_t gLastLedPulse = 0;
bool gLedState = false;

IPAddress gApIp;

struct LineBuffer {
  char data[160];
  size_t length = 0;

  void clear() { length = 0; }

  void append(char c) {
    if (length + 1 >= sizeof(data)) {
      data[sizeof(data) - 2] = '\0';
      length = sizeof(data) - 1;
      return;
    }
    data[length++] = c;
  }

  bool empty() const { return length == 0; }

  const char* c_str() {
    if (length >= sizeof(data)) {
      length = sizeof(data) - 1;
    }
    data[length] = '\0';
    return data;
  }
};

LineBuffer gCliBuffer{};
LineBuffer gStmBuffer{};

String gLastCommand;
String gLastAck;
String gLastStmLine;

const char kIndexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Hill Assist Controller</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; margin: 16px; background:#1f2933; color:#f5f7fa; }
    h1 { font-size: 1.4rem; margin-bottom: 0.5rem; }
    .card { background:#323f4b; border-radius: 12px; padding: 16px; margin-bottom:16px; box-shadow:0 4px 12px rgba(0,0,0,0.3); }
    button { margin: 6px; padding: 10px 18px; font-size:0.95rem; border:none; border-radius:8px; background:#3f83f8; color:#fff; cursor:pointer; }
    button:active { transform:scale(0.97); }
    button.stop { background:#e12d39; }
    button.cmd { background:#0e9f6e; }
    label { display:block; margin:12px 0 4px; font-weight:bold; }
    input[type=range] { width:100%; }
    pre { background:#1a202c; padding:12px; border-radius:8px; max-height:180px; overflow:auto; }
    .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:8px; }
    .status-line { margin:6px 0; font-family:monospace; }
  </style>
</head>
<body>
  <h1>Hill Assist ESP32 Control</h1>
  <div class="card">
    <label for="speed">Drive speed: <span id="speedLabel">180</span></label>
    <input type="range" id="speed" min="0" max="255" value="180" oninput="speedLabel.textContent=value">
    <div class="grid">
      <button onclick="sendMove('FWD')">Forward</button>
      <button onclick="sendMove('BACK')">Backward</button>
      <button onclick="sendMove('LEFT')">Left</button>
      <button onclick="sendMove('RIGHT')">Right</button>
      <button class="stop" onclick="sendRaw('STOP')">Stop</button>
      <button class="cmd" onclick="sendRaw('/ping')">Ping</button>
      <button class="cmd" onclick="sendRaw('/reset')">Reset STM</button>
    </div>
  </div>

  <div class="card">
    <label for="raw">Custom command</label>
    <input id="raw" type="text" placeholder="MOTORS 120 -80">
    <button onclick="sendRaw(document.getElementById('raw').value)">Send</button>
  </div>

  <div class="card">
    <h2>Status</h2>
    <div class="status-line">Uptime: <span id="uptime">-</span> ms</div>
    <div class="status-line">Last Command: <span id="lastCmd">-</span></div>
    <div class="status-line">Last Ack: <span id="lastAck">-</span></div>
    <div class="status-line">Last STM: <span id="lastStm">-</span></div>
    <div class="status-line">SoftAP IP: <span id="apIp">-</span></div>
    <pre id="log"></pre>
  </div>

  <script>
    const logEl = document.getElementById('log');
    function appendLog(line) {
      if (!line) return;
      const now = new Date().toLocaleTimeString();
      logEl.textContent = `[${now}] ${line}\n` + logEl.textContent;
    }
    function sendMove(dir) {
      const speed = document.getElementById('speed').value;
      sendRaw(`${dir} ${speed}`);
    }
    function sendRaw(cmd) {
      if (!cmd) return;
      fetch(`/cmd?command=${encodeURIComponent(cmd)}`)
        .then(resp => resp.text())
        .then(text => appendLog(text))
        .catch(err => appendLog(`error: ${err}`));
    }
    function refreshStatus() {
      fetch('/status')
        .then(resp => resp.json())
        .then(data => {
          document.getElementById('uptime').textContent = data.uptime || '-';
          document.getElementById('lastCmd').textContent = data.lastCommand || '-';
          document.getElementById('lastAck').textContent = data.lastAck || '-';
          document.getElementById('lastStm').textContent = data.lastStm || '-';
          document.getElementById('apIp').textContent = data.apIp || '-';
        })
        .catch(() => {});
    }
    setInterval(refreshStatus, 1500);
    refreshStatus();
  </script>
</body>
</html>
)rawliteral";

void pulseLed(uint32_t nowMs) {
  if (gLedState && (nowMs - gLastLedPulse >= kLedPulseMs)) {
    digitalWrite(kStatusLedPin, LOW);
    gLedState = false;
  }
}

void kickLed(uint32_t nowMs) {
  digitalWrite(kStatusLedPin, HIGH);
  gLedState = true;
  gLastLedPulse = nowMs;
}

void sendHeartbeat(uint32_t nowMs) {
  if (nowMs - gLastHeartbeat < kHeartbeatIntervalMs) {
    return;
  }
  gLastHeartbeat = nowMs;
  stmSerial.print(F("ESP32_HEARTBEAT "));
  stmSerial.println(nowMs);
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 4);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in.charAt(i);
    switch (c) {
      case '\\':
      case '\"':
        out += '\\';
        out += c;
        break;
      case '\r':
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

String processCommand(const String& rawCmd, bool fromHttp) {
  String cmd = rawCmd;
  cmd.trim();
  if (cmd.isEmpty()) {
    return F("No command supplied");
  }

  gLastCommand = cmd;

  const uint32_t now = millis();

  if (cmd.startsWith("/")) {
    if (cmd.equalsIgnoreCase("/?") || cmd.equalsIgnoreCase("/help")) {
      const char helpMsg[] =
          "Commands:\n"
          "  /? or /help      - show this help\n"
          "  /ping            - send ESP32_PING to STM32\n"
          "  /reset           - send ESP32_RESET to STM32\n"
          "  FWD [speed]      - drive forward (0-255)\n"
          "  BACK [speed]     - drive backward\n"
          "  LEFT [speed]     - pivot left\n"
          "  RIGHT [speed]    - pivot right\n"
          "  STOP             - stop motors\n"
          "  MOTORS L R       - raw motor command (-255..255)\n";
      gLastAck = F("Help requested");
      return String(helpMsg);
    }
    if (cmd.equalsIgnoreCase("/ping")) {
      stmSerial.println(F("ESP32_PING"));
      kickLed(now);
      gLastAck = F("[esp->stm] ESP32_PING");
      return gLastAck;
    }
    if (cmd.equalsIgnoreCase("/reset")) {
      stmSerial.println(F("ESP32_RESET"));
      kickLed(now);
      gLastAck = F("[esp->stm] ESP32_RESET");
      return gLastAck;
    }
    gLastAck = F("[warn] Unknown slash-command");
    return gLastAck;
  }

  stmSerial.println(cmd);
  kickLed(now);
  gLastAck = "[esp->stm] " + cmd;
  return gLastAck;
}

void handleRoot() {
  server.send_P(200, "text/html", kIndexHtml);
}

void handleCmd() {
  if (!server.hasArg("command")) {
    server.send(400, "text/plain", "Missing command parameter");
    return;
  }
  String cmd = server.arg("command");
  String result = processCommand(cmd, true);
  server.send(200, "text/plain", result);
}

void handleStatus() {
  String json = "{";
  json += "\"uptime\":" + String(millis());
  json += ",\"lastCommand\":\"" + jsonEscape(gLastCommand) + "\"";
  json += ",\"lastAck\":\"" + jsonEscape(gLastAck) + "\"";
  json += ",\"lastStm\":\"" + jsonEscape(gLastStmLine) + "\"";
  json += ",\"apIp\":\"" + gApIp.toString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

void handleCliInput() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!gCliBuffer.empty()) {
        const char* line = gCliBuffer.c_str();
        String result = processCommand(String(line), false);
        Serial.println(result);
        gCliBuffer.clear();
      }
      continue;
    }
    gCliBuffer.append(c);
  }
}

void handleStmInput() {
  while (stmSerial.available() > 0) {
    char c = static_cast<char>(stmSerial.read());
    const uint32_t now = millis();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!gStmBuffer.empty()) {
        const char* line = gStmBuffer.c_str();
        gLastStmLine = line;
        Serial.print(F("[stm] "));
        Serial.println(line);
        gStmBuffer.clear();
        kickLed(now);
      }
      continue;
    }
    gStmBuffer.append(c);
  }
}

}  // namespace

void setup() {
  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, LOW);

  Serial.begin(115200);
  const uint32_t startWait = millis();
  while (!Serial && (millis() - startWait) < 1500) {
    delay(10);
  }

  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(kWifiSsid, kWifiPassword)) {
    gApIp = WiFi.softAPIP();
    Serial.print(F("[wifi] AP SSID: "));
    Serial.print(kWifiSsid);
    Serial.print(F("  IP: "));
    Serial.println(gApIp);
  } else {
    Serial.println(F("[wifi] Failed to start SoftAP"));
    gApIp = IPAddress(192, 168, 4, 1);
  }

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  stmSerial.begin(kStmBaudRate, SERIAL_8N1, kStmRxPin, kStmTxPin);

  Serial.println();
  Serial.println(F("=== ESP32 <-> STM32 bridge ==="));
  Serial.print(F("STM UART @ "));
  Serial.print(kStmBaudRate);
  Serial.print(F(" bps (RX="));
  Serial.print(kStmRxPin);
  Serial.print(F(" TX="));
  Serial.print(kStmTxPin);
  Serial.println(')');
  Serial.print(F("Connect to http://"));
  Serial.print(gApIp);
  Serial.println(F(" (Wi-Fi SSID 'HillAssist-ESP32', password 'hillassist')."));
  Serial.println(F("USB serial also accepts commands (FWD/BACK/LEFT/RIGHT, STOP, /ping, /reset etc.)"));

  stmSerial.println(F("ESP32_READY"));
  gLastHeartbeat = millis();
}

void loop() {
  const uint32_t now = millis();
  handleCliInput();
  handleStmInput();
  server.handleClient();
  sendHeartbeat(now);
  pulseLed(now);
}
