#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>

// ================= WIFI =================
const char* ssid = "Vismi";
const char* password = "111111111";

// ================= MQTT =================
// MQTT broker runs on Raspberry Pi
// If raspberrypi.local does not work, replace with Pi IP, example: "10.140.180.xxx"
const char* mqtt_server = "raspberrypi.local";
const int mqtt_port = 1883;

const char* MQTT_CLIENT_ID = "luna-motor-esp32";

const char* TOPIC_ROBOT_CMD     = "luna/robot/cmd";
const char* TOPIC_ROBOT_STATUS  = "luna/robot/status";
const char* TOPIC_ROBOT_SENSORS = "luna/robot/sensors";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WebServer server(80);

// ================= MOTOR PINS =================
#define INA1 27
#define INB1 26
#define INC1 25
#define IND1 33

#define INA2 23
#define INB2 22
#define INC2 21
#define IND2 19

// ================= PWM PINS =================
#define ENA1 14
#define ENB1 12
#define ENA2 13
#define ENB2 15

// ================= ULTRASONIC =================
#define TRIG_FRONT 18
#define ECHO_FRONT 5
#define TRIG_LEFT 17
#define ECHO_LEFT 16
#define TRIG_RIGHT 4
#define ECHO_RIGHT 32

// ================= TCRT + BUZZER =================
#define TCRT_PIN 35
#define BUZZER_PIN 2

// IMPORTANT: your web showed TCRT Edge: 0 always, so 0 = SAFE, 1 = EDGE
#define EDGE_DETECTED_VALUE 1
#define OBSTACLE_LIMIT 15

// ================= VALUES =================
int speedValue = 240;
long frontDist = 300;
long leftDist = 300;
long rightDist = 300;
int tcrtValue = 0;

String robotState = "STOP";
String sensorStatus = "READY";
String piCommand = "STOP";
String faceStatus = "NO FACE";
unsigned long lastPiCommandTime = 0;

int controlMode = 1;  // default: Face Follow
// 1 = Face Follow, 2 = Manual

String manualCommand = "STOP";

int danceMode = 0;
// 0 = off, 1 = slow, 2 = medium, 3 = fast

unsigned long danceTimer = 0;
int danceStep = 0;

unsigned long lastMqttSensorPublish = 0;
unsigned long lastMqttStatusPublish = 0;

// ================= MQTT PUBLISH =================
void publishStatus(String statusText) {
  if (!mqttClient.connected()) return;

  String json = "{";
  json += "\"state\":\"" + robotState + "\",";
  json += "\"status\":\"" + statusText + "\",";
  json += "\"piCommand\":\"" + piCommand + "\",";
  json += "\"face\":\"" + faceStatus + "\",";
  json += "\"controlMode\":" + String(controlMode) + ",";
  json += "\"dance\":" + String(danceMode);
  json += "}";

  mqttClient.publish(TOPIC_ROBOT_STATUS, json.c_str(), true);
}

void publishSensors() {
  if (!mqttClient.connected()) return;

  String json = "{";
  json += "\"front\":" + String(frontDist) + ",";
  json += "\"left\":" + String(leftDist) + ",";
  json += "\"right\":" + String(rightDist) + ",";
  json += "\"tcrt\":" + String(tcrtValue) + ",";
  json += "\"edge\":" + String(tcrtValue == EDGE_DETECTED_VALUE ? "true" : "false") + ",";
  json += "\"obstacle\":" + String((frontDist <= OBSTACLE_LIMIT || leftDist <= OBSTACLE_LIMIT || rightDist <= OBSTACLE_LIMIT) ? "true" : "false") + ",";
  json += "\"speed\":" + String(speedValue);
  json += "}";

  mqttClient.publish(TOPIC_ROBOT_SENSORS, json.c_str(), false);
}

// ================= SPEED =================
void setSpeed(int spd) {
  speedValue = constrain(spd, 0, 255);
  ledcWrite(ENA1, speedValue);
  ledcWrite(ENB1, speedValue);
  ledcWrite(ENA2, speedValue);
  ledcWrite(ENB2, speedValue);
}

