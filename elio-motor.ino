#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

// ================= WIFI =================
#define WIFI_SSID_1    "Yasiru"
#define WIFI_PASS_1    "111111111"

// Backup WiFi hotspot
#define WIFI_SSID_2    "Amrith’s iPhone"
#define WIFI_PASS_2    "brat summer"

const char* ssid = WIFI_SSID_1;
const char* password = WIFI_PASS_1;

// ================= MQTT mDNS =================
#define ESP32_MDNS_HOST "luna-motor"
#define MQTT_MDNS_HOST  "raspberrypi"   // raspberrypi.local
const int mqtt_port = 1883;

IPAddress mqttIP;
unsigned long lastMdnsTry = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

String lastPublishedStatus = "";
String lastPublishedEmotion = "";

const char* MQTT_CLIENT_ID = "luna-motor-esp32";

const char* TOPIC_ROBOT_CMD     = "luna/robot/cmd";
const char* TOPIC_ROBOT_MANUAL  = "luna/robot/manual";
const char* TOPIC_ROBOT_FACE    = "luna/robot/face";
const char* TOPIC_ROBOT_STATUS  = "luna/robot/status";
const char* TOPIC_ROBOT_SENSORS = "luna/robot/sensors";
const char* TOPIC_ROBOT_EMOTION = "luna/robot/emotion";

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

#define EDGE_DETECTED_VALUE 1
#define OBSTACLE_LIMIT 15

// ================= VALUES =================
int speedValue = 240;
int faceSpeed = 200;
int turnSpeed = 230;
int danceSpeed = 220;

long frontDist = 300;
long leftDist = 300;
long rightDist = 300;
int tcrtValue = 0;

String robotState = "STOP";
String sensorStatus = "READY - MANUAL MODE";
String piCommand = "STOP";
String faceStatus = "NO FACE";
String manualCommand = "STOP";

unsigned long lastPiCommandTime = 0;
unsigned long lastMqttSensorPublish = 0;

int controlMode = 2; 
// 1 = Face Follow, 2 = Manual, 3 = Dance
// DEFAULT = MANUAL. Face commands cannot move robot in manual mode.

int danceMode = 0;
int danceStep = 0;
unsigned long danceTimer = 0;
bool danceStatusPublished = false;

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

bool edgeDetected();
bool obstacleFront();
void updateEmotion();

