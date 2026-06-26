#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

// ================= WIFI =================
const char* ssid = "Yasiru";
const char* password = "111111111";
String lastPublishedStatus = "";
String lastPublishedEmotion = "";

// ================= MQTT =================
// MQTT broker runs on Raspberry Pi
// If raspberrypi.local does not work, replace with Pi IP, example: "10.140.180.xxx"
const char* mqtt_server = "10.112.241.209";
const int mqtt_port = 1883;

bool danceStatusPublished = false;
const char* MQTT_CLIENT_ID = "luna-motor-esp32";

const char* TOPIC_ROBOT_CMD     = "luna/robot/cmd";      // old common topic still supported
const char* TOPIC_ROBOT_MANUAL  = "luna/robot/manual";   // PC / web manual control
const char* TOPIC_ROBOT_FACE    = "luna/robot/face";     // Python face program only
const char* TOPIC_ROBOT_STATUS  = "luna/robot/status";
const char* TOPIC_ROBOT_SENSORS = "luna/robot/sensors";
const char* TOPIC_ROBOT_EMOTION = "luna/robot/emotion";

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
int speedValue = 240;      // Manual speed
int faceSpeed = 200;       // Face follow medium speed
int turnSpeed = 180;       // Face search turn speed
int danceSpeed = 220;      // Dance speed
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
// 1 = Face Follow, 2 = Manual, 3 = Dance

String manualCommand = "STOP";

int danceMode = 0;
// 0 = off, 1 = slow, 2 = medium, 3 = fast

unsigned long danceTimer = 0;
int danceStep = 0;

unsigned long lastMqttSensorPublish = 0;
unsigned long lastMqttStatusPublish = 0;

// ================= MQTT PUBLISH =================
void publishEmotion(String mood) {
  if (!mqttClient.connected()) return;

  mood.trim();
  mood.toUpperCase();

  if (mood == lastPublishedEmotion) return;

  lastPublishedEmotion = mood;
  mqttClient.publish(TOPIC_ROBOT_EMOTION, mood.c_str(), true);

  Serial.print("EMOTION -> ");
  Serial.println(mood);
}

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

  if (json == lastPublishedStatus) return;

  lastPublishedStatus = json;
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
  json += "\"obstacle\":" + String(obstacleFront() ? "true" : "false") + ",";
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

bool obstacleFront() {
  return frontDist <= OBSTACLE_LIMIT;
}

bool sideDisturb() {
  return leftDist <= OBSTACLE_LIMIT || rightDist <= OBSTACLE_LIMIT;
}

bool obstacleNear() {
  return obstacleFront() || sideDisturb();
}

bool safeForDanceOrFace() {
  // For face follow and dance, front obstacle must stop.
  // Left/right readings are shown in web data but do not force all modes to SAD.
  return !edgeDetected() && !obstacleFront();
}

// ================= MOVEMENT MODES =================
void searchFace() {
  sensorStatus = "NO FACE - SLOW SEARCH";
  publishEmotion("SAD");

  // Face search turn speed
  ledcWrite(ENA1, turnSpeed);
  ledcWrite(ENB1, turnSpeed);
  ledcWrite(ENA2, turnSpeed);
  ledcWrite(ENB2, turnSpeed);

  turnRight();  // rotate slowly until Python detects face and sends FORWARD
}