// ================= MOTOR CONTROL =================
void stopRobot() {
  digitalWrite(INA1, LOW);
  digitalWrite(INB1, LOW);
  digitalWrite(INC1, LOW);
  digitalWrite(IND1, LOW);
  digitalWrite(INA2, LOW);
  digitalWrite(INB2, LOW);
  digitalWrite(INC2, LOW);
  digitalWrite(IND2, LOW);
  robotState = "STOP";
}

void moveForward() {
  digitalWrite(INA1, LOW);  digitalWrite(INB1, HIGH);
  digitalWrite(INC1, LOW);  digitalWrite(IND1, HIGH);
  digitalWrite(INA2, LOW);  digitalWrite(INB2, HIGH);
  digitalWrite(INC2, LOW);  digitalWrite(IND2, HIGH);
  robotState = "FORWARD";
}

void moveBackward() {
  digitalWrite(INA1, HIGH); digitalWrite(INB1, LOW);
  digitalWrite(INC1, HIGH); digitalWrite(IND1, LOW);
  digitalWrite(INA2, HIGH); digitalWrite(INB2, LOW);
  digitalWrite(INC2, HIGH); digitalWrite(IND2, LOW);
  robotState = "BACKWARD";
}

void turnLeft() {
  digitalWrite(INA1, HIGH); digitalWrite(INB1, LOW);
  digitalWrite(INC1, HIGH); digitalWrite(IND1, LOW);
  digitalWrite(INA2, LOW);  digitalWrite(INB2, HIGH);
  digitalWrite(INC2, LOW);  digitalWrite(IND2, HIGH);
  robotState = "TURN LEFT";
}

void turnRight() {
  digitalWrite(INA1, LOW);  digitalWrite(INB1, HIGH);
  digitalWrite(INC1, LOW);  digitalWrite(IND1, HIGH);
  digitalWrite(INA2, HIGH); digitalWrite(INB2, LOW);
  digitalWrite(INC2, HIGH); digitalWrite(IND2, LOW);
  robotState = "TURN RIGHT";
}

// ================= SENSOR =================
long readDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  long duration = pulseIn(echo, HIGH, 30000);
  if (duration == 0) return 300;
  return duration * 0.034 / 2;
}

void readSensors() {
  tcrtValue = digitalRead(TCRT_PIN);
  frontDist = readDistance(TRIG_FRONT, ECHO_FRONT);
  delay(8);
  leftDist = readDistance(TRIG_LEFT, ECHO_LEFT);
  delay(8);
  rightDist = readDistance(TRIG_RIGHT, ECHO_RIGHT);
}

bool edgeDetected() {
  return tcrtValue == EDGE_DETECTED_VALUE;
}

bool obstacleNear() {
  return frontDist <= OBSTACLE_LIMIT || leftDist <= OBSTACLE_LIMIT || rightDist <= OBSTACLE_LIMIT;
}

bool safeForDanceOrFace() {
  return !edgeDetected() && !obstacleNear();
}

// ================= MOVEMENT MODES =================
void searchFace() {
  sensorStatus = "NO FACE - SLOW 360 SEARCH";

  // Search rotation speed only = 210
  ledcWrite(ENA1, 210);
  ledcWrite(ENB1, 210);
  ledcWrite(ENA2, 210);
  ledcWrite(ENB2, 210);

  turnRight();  // rotate slowly until Pi detects face and sends FORWARD
}

void runCommand(String cmd, String sourceText) {
  setSpeed(speedValue);

  if (cmd == "FORWARD") {
    sensorStatus = sourceText + " - FORWARD";
    moveForward();
  } else if (cmd == "LEFT") {
    sensorStatus = sourceText + " - LEFT";
    turnLeft();
  } else if (cmd == "RIGHT") {
    sensorStatus = sourceText + " - RIGHT";
    turnRight();
  } else if (cmd == "BACKWARD") {
    sensorStatus = sourceText + " - BACKWARD";
    moveBackward();
  } else {
    sensorStatus = sourceText + " - STOP";
    stopRobot();
  }
}