void publishSensors() {
  if (!mqttClient.connected()) return;

  String json = "{";
  json += "\"front\":" + String(frontDist) + ",";
  json += "\"left\":" + String(leftDist) + ",";
  json += "\"right\":" + String(rightDist) + ",";
  json += "\"tcrt\":" + String(tcrtValue) + ",";
  json += "\"edge\":" + String(edgeDetected() ? "true" : "false") + ",";
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

bool safeForDanceOrFace() {
  return !edgeDetected() && !obstacleFront();
}

// ================= MOVEMENT =================
void searchFace() {
  sensorStatus = "NO FACE - SLOW SEARCH";

  ledcWrite(ENA1, turnSpeed);
  ledcWrite(ENB1, turnSpeed);
  ledcWrite(ENA2, turnSpeed);
  ledcWrite(ENB2, turnSpeed);

  turnRight();
}

void runCommand(String cmd, String sourceText) {
  cmd.trim();
  cmd.toUpperCase();

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
  } 
  else if (cmd == "LEFT") {
    sensorStatus = sourceText + " - LEFT";
    turnLeft();
  } 
  else if (cmd == "RIGHT") {
    sensorStatus = sourceText + " - RIGHT";
    turnRight();
  } 
  else if (cmd == "BACKWARD") {
    sensorStatus = sourceText + " - BACKWARD";
    moveBackward();
  } 
  else {
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
  else if (danceMode == 4) beatTime = 260;
  else if (danceMode == 5) beatTime = 220;
  else if (danceMode == 6) beatTime = 140;
  else return;

  if (now - danceTimer < beatTime) return;

  danceTimer = now;
  danceStep++;


  if (!danceStatusPublished) {
    if (danceMode == 1) sensorStatus = "DANCE 1 - SLOW";
    else if (danceMode == 2) sensorStatus = "DANCE 2 - MEDIUM";
    else if (danceMode == 3) sensorStatus = "DANCE 3 - FAST";
    else if (danceMode == 4) sensorStatus = "DANCE 4 - SWING";
    else if (danceMode == 5) sensorStatus = "DANCE 5 - SPIN";
    else if (danceMode == 6) sensorStatus = "DANCE 6 - PARTY";

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
  else if (danceMode == 4) {
    tone(BUZZER_PIN, 520, 60);
    if (danceStep % 6 == 0) moveForward();
    else if (danceStep % 6 == 1) stopRobot();
    else if (danceStep % 6 == 2) moveBackward();
    else if (danceStep % 6 == 3) stopRobot();
    else if (danceStep % 6 == 4) turnLeft();
    else turnRight();
  }
  else if (danceMode == 5) {
    tone(BUZZER_PIN, 760, 55);
    if (danceStep % 5 == 0) turnLeft();
    else if (danceStep % 5 == 1) turnLeft();
    else if (danceStep % 5 == 2) stopRobot();
    else if (danceStep % 5 == 3) turnRight();
    else stopRobot();
  }
  else if (danceMode == 6) {
    tone(BUZZER_PIN, 1000, 45);
    if (danceStep % 10 == 0) moveForward();
    else if (danceStep % 10 == 1) turnLeft();
    else if (danceStep % 10 == 2) turnRight();
    else if (danceStep % 10 == 3) moveBackward();
    else if (danceStep % 10 == 4) turnRight();
    else if (danceStep % 10 == 5) turnLeft();
    else stopRobot();
  }

  if (danceStep > 100) danceStep = 0;
}

// ================= MQTT COMMANDS =================
void startDance(int requestedDance) {
  if (requestedDance < 0 || requestedDance > 6) requestedDance = 0;

  readSensors();

  if (requestedDance != 0 && !safeForDanceOrFace()) {
    danceMode = 0;
    stopRobot();
    sensorStatus = "CAN'T DANCE - OBSTACLE OR EDGE";
    updateEmotion();
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
    controlMode = 2;
    sensorStatus = "DANCE STOPPED";
  } else {
    controlMode = 3;
    sensorStatus = "DANCE MODE STARTED";
  }

  publishStatus(sensorStatus);
}

void enterManualMode() {
  controlMode = 2;
  danceMode = 0;
  noTone(BUZZER_PIN);
  manualCommand = "STOP";
  piCommand = "STOP";
  faceStatus = "FACE IGNORED - MANUAL MODE";
  lastPiCommandTime = 0;
  stopRobot();
  sensorStatus = "MANUAL MODE";
  publishStatus(sensorStatus);
}

void enterFaceMode() {
  controlMode = 1;
  danceMode = 0;
  noTone(BUZZER_PIN);
  manualCommand = "STOP";
  piCommand = "STOP";
  faceStatus = "NO FACE";
  lastPiCommandTime = millis();
  stopRobot();
  sensorStatus = "FACE FOLLOW MODE";
  publishStatus(sensorStatus);
}

void handleFaceCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // STRICT SAFETY RULE:
  // Face detection FORWARD/STOP works ONLY in Face Follow mode.
  // In Manual mode, face messages are ignored and robot will NOT move forward.
  if (controlMode != 1 || danceMode != 0) {
    piCommand = "STOP";
    faceStatus = "FACE IGNORED - MANUAL MODE";
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

  // Mode commands on manual topic:
  // mosquitto_pub -h raspberrypi.local -t luna/robot/manual -m "MANUAL"
  // mosquitto_pub -h raspberrypi.local -t luna/robot/manual -m "FACE"
  if (cmd == "MANUAL" || cmd == "MODE:MANUAL" || cmd == "MODE:2") {
    enterManualMode();
    return;
  }

  if (cmd == "FACE" || cmd == "FACE_FOLLOW" || cmd == "MODE:FACE" || cmd == "MODE:1") {
    enterFaceMode();
    return;
  }

  if (cmd.startsWith("SPEED:")) {
    setSpeed(cmd.substring(6).toInt());
    sensorStatus = "MANUAL SPEED UPDATED: " + String(speedValue);
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("FACE_SPEED:")) {
    faceSpeed = constrain(cmd.substring(11).toInt(), 120, 255);
    sensorStatus = "FACE SPEED UPDATED: " + String(faceSpeed);
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("TURN_SPEED:")) {
    turnSpeed = constrain(cmd.substring(11).toInt(), 120, 255);
    sensorStatus = "FACE SEARCH SPEED UPDATED: " + String(turnSpeed);
    publishStatus(sensorStatus);
    return;
  }

  if (cmd == "FORWARD" || cmd == "LEFT" || cmd == "RIGHT" || cmd == "BACKWARD" || cmd == "STOP") {
    // Manual topic movement is allowed only as manual control.
    // It also clears face command so face cannot override manual mode.
    controlMode = 2;
    danceMode = 0;
    noTone(BUZZER_PIN);
    piCommand = "STOP";
    faceStatus = "FACE IGNORED - MANUAL MODE";
    manualCommand = cmd;
    sensorStatus = "MQTT MANUAL COMMAND: " + manualCommand;
    publishStatus(sensorStatus);
    return;
  }
}

void handleMqttCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  Serial.print("MQTT CMD: ");
  Serial.println(cmd);

  if (cmd == "EMERGENCY_STOP") {
    danceMode = 0;
    noTone(BUZZER_PIN);
    manualCommand = "STOP";
    piCommand = "STOP";
    faceStatus = "NO FACE";
    stopRobot();
    sensorStatus = "MQTT EMERGENCY STOP";
    publishStatus(sensorStatus);
    return;
  }

  if (cmd == "MANUAL" || cmd == "MODE:MANUAL" || cmd == "MODE:2") {
    enterManualMode();
    return;
  }

  if (cmd == "FACE" || cmd == "FACE_FOLLOW" || cmd == "MODE:FACE" || cmd == "MODE:1") {
    enterFaceMode();
    return;
  }

  if (cmd.startsWith("MODE:")) {
    String modeText = cmd.substring(5);
    modeText.trim();
    if (modeText == "1" || modeText == "FACE" || modeText == "FACE_FOLLOW") enterFaceMode();
    else if (modeText == "2" || modeText == "MANUAL") enterManualMode();
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
    setSpeed(cmd.substring(6).toInt());
    sensorStatus = "SPEED UPDATED";
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("FACE_SPEED:")) {
    faceSpeed = constrain(cmd.substring(11).toInt(), 120, 255);
    sensorStatus = "FACE SPEED UPDATED: " + String(faceSpeed);
    publishStatus(sensorStatus);
    return;
  }

  if (cmd.startsWith("TURN_SPEED:")) {
    turnSpeed = constrain(cmd.substring(11).toInt(), 120, 255);
    sensorStatus = "FACE SEARCH SPEED UPDATED: " + String(turnSpeed);
    publishStatus(sensorStatus);
    return;
  }

  // IMPORTANT FIX:
  // Plain FORWARD from luna/robot/cmd is treated as FACE command only when Face mode is active.
  // In Manual mode it is ignored, so face detection cannot push robot forward.
  if (cmd == "FORWARD") {
    handleFaceCommand(cmd);
    return;
  }

  if (cmd == "STOP") {
    if (controlMode == 1) handleFaceCommand(cmd);
    else handleManualCommand(cmd);
    return;
  }

  if (cmd == "LEFT" || cmd == "RIGHT" || cmd == "BACKWARD") {
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
    handleFaceCommand(msg);
    return;
  }

  if (t == TOPIC_ROBOT_MANUAL) {
    handleManualCommand(msg);
    return;
  }

  if (t == TOPIC_ROBOT_CMD) {
    handleMqttCommand(msg);
    return;
  }
}


// ================= WIFI CONNECT =================
bool connectToWiFi(const char* wifiSsid, const char* wifiPass, unsigned long timeoutMs) {
  Serial.print("Connecting WiFi: ");
  Serial.println(wifiSsid);

  WiFi.begin(wifiSsid, wifiPass);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ssid = wifiSsid;
    password = wifiPass;

    Serial.println("WiFi Connected!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    return true;
  }

  WiFi.disconnect(true);
  delay(500);
  Serial.println("WiFi failed");
  return false;
}

// ================= mDNS MQTT =================
bool resolveMqttBroker() {
  IPAddress resolved = MDNS.queryHost(MQTT_MDNS_HOST, 1000);

  if (resolved == IPAddress(0, 0, 0, 0)) {
    Serial.println("[mDNS] Broker not found");
    return false;
  }

  if (resolved != mqttIP) {
    mqttIP = resolved;

    Serial.print("[mDNS] Resolved ");
    Serial.print(MQTT_MDNS_HOST);
    Serial.print(".local -> ");
    Serial.println(mqttIP);

    mqttClient.setServer(mqttIP, mqtt_port);
    mqttClient.disconnect();
  }

  return true;
}

void reconnectMqtt() {
  if (mqttIP == IPAddress(0, 0, 0, 0)) return;

  if (mqttClient.connected()) return;

  Serial.print("Connecting MQTT to ");
  Serial.print(mqttIP);
  Serial.print(" ... ");

  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("connected");

    mqttClient.subscribe(TOPIC_ROBOT_CMD);
    mqttClient.subscribe(TOPIC_ROBOT_MANUAL);
    mqttClient.subscribe(TOPIC_ROBOT_FACE);

    publishStatus("MQTT CONNECTED - READY");
    publishEmotion("HAPPY");
  } else {
    Serial.print("failed rc=");
    Serial.print(mqttClient.state());
    Serial.println(" retry later");
  }
}