void runCommand(String cmd, String sourceText) {
  cmd.trim();
  cmd.toUpperCase();

  // Manual uses slider speed. Face uses medium speed.
  if (sourceText.indexOf("FACE") >= 0) {
    ledcWrite(ENA1, faceSpeed);
    ledcWrite(ENB1, faceSpeed);
    ledcWrite(ENA2, faceSpeed);
    ledcWrite(ENB2, faceSpeed);
  } else {
    setSpeed(speedValue);
  }

  if (cmd == "FORWARD") {
    sensorStatus = sourceText + " - FORWARD";
    moveForward();

    if (sourceText.indexOf("FACE") >= 0) publishEmotion("LOVE");
    else publishEmotion("HAPPY");
  } 
  else if (cmd == "LEFT") {
    sensorStatus = sourceText + " - LEFT";
    turnLeft();
    publishEmotion("ANGRY");
  } 
  else if (cmd == "RIGHT") {
    sensorStatus = sourceText + " - RIGHT";
    turnRight();
    publishEmotion("ANGRY");
  } 
  else if (cmd == "BACKWARD") {
    sensorStatus = sourceText + " - BACKWARD";
    moveBackward();
    publishEmotion("ANGRY");
  } 
  else {
    sensorStatus = sourceText + " - STOP";
    stopRobot();
    publishEmotion("SAD");
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

  publishEmotion("HAPPY");

  if (!danceStatusPublished) {
    if (danceMode == 1) sensorStatus = "DANCE 1 - SLOW";
    else if (danceMode == 2) sensorStatus = "DANCE 2 - MEDIUM";
    else if (danceMode == 3) sensorStatus = "DANCE 3 - FAST";

    // Do not publish every dance step. DANCE command already publishes start/stop.
    danceStatusPublished = true;
  }

  if (danceMode == 1) {
    tone(BUZZER_PIN, 440, 80);
    if (danceStep % 4 == 0) turnLeft();
    else if (danceStep % 4 == 1) stopRobot();
    else if (danceStep % 4 == 2) turnRight();
    else stopRobot();
  } 
  else if (danceMode == 2) {
    tone(BUZZER_PIN, 660, 70);
    if (danceStep % 6 == 0) moveForward();
    else if (danceStep % 6 == 1) turnLeft();
    else if (danceStep % 6 == 2) moveBackward();
    else if (danceStep % 6 == 3) turnRight();
    else stopRobot();
  } 
  else if (danceMode == 3) {
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
void startDance(int requestedDance) {
  readSensors();

  if (requestedDance != 0 && !safeForDanceOrFace()) {
    danceMode = 0;
    stopRobot();
    sensorStatus = "CAN'T DANCE - OBSTACLE OR EDGE";
    publishEmotion(edgeDetected() ? "EMERGENCY" : "SAD");
    publishStatus(sensorStatus);
    return;
  }

  danceMode = requestedDance;
  danceStep = 0;
  danceTimer = millis();
  danceStatusPublished = false;

  if (danceMode == 0) {
    noTone(BUZZER_PIN);
    stopRobot();
    controlMode = 2;       // after dance stop, stay in manual mode
    sensorStatus = "DANCE STOPPED";
    publishEmotion("SAD");
  } else {
    controlMode = 3;       // dance mode, ignore face/manual until stopped or mode changed
    sensorStatus = "DANCE MODE STARTED";
    publishEmotion("HAPPY");
  }

  publishStatus(sensorStatus);
}

void handleFaceCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // Face topic only works in FACE FOLLOW mode.
  // It cannot interrupt manual mode or dance mode.
  if (controlMode != 1 || danceMode != 0) {
    return;
  }

  if (cmd == "FORWARD" || cmd == "STOP") {
    piCommand = cmd;
    lastPiCommandTime = millis();

    if (cmd == "STOP") faceStatus = "NO FACE";
    else faceStatus = "FACE DETECTED";

    sensorStatus = "FACE COMMAND: " + cmd;
    publishStatus(sensorStatus);
  }
}

void handleManualCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // Manual command switches to manual and stops dance.
  manualCommand = cmd;
  controlMode = 2;
  danceMode = 0;
  noTone(BUZZER_PIN);

  sensorStatus = "MQTT MANUAL COMMAND: " + manualCommand;
  if (manualCommand == "FORWARD") publishEmotion("HAPPY");
  else if (manualCommand == "LEFT" || manualCommand == "RIGHT" || manualCommand == "BACKWARD") publishEmotion("ANGRY");
  else publishEmotion("SAD");
  publishStatus(sensorStatus);
}

void handleMqttCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  Serial.print("MQTT CMD: ");
  Serial.println(cmd);

  // Emergency/normal stop from any topic must always stop immediately.
  if (cmd == "STOP" || cmd == "EMERGENCY_STOP") {
    danceMode = 0;
    noTone(BUZZER_PIN);
    manualCommand = "STOP";
    piCommand = "STOP";
    faceStatus = "NO FACE";
    stopRobot();
    sensorStatus = "MQTT STOP";
    publishEmotion("SAD");
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("MODE:")) {
    int mode = cmd.substring(5).toInt();

    if (mode == 1 || mode == 2) {
      controlMode = mode;
      danceMode = 0;
      manualCommand = "STOP";
      piCommand = "STOP";
      faceStatus = "NO FACE";
      noTone(BUZZER_PIN);
      stopRobot();

      if (controlMode == 1) sensorStatus = "FACE FOLLOW MODE";
      else sensorStatus = "MANUAL MODE";

      publishEmotion("SAD");
      publishStatus(sensorStatus);
    }
    return;
  }

  if (cmd.startsWith("MANUAL:")) {
    handleManualCommand(cmd.substring(7));
    return;
  }

  if (cmd.startsWith("DANCE:")) {
    startDance(cmd.substring(6).toInt());
    return;
  }

  if (cmd.startsWith("SPEED:")) {
    int spd = cmd.substring(6).toInt();
    setSpeed(spd);
    sensorStatus = "SPEED UPDATED";
    publishStatus(sensorStatus);
    return;
  }

  // Old common command topic support:
  // LEFT/RIGHT/BACKWARD/FORWARD become manual commands.
  if (cmd == "FORWARD" || cmd == "LEFT" || cmd == "RIGHT" || cmd == "BACKWARD") {
    handleManualCommand(cmd);
    return;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  String t = String(topic);

  if (t == TOPIC_ROBOT_FACE) {
    handleFaceCommand(msg);     // Python face: FORWARD / STOP only
    return;
  }

  if (t == TOPIC_ROBOT_MANUAL) {
    handleManualCommand(msg);   // PC manual topic
    return;
  }

  if (t == TOPIC_ROBOT_CMD) {
    handleMqttCommand(msg);     // old shared topic still supported
    return;
  }
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting MQTT... ");

    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected");
      mqttClient.subscribe(TOPIC_ROBOT_CMD);
      mqttClient.subscribe(TOPIC_ROBOT_MANUAL);
      mqttClient.subscribe(TOPIC_ROBOT_FACE);
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
    <h2>Manual Speed</h2>
    <h2><span id="speedValue">240</span> / 255</h2>
    <input type="range" min="0" max="255" value="240" id="speedSlider">
    <button class="green" onclick="sendSpeed()">Set Manual Speed</button>
  </div>

  <div class="card">
    <h2>Face Forward Speed</h2>
    <h2><span id="faceSpeedValue">200</span> / 255</h2>
    <input type="range" min="120" max="240" value="200" id="faceSpeedSlider">
    <button class="green" onclick="sendFaceSpeed()">Set Face Speed</button>
  </div>

  <div class="card">
    <h2>Face Search Turn Speed</h2>
    <h2><span id="turnSpeedValue">180</span> / 255</h2>
    <input type="range" min="120" max="220" value="180" id="turnSpeedSlider">
    <button class="green" onclick="sendTurnSpeed()">Set Search Speed</button>
  </div>
</div>

<script>
function modeName(m){ if(m==1)return 'FACE FOLLOW'; if(m==2)return 'MANUAL'; if(m==3)return 'DANCE'; return 'UNKNOWN'; }
function api(url){ fetch(url).then(()=>setTimeout(updateData,150)).catch(e=>alert('ESP32 web command failed')); }
function setMode(m){ api('/mode?value=' + m); }
function setDance(m){ api('/dance?mode=' + m); }
function manualMove(m){ api('/manual?move=' + m); }
function sendSpeed(){ api('/speed?value=' + speedSlider.value); }
function sendFaceSpeed(){ api('/faceSpeed?value=' + faceSpeedSlider.value); }
function sendTurnSpeed(){ api('/turnSpeed?value=' + turnSpeedSlider.value); }
speedSlider.oninput=function(){speedValue.innerHTML=this.value;}
faceSpeedSlider.oninput=function(){faceSpeedValue.innerHTML=this.value;}
turnSpeedSlider.oninput=function(){turnSpeedValue.innerHTML=this.value;}
function updateData(){fetch('/data').then(r=>r.json()).then(d=>{
  state.innerHTML=d.state; status.innerHTML=d.status; front.innerHTML=d.front; left.innerHTML=d.left; right.innerHTML=d.right; tcrt.innerHTML=d.tcrt;
  speedValue.innerHTML=d.speed; speedSlider.value=d.speed;
  faceSpeedValue.innerHTML=d.faceSpeed; faceSpeedSlider.value=d.faceSpeed;
  turnSpeedValue.innerHTML=d.turnSpeed; turnSpeedSlider.value=d.turnSpeed;
  picmd.innerHTML=d.picmd; face.innerHTML=d.face; controlMode.innerHTML=modeName(d.controlMode);
  edgeStatus.innerHTML=d.edge ? 'EDGE DETECTED' : 'SAFE'; edgeStatus.className=d.edge?'bad':'ok';
}).catch(e=>{ status.innerHTML='WEB DATA ERROR'; });}
setInterval(updateData,1000);
updateData();
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
  json += "\"faceSpeed\":" + String(faceSpeed) + ",";
  json += "\"turnSpeed\":" + String(turnSpeed) + ",";
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

void handleFaceSpeed() {
  if (server.hasArg("value")) {
    faceSpeed = constrain(server.arg("value").toInt(), 120, 240);
    sensorStatus = "FACE SPEED UPDATED: " + String(faceSpeed);
    publishStatus(sensorStatus);
  }
  server.send(200, "text/plain", "Face Speed Updated");
}

void handleTurnSpeed() {
  if (server.hasArg("value")) {
    turnSpeed = constrain(server.arg("value").toInt(), 120, 220);
    sensorStatus = "FACE SEARCH SPEED UPDATED: " + String(turnSpeed);
    publishStatus(sensorStatus);
  }
  server.send(200, "text/plain", "Face Search Speed Updated");
}

void handleDance() {
  if (server.hasArg("mode")) {
    int requestedDance = server.arg("mode").toInt();
    startDance(requestedDance);
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
    handleManualCommand(server.arg("move"));
  }

  server.send(200, "text/plain", "Manual Command OK");
}


// Keep old HTTP Pi endpoint also working, but MQTT is the main control now.
void handlePiCmd() {
  if (server.hasArg("move")) {
    String move = server.arg("move");
    handleFaceCommand(move);
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

  if (MDNS.begin("luna-motor-esp32")) {
  Serial.println("mDNS started: luna-motor-esp32.local");
} else {
  Serial.println("mDNS failed");
}

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
  server.on("/faceSpeed", handleFaceSpeed);
  server.on("/turnSpeed", handleTurnSpeed);
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

  if (now - lastMqttSensorPublish > 1000) {
    lastMqttSensorPublish = now;
    publishSensors();
  }

  // Safety priority 1: Edge / fall danger always stops everything
  if (edgeDetected()) {
    stopRobot();
    danceMode = 0;
    noTone(BUZZER_PIN);
    sensorStatus = "EDGE / FALL DANGER - STOP";
    publishEmotion("EMERGENCY");
    publishStatus(sensorStatus);
    return;
  }

  // Safety priority 2: Front obstacle blocks forward, face-follow, and dance.
  // Manual LEFT / RIGHT / BACKWARD can still work so the robot can escape.
  if (obstacleFront()) {
    bool tryingForwardManual = (controlMode == 2 && manualCommand == "FORWARD");
    bool tryingFaceOrDance = (controlMode == 1 || controlMode == 3 || danceMode != 0);

    if (tryingForwardManual || tryingFaceOrDance) {
      stopRobot();
      danceMode = 0;
      noTone(BUZZER_PIN);
      sensorStatus = "FRONT OBSTACLE - STOP";
      publishEmotion("SAD");
      publishStatus(sensorStatus);
      return;
    }
  }

  // Dance mode: only dance runs. Face/manual commands are ignored until stop/mode change.
  if (controlMode == 3 && danceMode != 0) {
    ledcWrite(ENA1, danceSpeed);
    ledcWrite(ENB1, danceSpeed);
    ledcWrite(ENA2, danceSpeed);
    ledcWrite(ENB2, danceSpeed);
    runDanceMode();
    return;
  }

  // Manual mode: only manual command runs. Face commands are ignored.
  if (controlMode == 2) {
    noTone(BUZZER_PIN);
    runCommand(manualCommand, "MANUAL");
    return;
  }

  // Face follow mode: only Python face command runs. Manual/dance does not run.
  if (controlMode == 1) {
    noTone(BUZZER_PIN);

    if (millis() - lastPiCommandTime > 1200) {
      piCommand = "STOP";
      faceStatus = "NO FACE / PI TIMEOUT";
    }

    if (piCommand == "FORWARD") {
      runCommand("FORWARD", "FACE CENTER");
    } else {
      searchFace();
    }

    return;
  }

  stopRobot();
  sensorStatus = "IDLE";
  publishEmotion("SAD");
}