void runDanceMode() {
  unsigned long now = millis();
  int beatTime;

  if (danceMode == 1) beatTime = 500;
  else if (danceMode == 2) beatTime = 300;
  else if (danceMode == 3) beatTime = 180;
  else return;

  if (now - danceTimer < beatTime) return;

  danceTimer = now;
  danceStep++;

  if (danceMode == 1) {
    sensorStatus = "DANCE 1 - SLOW";
    tone(BUZZER_PIN, 440, 80);
    if (danceStep % 4 == 0) turnLeft();
    else if (danceStep % 4 == 1) stopRobot();
    else if (danceStep % 4 == 2) turnRight();
    else stopRobot();
  } else if (danceMode == 2) {
    sensorStatus = "DANCE 2 - MEDIUM";
    tone(BUZZER_PIN, 660, 70);
    if (danceStep % 6 == 0) moveForward();
    else if (danceStep % 6 == 1) turnLeft();
    else if (danceStep % 6 == 2) moveBackward();
    else if (danceStep % 6 == 3) turnRight();
    else stopRobot();
  } else if (danceMode == 3) {
    sensorStatus = "DANCE 3 - FAST";
    tone(BUZZER_PIN, 900, 50);
    if (danceStep % 8 == 0) turnLeft();
    else if (danceStep % 8 == 1) turnRight();
    else if (danceStep % 8 == 2) moveForward();
    else if (danceStep % 8 == 3) moveBackward();
    else stopRobot();
  }

  if (danceStep > 100) danceStep = 0;
}

// ================= MQTT COMMAND HANDLER =================
void handleMqttCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  Serial.print("MQTT CMD: ");
  Serial.println(cmd);

  if (cmd == "FORWARD" || cmd == "STOP" || cmd == "LEFT" || cmd == "RIGHT" || cmd == "BACKWARD") {
    piCommand = cmd;
    lastPiCommandTime = millis();
    controlMode = 1;
    danceMode = 0;

    if (cmd == "STOP") faceStatus = "NO FACE";
    else faceStatus = "FACE DETECTED";

    sensorStatus = "MQTT PI COMMAND: " + cmd;
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("MODE:")) {
    int mode = cmd.substring(5).toInt();

    // Only 1 and 2 now. No game mode.
    if (mode == 1 || mode == 2) {
      controlMode = mode;
      danceMode = 0;
      manualCommand = "STOP";
      stopRobot();

      if (controlMode == 1) sensorStatus = "FACE FOLLOW MODE";
      else if (controlMode == 2) sensorStatus = "MANUAL MODE";

      publishStatus(sensorStatus);
    }
    return;
  }

  if (cmd.startsWith("MANUAL:")) {
    manualCommand = cmd.substring(7);
    controlMode = 2;
    danceMode = 0;
    sensorStatus = "MQTT MANUAL COMMAND: " + manualCommand;
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("DANCE:")) {
    readSensors();
    int requestedDance = cmd.substring(6).toInt();

    if (requestedDance != 0 && !safeForDanceOrFace()) {
      danceMode = 0;
      stopRobot();
      sensorStatus = "CAN'T DANCE - OBSTACLE OR EDGE";
      publishStatus(sensorStatus);
      return;
    }

    danceMode = requestedDance;
    danceStep = 0;
    danceTimer = millis();

    if (danceMode == 0) {
      noTone(BUZZER_PIN);
      stopRobot();
      sensorStatus = "DANCE STOPPED";
    } else {
      sensorStatus = "DANCE MODE STARTED";
    }

    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("SPEED:")) {
    int spd = cmd.substring(6).toInt();
    setSpeed(spd);
    sensorStatus = "SPEED UPDATED";
    publishStatus(sensorStatus);
    return;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (String(topic) == TOPIC_ROBOT_CMD) {
    handleMqttCommand(msg);
  }
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting MQTT... ");

    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected");
      mqttClient.subscribe(TOPIC_ROBOT_CMD);
      publishStatus("MQTT CONNECTED - READY");
    } else {
      Serial.print("failed rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 2s");
      delay(2000);
    }
  }
}