// ================= WEB PAGE =================
String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>ELIO Robot Dashboard</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{--bg:#07111f;--card:#0f1b2d;--line:#263852;--text:#eef7ff;--muted:#9fb2cc;--cyan:#38bdf8;--green:#22c55e;--red:#ef4444;--orange:#f97316;--purple:#9333ea}
*{box-sizing:border-box}
body{margin:0;font-family:Arial,sans-serif;background:radial-gradient(circle at top,#123456 0,#07111f 42%,#020617 100%);color:var(--text);text-align:center}
header{padding:24px 16px;background:linear-gradient(90deg,#07111f,#0f766e,#075985);box-shadow:0 10px 30px #0007}
header .title{font-size:32px;font-weight:900;letter-spacing:.5px}
header .sub{font-size:14px;color:#d5f3ff;margin-top:6px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;padding:16px;max-width:1100px;margin:auto}
.card{background:linear-gradient(180deg,rgba(15,27,45,.96),rgba(15,23,42,.96));border:1px solid var(--line);border-radius:22px;padding:18px;box-shadow:0 14px 35px #0008}
h2{margin-top:0;color:#bae6fd}.big{font-size:38px;font-weight:900;color:var(--green)}.warn{color:#facc15}.bad{color:var(--red)}.ok{color:var(--green)}
button{width:100%;padding:14px;margin:6px 0;border:none;border-radius:14px;color:white;font-size:16px;font-weight:800;cursor:pointer;box-shadow:0 8px 18px #0006}
button:hover{filter:brightness(1.08);transform:translateY(-1px)}
.green{background:linear-gradient(135deg,#16a34a,#22c55e)}.blue{background:linear-gradient(135deg,#0284c7,#38bdf8)}.orange{background:linear-gradient(135deg,#ea580c,#fb923c)}.purple{background:linear-gradient(135deg,#7e22ce,#c084fc)}.red{background:linear-gradient(135deg,#b91c1c,#ef4444)}
input{width:100%;accent-color:var(--cyan)}.row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.badge{display:inline-block;padding:6px 10px;border-radius:999px;background:#12233a;border:1px solid #2b4564;color:#c7e8ff}
p{color:var(--muted)} b{color:white}
@media(max-width:800px){.grid{grid-template-columns:1fr}.big{font-size:32px}}
</style>
</head>
<body>
<header>
  <div class="title">🤖 ELIO Robot Dashboard</div>
  <div class="sub">Motor ESP32 + MQTT OLED Emotion Control</div>
</header>
<div class="grid">
  <div class="card">
    <h2>Robot State</h2>
    <div id="state" class="big">STOP</div>
    <h3 id="status" class="warn">READY</h3>
    <p>Mode: <b id="controlMode">MANUAL</b></p>
    <p>Pi Command: <b id="picmd">STOP</b></p>
    <p>Face: <b id="face">NO FACE</b></p>
    <p>MQTT IP: <b id="mqttip">...</b></p>
    <p>WiFi: <span class="badge" id="wifiName">...</span></p>
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
    <button class="blue" onclick="setDance(4)">Dance 4 Swing</button>
    <button class="purple" onclick="setDance(5)">Dance 5 Spin</button>
    <button class="orange" onclick="setDance(6)">Dance 6 Party</button>
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
    <h2><span id="turnSpeedValue">230</span> / 255</h2>
    <input type="range" min="120" max="255" value="230" id="turnSpeedSlider">
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

function updateData(){
fetch('/data').then(r=>r.json()).then(d=>{
  state.innerHTML=d.state;
  status.innerHTML=d.status;
  front.innerHTML=d.front;
  left.innerHTML=d.left;
  right.innerHTML=d.right;
  tcrt.innerHTML=d.tcrt;
  speedValue.innerHTML=d.speed;
  speedSlider.value=d.speed;
  faceSpeedValue.innerHTML=d.faceSpeed;
  faceSpeedSlider.value=d.faceSpeed;
  turnSpeedValue.innerHTML=d.turnSpeed;
  turnSpeedSlider.value=d.turnSpeed;
  picmd.innerHTML=d.picmd;
  face.innerHTML=d.face;
  controlMode.innerHTML=modeName(d.controlMode);
  mqttip.innerHTML=d.mqttIP;
  wifiName.innerHTML=d.wifi;
  edgeStatus.innerHTML=d.edge ? 'EDGE DETECTED' : 'SAFE';
  edgeStatus.className=d.edge?'bad':'ok';
}).catch(e=>{ status.innerHTML='WEB DATA ERROR'; });
}
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
  json += "\"mqttIP\":\"" + mqttIP.toString() + "\",";
  json += "\"wifi\":\"" + WiFi.SSID() + "\",";
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
    turnSpeed = constrain(server.arg("value").toInt(), 120, 255);
    sensorStatus = "FACE SEARCH SPEED UPDATED: " + String(turnSpeed);
    publishStatus(sensorStatus);
  }
  server.send(200, "text/plain", "Face Search Speed Updated");
}

void handleDance() {
  if (server.hasArg("mode")) {
    startDance(server.arg("mode").toInt());
  }
  server.send(200, "text/plain", "Dance Mode Updated");
}

void handleMode() {
  if (server.hasArg("value")) {
    int requestedMode = server.arg("value").toInt();
    if (requestedMode == 1) enterFaceMode();
    else if (requestedMode == 2) enterManualMode();
  }

  server.send(200, "text/plain", "Mode Updated");
}

void handleManual() {
  if (server.hasArg("move")) {
    handleManualCommand(server.arg("move"));
  }

  server.send(200, "text/plain", "Manual Command OK");
}

void handlePiCmd() {
  if (server.hasArg("move")) {
    handleFaceCommand(server.arg("move"));
  }

  server.send(200, "text/plain", "OK " + piCommand);
}

// ================= EMOTION CONTROL =================
void updateEmotion() {
  // OLED emotion priority:
  // 1) Edge/Fall danger -> EMERGENCY
  // 2) Front obstacle   -> SAD
  // 3) Normal state     -> HAPPY
  // Dance modes do NOT change emotion.

  if (edgeDetected()) {
    publishEmotion("EMERGENCY");
  }
  else if (obstacleFront()) {
    publishEmotion("SAD");
  }
  else {
    publishEmotion("HAPPY");
  }
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
  controlMode = 2;
  manualCommand = "STOP";
  piCommand = "STOP";
  faceStatus = "FACE IGNORED - MANUAL MODE";

  WiFi.mode(WIFI_STA);

  if (!connectToWiFi(WIFI_SSID_1, WIFI_PASS_1, 15000)) {
    connectToWiFi(WIFI_SSID_2, WIFI_PASS_2, 15000);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Both WiFi networks failed. Robot stopped.");
    stopRobot();
    return;
  }

  if (!MDNS.begin(ESP32_MDNS_HOST)) {
    Serial.println("ESP32 mDNS failed");
  } else {
    Serial.print("ESP32 mDNS started: ");
    Serial.print(ESP32_MDNS_HOST);
    Serial.println(".local");
  }

  Serial.print("Searching MQTT broker: ");
  Serial.print(MQTT_MDNS_HOST);
  Serial.println(".local");

  resolveMqttBroker();

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

  if (WiFi.status() != WL_CONNECTED) {
    stopRobot();
    return;
  }

  if (mqttIP == IPAddress(0, 0, 0, 0)) {
    if (millis() - lastMdnsTry > 3000) {
      lastMdnsTry = millis();
      Serial.print("[mDNS] Retrying ");
      Serial.print(MQTT_MDNS_HOST);
      Serial.println(".local");
      resolveMqttBroker();
    }

    readSensors();
    return;
  }

  if (!mqttClient.connected()) {
    reconnectMqtt();
  }

  mqttClient.loop();

  readSensors();
  updateEmotion();

  unsigned long now = millis();

  if (now - lastMqttSensorPublish > 1000) {
    lastMqttSensorPublish = now;
    publishSensors();
  }

  if (edgeDetected()) {
    stopRobot();
    danceMode = 0;
    noTone(BUZZER_PIN);
    sensorStatus = "EDGE / FALL DANGER - STOP";
    publishStatus(sensorStatus);
    return;
  }

  if (obstacleFront()) {
    bool tryingForwardManual = (controlMode == 2 && manualCommand == "FORWARD");
    bool tryingFaceOrDance = (controlMode == 1 || controlMode == 3 || danceMode != 0);

    if (tryingForwardManual || tryingFaceOrDance) {
      stopRobot();
      danceMode = 0;
      noTone(BUZZER_PIN);
      sensorStatus = "FRONT OBSTACLE - STOP";
      publishStatus(sensorStatus);
      return;
    }
  }

  if (controlMode == 3 && danceMode != 0) {
    ledcWrite(ENA1, danceSpeed);
    ledcWrite(ENB1, danceSpeed);
    ledcWrite(ENA2, danceSpeed);
    ledcWrite(ENB2, danceSpeed);
    runDanceMode();
    return;
  }

  if (controlMode == 2) {
    noTone(BUZZER_PIN);
    piCommand = "STOP";       // Manual mode NEVER uses face FORWARD
    lastPiCommandTime = 0;
    faceStatus = "FACE IGNORED - MANUAL MODE";
    runCommand(manualCommand, "MANUAL");
    return;
  }

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
}