// ================= WEB PAGE =================
String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>LUNA AI Robot</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{margin:0;font-family:Arial,sans-serif;background:#020617;color:white;text-align:center}
header{padding:20px;background:linear-gradient(90deg,#0f172a,#075985);color:#38bdf8;font-size:28px;font-weight:bold}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;padding:16px}
.card{background:#0f172a;border:1px solid #334155;border-radius:18px;padding:18px;box-shadow:0 10px 25px #0008}
.big{font-size:34px;font-weight:bold;color:#22c55e}.warn{color:#facc15}.bad{color:#ef4444}.ok{color:#22c55e}
button{width:100%;padding:14px;margin:6px 0;border:none;border-radius:12px;color:white;font-size:16px;font-weight:bold;cursor:pointer}
.green{background:#22c55e}.blue{background:#0ea5e9}.orange{background:#f97316}.purple{background:#9333ea}.red{background:#ef4444}
input{width:100%}.row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media(max-width:800px){.grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<header>🤖 LUNA AI Robot Dashboard</header>
<div class="grid">
  <div class="card">
    <h2>Robot State</h2>
    <div id="state" class="big">STOP</div>
    <h3 id="status" class="warn">READY</h3>
    <p>Mode: <b id="controlMode">FACE FOLLOW</b></p>
    <p>Pi Command: <b id="picmd">STOP</b></p>
    <p>Face: <b id="face">NO FACE</b></p>
  </div>

  <div class="card">
    <h2>Safety Sensors</h2>
    <p>Front: <b id="front">0</b> cm</p>
    <p>Left: <b id="left">0</b> cm</p>
    <p>Right: <b id="right">0</b> cm</p>
    <p>TCRT Edge Raw: <b id="tcrt">0</b></p>
    <p>Edge Status: <b id="edgeStatus">SAFE</b></p>
  </div>

  <div class="card">
    <h2>Modes</h2>
    <button class="blue" onclick="setMode(1)">Face Follow Mode</button>
    <button class="orange" onclick="setMode(2)">Manual Mode</button>
    <button class="red" onclick="manualMove('STOP')">Emergency Stop</button>
  </div>

  <div class="card">
    <h2>Manual Control</h2>
    <button class="green" onclick="manualMove('FORWARD')">⬆ Forward</button>
    <div class="row">
      <button class="blue" onclick="manualMove('LEFT')">⬅ Left</button>
      <button class="blue" onclick="manualMove('RIGHT')">➡ Right</button>
    </div>
    <button class="orange" onclick="manualMove('BACKWARD')">⬇ Backward</button>
    <button class="red" onclick="manualMove('STOP')">Stop</button>
  </div>

  <div class="card">
    <h2>Dance Modes</h2>
    <button class="blue" onclick="setDance(1)">Dance 1 Slow</button>
    <button class="purple" onclick="setDance(2)">Dance 2 Medium</button>
    <button class="orange" onclick="setDance(3)">Dance 3 Fast</button>
    <button class="red" onclick="setDance(0)">Stop Dance</button>
  </div>

  <div class="card">
    <h2>Speed</h2>
    <h2><span id="speedValue">240</span> / 255</h2>
    <input type="range" min="0" max="255" value="240" id="speedSlider">
    <button class="green" onclick="sendSpeed()">Set Speed</button>
  </div>
</div>

<script>
function modeName(m){if(m==1)return 'FACE FOLLOW'; if(m==2)return 'MANUAL'; return 'UNKNOWN'}
function updateData(){fetch('/data').then(r=>r.json()).then(d=>{
  state.innerHTML=d.state; status.innerHTML=d.status; front.innerHTML=d.front; left.innerHTML=d.left; right.innerHTML=d.right; tcrt.innerHTML=d.tcrt;
  speedValue.innerHTML=d.speed; picmd.innerHTML=d.picmd; face.innerHTML=d.face; controlMode.innerHTML=modeName(d.controlMode);
  edgeStatus.innerHTML=d.edge ? 'EDGE DETECTED' : 'SAFE'; edgeStatus.className=d.edge?'bad':'ok';
});}
function setMode(m){fetch('/mode?value='+m)}
function setDance(m){fetch('/dance?mode='+m).then(r=>r.text()).then(t=>{status.innerHTML=t})}
function manualMove(m){fetch('/manual?move='+m)}
function sendSpeed(){fetch('/speed?value='+speedSlider.value)}
speedSlider.oninput=function(){speedValue.innerHTML=this.value}
setInterval(updateData,300);
</script>
</body>
</html>
)rawliteral";
}

// ================= WEB HANDLERS =================
void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleData() {
  String json = "{";
  json += "\"front\":" + String(frontDist) + ",";
  json += "\"left\":" + String(leftDist) + ",";
  json += "\"right\":" + String(rightDist) + ",";
  json += "\"tcrt\":" + String(tcrtValue) + ",";
  json += "\"edge\":" + String(edgeDetected() ? "true" : "false") + ",";
  json += "\"speed\":" + String(speedValue) + ",";
  json += "\"dance\":" + String(danceMode) + ",";
  json += "\"controlMode\":" + String(controlMode) + ",";
  json += "\"state\":\"" + robotState + "\",";
  json += "\"picmd\":\"" + piCommand + "\",";
  json += "\"face\":\"" + faceStatus + "\",";
  json += "\"status\":\"" + sensorStatus + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleSpeed() {
  if (server.hasArg("value")) {
    setSpeed(server.arg("value").toInt());
    sensorStatus = "WEB SPEED UPDATED";
    publishStatus(sensorStatus);
  }
  server.send(200, "text/plain", "Speed Updated");
}

void handleDance() {
  readSensors();

  if (server.hasArg("mode")) {
    int requestedDance = server.arg("mode").toInt();

    if (requestedDance != 0 && !safeForDanceOrFace()) {
      danceMode = 0;
      stopRobot();
      sensorStatus = "CAN'T DANCE - OBSTACLE OR EDGE";
      publishStatus(sensorStatus);
      server.send(200, "text/plain", "CAN'T DANCE - OBSTACLE OR EDGE");
      return;
    }

    danceMode = requestedDance;
    danceStep = 0;
    danceTimer = millis();

    if (danceMode == 0) {
      noTone(BUZZER_PIN);
      stopRobot();
      sensorStatus = "DANCE STOPPED";
    } else {
      sensorStatus = "DANCE MODE STARTED";
    }

    publishStatus(sensorStatus);
  }

  server.send(200, "text/plain", "Dance Mode Updated");
}

void handleMode() {
  if (server.hasArg("value")) {
    int requestedMode = server.arg("value").toInt();

    // Only 1 and 2. Game removed.
    if (requestedMode == 1 || requestedMode == 2) {
      controlMode = requestedMode;
      danceMode = 0;
      manualCommand = "STOP";
      stopRobot();

      if (controlMode == 1) sensorStatus = "FACE FOLLOW MODE";
      else if (controlMode == 2) sensorStatus = "MANUAL MODE";

      publishStatus(sensorStatus);
    }
  }

  server.send(200, "text/plain", "Mode Updated");
}

void handleManual() {
  if (server.hasArg("move")) {
    manualCommand = server.arg("move");
    manualCommand.toUpperCase();
    controlMode = 2;
    danceMode = 0;
    sensorStatus = "MANUAL COMMAND: " + manualCommand;
    publishStatus(sensorStatus);
  }

  server.send(200, "text/plain", "Manual Command OK");
}

// Keep old HTTP Pi endpoint also working, but MQTT is the main control now.
void handlePiCmd() {
  if (server.hasArg("move")) {
    piCommand = server.arg("move");
    piCommand.toUpperCase();
    lastPiCommandTime = millis();
    controlMode = 1;
    danceMode = 0;
    faceStatus = (piCommand == "STOP") ? "NO FACE" : "FACE DETECTED";
    sensorStatus = "HTTP PI COMMAND: " + piCommand;
    publishStatus(sensorStatus);
  }

  server.send(200, "text/plain", "OK " + piCommand);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(INA1, OUTPUT); pinMode(INB1, OUTPUT); pinMode(INC1, OUTPUT); pinMode(IND1, OUTPUT);
  pinMode(INA2, OUTPUT); pinMode(INB2, OUTPUT); pinMode(INC2, OUTPUT); pinMode(IND2, OUTPUT);

  ledcAttach(ENA1, 1000, 8);
  ledcAttach(ENB1, 1000, 8);
  ledcAttach(ENA2, 1000, 8);
  ledcAttach(ENB2, 1000, 8);

  pinMode(TRIG_FRONT, OUTPUT); pinMode(ECHO_FRONT, INPUT);
  pinMode(TRIG_LEFT, OUTPUT); pinMode(ECHO_LEFT, INPUT);
  pinMode(TRIG_RIGHT, OUTPUT); pinMode(ECHO_RIGHT, INPUT);

  pinMode(TCRT_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  setSpeed(speedValue);
  stopRobot();

  WiFi.begin(ssid, password);
  Serial.println("Connecting WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Connected SSID: ");
Serial.println(WiFi.SSID());

Serial.print("MQTT Server: ");
Serial.println(mqtt_server);

Serial.print("Gateway: ");
Serial.println(WiFi.gatewayIP());

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/speed", handleSpeed);
  server.on("/dance", handleDance);
  server.on("/mode", handleMode);
  server.on("/manual", handleManual);
  server.on("/picmd", handlePiCmd);

  server.begin();
  Serial.println("Web Server Started");
}

// ================= LOOP =================
void loop() {
  server.handleClient();

  if (!mqttClient.connected()) {
    reconnectMqtt();
  }
  mqttClient.loop();

  readSensors();

  unsigned long now = millis();

  if (now - lastMqttSensorPublish > 500) {
    lastMqttSensorPublish = now;
    publishSensors();
  }

  if (now - lastMqttStatusPublish > 1500) {
    lastMqttStatusPublish = now;
    publishStatus(sensorStatus);
  }

  // Edge safety always active, including manual
  if (edgeDetected()) {
    stopRobot();
    danceMode = 0;
    sensorStatus = "EDGE DETECTED - STOP";
    publishStatus(sensorStatus);
    return;
  }

  // Manual mode works even when ultrasonic says obstacle. This helps testing.
  // Edge still stops manual.
  if (controlMode == 2) {
    noTone(BUZZER_PIN);
    runCommand(manualCommand, "MANUAL");
    return;
  }

  // Face + dance are blocked by 15cm obstacle
  if (obstacleNear()) {
    stopRobot();
    danceMode = 0;
    sensorStatus = "OBSTACLE WITHIN 15CM - STOP";
    publishStatus(sensorStatus);
    return;
  }

  if (danceMode != 0) {
    runDanceMode();
    return;
  }

  noTone(BUZZER_PIN);

  if (controlMode == 1) {
    if (millis() - lastPiCommandTime > 1200) {
      piCommand = "STOP";
      faceStatus = "NO FACE / PI TIMEOUT";
    }

    if (piCommand == "FORWARD") runCommand("FORWARD", "FACE CENTER");
    else if (piCommand == "LEFT") runCommand("LEFT", "FACE LEFT");
    else if (piCommand == "RIGHT") runCommand("RIGHT", "FACE RIGHT");
    else searchFace();

    return;
  }

  stopRobot();
  sensorStatus = "IDLE";
}
