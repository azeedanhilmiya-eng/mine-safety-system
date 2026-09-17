#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>

// Shared with the voice node: one definition of the packet format and one of
// the command-id -> text mapping. Both are header-only, so including them here
// needs no extra build setup.
#include "voice_node/kws_config.h"
#include "voice_node/kws_packet.h"

// Surface Node pins
const int LORA_SS = 10;
const int LORA_MOSI = 11;
const int LORA_SCK = 12;
const int LORA_MISO = 13;
const int LORA_RST = 14;
const int LORA_DIO0 = 9;

const int GSM_RX = 47;
const int GSM_TX = 48;
const int GSM_RST = 19;
const int OLED_SDA = 5;
const int OLED_SCL = 6;
const int BUZZER_PIN = 8;

const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
const char FIREBASE_DB_URL[] = "https://YOUR_PROJECT_ID-default-rtdb.firebaseio.com";
const char ALERT_PHONE_NUMBER[] = "+94716643413";

const unsigned long WIFI_TIMEOUT_MS = 20000UL;
// connectWiFi() blocks for up to WIFI_TIMEOUT_MS. Retrying every 10 s with no
// network present left the gateway blocked more often than it was listening,
// which is exactly the condition the offline demo runs in. Retry rarely, and
// never while an alert is on screen.
const unsigned long WIFI_RETRY_MS = 30000UL;
const unsigned long GSM_RETRY_MS = 30000UL;
const unsigned long SMS_COOLDOWN_MS = 30000UL;
const unsigned long BUZZER_ON_MS = 300UL;
const unsigned long BUZZER_OFF_MS = 300UL;
const uint8_t LORA_SYNC_WORD = 0xA3;

// How long a voice alert holds the screen and the buzzer before the gateway
// goes back to its normal display.
const unsigned long VOICE_ALERT_MS = 15000UL;
// The node retransmits when an ack goes missing, so the same seq arrives
// twice. Inside this window it is a retransmission and must be acked again
// but not alarmed on twice; outside it, a repeated seq is a genuine second
// call (seq wraps at 255, and a node reboot restarts it).
const unsigned long VOICE_DEDUP_MS = 10000UL;
const unsigned long VOICE_SMS_COOLDOWN_MS = 60000UL;

// Chinese on the OLED needs a CJK font. The gateway has flash to spare, but
// the font name has to match the u8g2 build, so this ships off and is a
// one-line change for the demo:
//   set to 1, and pick a font your u8g2 actually provides, e.g.
//   u8g2_font_wqy12_t_gb2312 or u8g2_font_unifont_t_chinese2.
#define VOICE_OLED_CHINESE 0
#if VOICE_OLED_CHINESE
#define VOICE_LABEL_FONT u8g2_font_wqy12_t_gb2312
#define VOICE_LABEL_TABLE kKwsLabelsZh
#else
#define VOICE_LABEL_FONT u8g2_font_6x10_tf
#define VOICE_LABEL_TABLE kKwsLabels
#endif

HardwareSerial sim800(1);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
WiFiClientSecure wifiClient;
HTTPClient http;

struct MineState {
  String nodeId;
  uint16_t mq4;
  uint16_t mq7;
  uint16_t water;
  int flags;
  int rssi;
  bool inAlert;
  unsigned long lastSmsMs;
  unsigned long lastUpdateMs;
};

MineState mines[2] = {
  {"M1", 0, 0, 0, 0, -120, false, 0, 0},
  {"M2", 0, 0, 0, 0, -120, false, 0, 0}
};

// Voice events are kept apart from MineState on purpose. Folding them into
// `flags` / `inAlert` would make a shout for help look like a gas reading to
// the SMS text, the dashboard and the app.
struct VoiceState {
  uint8_t cmd;
  uint8_t conf;
  uint8_t seq;
  int rssi;
  unsigned long receivedMs;
  unsigned long lastSmsMs;
  bool seqSeen;
  unsigned long lastSeqMs;
};

VoiceState voices[2] = {
  {0, 0, 0, -120, 0, 0, false, 0},
  {0, 0, 0, -120, 0, 0, false, 0}
};

int voiceAlertIndex = -1;
unsigned long voiceAlertUntilMs = 0;

bool wifiConnected = false;
unsigned long nextWifiAttemptMs = 0;
bool gsmReady = false;
bool buzzerState = false;
unsigned long nextBuzzerChangeMs = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) ;

  pinMode(GSM_RST, OUTPUT);
  digitalWrite(GSM_RST, HIGH);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.setPins(OLED_SDA, OLED_SCL);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);

  drawSplashScreen();
  connectWiFi();
  initializeGSM();
  initializeLoRa();
  drawReadyScreen();
}

void loop() {
  handleLoRaPackets();
  handleVoiceAlert();
  handleBuzzer();
  if (!wifiConnected && !voiceAlertActive() && millis() >= nextWifiAttemptMs) {
    connectWiFi();
    nextWifiAttemptMs = millis() + WIFI_RETRY_MS;
  }
  if (!gsmReady && millis() - mines[0].lastSmsMs > GSM_RETRY_MS) {
    initializeGSM();
  }
}

void initializeLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("[ERROR] LoRa init failed");
    return;
  }
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  Serial.println("[OK] LoRa initialized");
}

void connectWiFi() {
  Serial.println("[WIFI] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    wifiClient.setInsecure();
  }
  Serial.printf("[WIFI] %s\n", wifiConnected ? "Connected" : "Failed");
  drawWiFiStatus(wifiConnected);
}

void initializeGSM() {
  Serial.println("[GSM] Resetting module...");
  digitalWrite(GSM_RST, LOW);
  delay(200);
  digitalWrite(GSM_RST, HIGH);
  delay(1500);

  sim800.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
  delay(2000);

  if (sendAT("AT", "OK", 5000) && sendAT("AT+CPIN?", "READY", 8000) && sendAT("AT+CMGF=1", "OK", 5000)) {
    gsmReady = true;
    Serial.println("[GSM] Ready");
  } else {
    gsmReady = false;
    Serial.println("[GSM] Initialization failed");
  }
  drawGsmStatus(gsmReady);
}

bool sendAT(const String &command, const String &expected, unsigned long timeoutMs) {
  flushSerial(sim800);
  sim800.print(command);
  sim800.print("\r");
  unsigned long start = millis();
  String response;
  while (millis() - start < timeoutMs) {
    while (sim800.available()) {
      response += char(sim800.read());
    }
    if (response.indexOf(expected) >= 0) {
      Serial.printf("[GSM] %s => %s\n", command.c_str(), expected.c_str());
      return true;
    }
    delay(50);
  }
  Serial.printf("[GSM] %s failed, response: %s\n", command.c_str(), response.c_str());
  return false;
}

void flushSerial(Stream &serial) {
  while (serial.available()) {
    serial.read();
  }
}

void handleLoRaPackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String packet;
  while (LoRa.available()) {
    packet += char(LoRa.read());
  }

  int rssi = LoRa.packetRssi();
  Serial.printf("[LORA] Received: %s | RSSI=%d\n", packet.c_str(), rssi);
  processPacket(packet, rssi);
}

void processPacket(const String &packet, int rssi) {
  // Voice events first. The sensor parse below reads fields by position, so a
  // voice packet falling through it would either be dropped as an unknown
  // node id or, worse, have its sequence number read as a hazard flag and
  // raise an alarm nobody called in.
  if (packet.startsWith(KWS_PACKET_PREFIX)) {
    processVoiceEvent(packet, rssi);
    return;
  }

  int idx1 = packet.indexOf(',');
  if (idx1 < 0) return;

  String nodeId = packet.substring(0, idx1);
  int mineIndex = (nodeId == "M1") ? 0 : (nodeId == "M2") ? 1 : -1;
  if (mineIndex < 0) return;

  int idx2 = packet.indexOf(',', idx1 + 1);
  int idx3 = packet.indexOf(',', idx2 + 1);
  int idx4 = packet.indexOf(',', idx3 + 1);
  if (idx2 < 0 || idx3 < 0 || idx4 < 0) return;

  uint16_t mq4 = packet.substring(idx1 + 1, idx2).toInt();
  uint16_t mq7 = packet.substring(idx2 + 1, idx3).toInt();
  uint16_t water = packet.substring(idx3 + 1, idx4).toInt();
  int flags = packet.substring(idx4 + 1).toInt();

  MineState &mine = mines[mineIndex];
  mine.mq4 = mq4;
  mine.mq7 = mq7;
  mine.water = water;
  mine.flags = flags;
  mine.rssi = rssi;
  mine.inAlert = (flags != 0);
  mine.lastUpdateMs = millis();

  if (mine.inAlert) {
    drawAlertScreen(mineIndex);
    if (gsmReady && millis() - mine.lastSmsMs >= SMS_COOLDOWN_MS) {
      sendSmsAlert(mineIndex);
      mine.lastSmsMs = millis();
    }
    if (wifiConnected) {
      uploadStatusToFirebase(mineIndex);
      uploadAlertToFirebase(mineIndex);
    }
  } else {
    bool anyAlert = mines[0].inAlert || mines[1].inAlert;
    if (!anyAlert && !voiceAlertActive()) {
      drawReadyScreen();
    }
    if (wifiConnected) {
      uploadStatusToFirebase(mineIndex);
    }
  }
}

// ============================ voice events ================================

bool voiceAlertActive() {
  return voiceAlertIndex >= 0 && millis() < voiceAlertUntilMs;
}

int mineIndexForNode(const char *nodeId) {
  for (int i = 0; i < 2; i++) {
    if (mines[i].nodeId.equals(nodeId)) return i;
  }
  return -1;
}

void sendVoiceAck(const char *nodeId, uint8_t seq) {
  char ack[24];
  if (kws_ack_format(ack, sizeof(ack), nodeId, seq) == 0) {
    Serial.println("[VOICE] ack buffer too small");
    return;
  }
  LoRa.beginPacket();
  LoRa.print(ack);
  LoRa.endPacket();
  Serial.printf("[VOICE] ACK %s\n", ack);
  // handleLoRaPackets() calls parsePacket() on the next pass, which puts the
  // radio back into receive; nothing else is needed here.
}

void processVoiceEvent(const String &packet, int rssi) {
  KwsPacket pkt;
  KwsPacketStatus status = kws_packet_parse(packet.c_str(), &pkt);
  if (status != KWS_PKT_OK) {
    Serial.printf("[VOICE] rejected (%s): %s\n",
                  kws_packet_status_text(status), packet.c_str());
    return;
  }

  const int index = mineIndexForNode(pkt.node);
  if (index < 0) {
    Serial.printf("[VOICE] unknown node '%s'\n", pkt.node);
    return;
  }

  // Acknowledge before doing anything slow. The node waits 900 ms and then
  // retransmits; drawing the screen, sending an SMS and posting to Firebase
  // together take far longer than that, and a late ack means every event goes
  // out twice.
  sendVoiceAck(pkt.node, pkt.seq);

  VoiceState &voice = voices[index];
  const unsigned long now = millis();
  const bool duplicate = voice.seqSeen && voice.seq == pkt.seq &&
                         (now - voice.lastSeqMs) < VOICE_DEDUP_MS;

  voice.seq = pkt.seq;
  voice.seqSeen = true;
  voice.lastSeqMs = now;

  if (duplicate) {
    Serial.printf("[VOICE] retransmission of seq=%u, acked and ignored\n",
                  (unsigned)pkt.seq);
    return;
  }

  if (!kws_is_command(pkt.cmd)) {
    // silence / unknown should never leave the node at all; if one arrives,
    // say so rather than alarming on it.
    Serial.printf("[VOICE] non-command id %u ignored\n", (unsigned)pkt.cmd);
    return;
  }

  voice.cmd = pkt.cmd;
  voice.conf = pkt.conf;
  voice.rssi = rssi;
  voice.receivedMs = now;

  voiceAlertIndex = index;
  voiceAlertUntilMs = now + VOICE_ALERT_MS;

  Serial.printf("[VOICE] %s said '%s' (%u%%) seq=%u rssi=%d\n",
                mines[index].nodeId.c_str(), kKwsLabels[pkt.cmd],
                (unsigned)pkt.conf, (unsigned)pkt.seq, rssi);

  drawVoiceScreen(index);

  if (gsmReady && now - voice.lastSmsMs >= VOICE_SMS_COOLDOWN_MS) {
    sendVoiceSms(index);
    voice.lastSmsMs = millis();
  }
  if (wifiConnected) {
    uploadVoiceEventToFirebase(index);
  }
}

void handleVoiceAlert() {
  if (voiceAlertIndex < 0) return;
  if (millis() < voiceAlertUntilMs) return;

  voiceAlertIndex = -1;
  // Hazard readings outrank a finished voice alert for the screen.
  if (mines[0].inAlert) {
    drawAlertScreen(0);
  } else if (mines[1].inAlert) {
    drawAlertScreen(1);
  } else {
    drawReadyScreen();
  }
}

void drawVoiceScreen(int index) {
  const VoiceState &voice = voices[index];
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "! VOICE ALERT !");
  u8g2.drawStr(0, 24, ("Node: " + mines[index].nodeId).c_str());

  // drawUTF8 handles both the ASCII font and a CJK one, so the only thing
  // VOICE_OLED_CHINESE changes is which font and which table are used.
  u8g2.setFont(VOICE_LABEL_FONT);
  u8g2.drawUTF8(0, 40, VOICE_LABEL_TABLE[voice.cmd]);

  u8g2.setFont(u8g2_font_6x10_tf);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "conf %u%%  rssi %d", (unsigned)voice.conf,
           voice.rssi);
  u8g2.drawStr(0, 56, buffer);
  u8g2.sendBuffer();
}

void sendVoiceSms(int index) {
  const VoiceState &voice = voices[index];
  String body = "MINE VOICE ALERT!\n";
  body += "Mine ";
  body += mines[index].nodeId.substring(1);
  body += ": miner said \"";
  body += kKwsLabels[voice.cmd];
  body += "\"\n";
  body += "Confidence " + String(voice.conf) + "%\n";
  body += "RSSI=" + String(voice.rssi) + " dBm\n";
  body += "Act immediately!";

  if (!sendAT("AT+CMGF=1", "OK", 5000)) return;
  if (!sendAT("AT+CMGS=\"" + String(ALERT_PHONE_NUMBER) + "\"", ">", 8000)) return;

  flushSerial(sim800);
  sim800.print(body);
  sim800.write(26); // CTRL+Z

  unsigned long start = millis();
  String response;
  while (millis() - start < 10000) {
    while (sim800.available()) {
      response += char(sim800.read());
    }
    if (response.indexOf("+CMGS") >= 0 || response.indexOf("OK") >= 0) {
      Serial.println("[VOICE-SMS] Sent successfully");
      return;
    }
    if (response.indexOf("ERROR") >= 0) {
      Serial.printf("[VOICE-SMS] Failed: %s\n", response.c_str());
      return;
    }
    delay(50);
  }
  Serial.println("[VOICE-SMS] Timeout waiting for send confirmation");
}

String buildVoiceJsonPayload(int index) {
  const VoiceState &voice = voices[index];
  String payload = "{";
  payload += "\"nodeId\":\"" + mines[index].nodeId + "\",";
  payload += "\"command\":" + String(voice.cmd) + ",";
  payload += "\"label\":\"" + String(kKwsLabels[voice.cmd]) + "\",";
  payload += "\"labelZh\":\"" + String(kKwsLabelsZh[voice.cmd]) + "\",";
  payload += "\"confidence\":" + String(voice.conf) + ",";
  payload += "\"seq\":" + String(voice.seq) + ",";
  payload += "\"rssi\":" + String(voice.rssi) + ",";
  payload += "\"receivedAt\":" + String(voice.receivedMs);
  payload += "}";
  return payload;
}

bool uploadVoiceEventToFirebase(int index) {
  if (!wifiConnected) return false;
  String node = mines[index].nodeId;
  node.toLowerCase();

  // A separate path from /alerts: the dashboard and the app read that one as
  // sensor hazards, and voice events posted there would show up as gas or
  // flooding readings that were never measured.
  String latestUrl = String(FIREBASE_DB_URL) + "/voice_events/" + node + "/latest.json";
  String historyUrl = String(FIREBASE_DB_URL) + "/voice_events/" + node + "/history.json";
  String payload = buildVoiceJsonPayload(index);

  http.begin(wifiClient, latestUrl);
  http.addHeader("Content-Type", "application/json");
  int ok1 = http.PUT(payload);
  http.end();

  http.begin(wifiClient, historyUrl);
  http.addHeader("Content-Type", "application/json");
  int ok2 = http.POST(payload);
  http.end();

  Serial.printf("[FB] Voice event %s latest=%d history=%d\n", node.c_str(), ok1, ok2);
  return (ok1 == HTTP_CODE_OK || ok1 == HTTP_CODE_NO_CONTENT) &&
         (ok2 == HTTP_CODE_OK || ok2 == HTTP_CODE_NO_CONTENT);
}

// ========================== end voice events ==============================

void sendSmsAlert(int index) {
  const MineState &mine = mines[index];
  String body = "MINE SAFETY ALERT!\n";
  body += "Mine ";
  body += mine.nodeId.substring(1);
  body += ":\n";
  if (mine.flags & 0x04) body += "* WATER LEVEL HIGH! Reading: " + String(mine.water) + "\n";
  if (mine.flags & 0x01) body += "* METHANE (CH4) HIGH! Reading: " + String(mine.mq4) + "\n";
  if (mine.flags & 0x02) body += "* CO HIGH! Reading: " + String(mine.mq7) + "\n";
  body += "RSSI=" + String(mine.rssi) + " dBm\n";
  body += "Act immediately!";

  if (!sendAT("AT+CMGF=1", "OK", 5000)) return;
  if (!sendAT("AT+CMGS=\"" + String(ALERT_PHONE_NUMBER) + "\"", ">", 8000)) return;

  flushSerial(sim800);
  sim800.print(body);
  sim800.write(26); // CTRL+Z

  unsigned long start = millis();
  String response;
  while (millis() - start < 10000) {
    while (sim800.available()) {
      response += char(sim800.read());
    }
    if (response.indexOf("+CMGS") >= 0 || response.indexOf("OK") >= 0) {
      Serial.println("[SMS] Sent successfully");
      return;
    }
    if (response.indexOf("ERROR") >= 0) {
      Serial.printf("[SMS] Failed: %s\n", response.c_str());
      return;
    }
    delay(50);
  }
  Serial.println("[SMS] Timeout waiting for send confirmation");
}

void handleBuzzer() {
  bool activeAlert = mines[0].inAlert || mines[1].inAlert || voiceAlertActive();
  if (!activeAlert) {
    buzzerState = false;
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  if (millis() >= nextBuzzerChangeMs) {
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    nextBuzzerChangeMs = millis() + (buzzerState ? BUZZER_ON_MS : BUZZER_OFF_MS);
  }
}

void drawSplashScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(2, 18, "SubterraGuard");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(2, 32, "Mine Safety Gateway");
  u8g2.drawStr(2, 48, "Initializing...");
  u8g2.sendBuffer();
}

void drawReadyScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "SubterraGuard Ready");
  u8g2.drawStr(0, 26, wifiConnected ? "WiFi: Connected" : "WiFi: Offline");
  u8g2.drawStr(0, 40, gsmReady ? "GSM: Ready" : "GSM: Not ready");
  u8g2.drawStr(0, 54, "Listening for LoRa alerts...");
  u8g2.sendBuffer();
}

// A link-status change is the least important thing on this screen. Redrawing
// unconditionally used to wipe a hazard or voice alert that was still current,
// and the WiFi retry fires on a timer, so an alert could vanish seconds after
// it appeared with nobody having touched anything.
void redrawCurrentScreen() {
  if (voiceAlertActive()) {
    drawVoiceScreen(voiceAlertIndex);
  } else if (mines[0].inAlert) {
    drawAlertScreen(0);
  } else if (mines[1].inAlert) {
    drawAlertScreen(1);
  } else {
    drawReadyScreen();
  }
}

void drawWiFiStatus(bool connected) {
  redrawCurrentScreen();
}

void drawGsmStatus(bool ready) {
  redrawCurrentScreen();
}

void drawAlertScreen(int index) {
  const MineState &mine = mines[index];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "! HAZARD ALERT !");
  u8g2.drawStr(0, 24, ("Mine: " + mine.nodeId).c_str());

  char buffer[32];
  if (mine.flags & 0x01) {
    sprintf(buffer, "CH4 HIGH: %u", mine.mq4);
    u8g2.drawStr(0, 38, buffer);
  }
  if (mine.flags & 0x02) {
    sprintf(buffer, "CO HIGH: %u", mine.mq7);
    u8g2.drawStr(0, 50, buffer);
  }
  if (mine.flags & 0x04) {
    sprintf(buffer, "WATER HIGH: %u", mine.water);
    u8g2.drawStr(0, 62, buffer);
  }
  u8g2.sendBuffer();
}

String buildJsonPayload(int index) {
  MineState &mine = mines[index];
  String payload = "{";
  payload += "\"nodeId\":\"" + mine.nodeId + "\",";
  payload += "\"mq4\":" + String(mine.mq4) + ",";
  payload += "\"mq7\":" + String(mine.mq7) + ",";
  payload += "\"water\":" + String(mine.water) + ",";
  payload += "\"flags\":" + String(mine.flags) + ",";
  payload += "\"rssi\":" + String(mine.rssi) + ",";
  payload += "\"inAlert\":" + String(mine.inAlert ? "true" : "false") + ",";
  payload += "\"updatedAt\":" + String(millis());
  payload += "}";
  return payload;
}

bool uploadStatusToFirebase(int index) {
  if (!wifiConnected) return false;
  String node = mines[index].nodeId;
  node.toLowerCase();
  String url = String(FIREBASE_DB_URL) + "/status/" + node + ".json";
  String payload = buildJsonPayload(index);

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(payload);
  http.end();

  Serial.printf("[FB] Status update %s code=%d\n", node.c_str(), httpCode);
  return httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT;
}

bool uploadAlertToFirebase(int index) {
  if (!wifiConnected) return false;
  String node = mines[index].nodeId;
  node.toLowerCase();
  String latestUrl = String(FIREBASE_DB_URL) + "/alerts/" + node + "/latest.json";
  String historyUrl = String(FIREBASE_DB_URL) + "/alerts/" + node + "/history.json";
  String payload = buildJsonPayload(index);

  http.begin(wifiClient, latestUrl);
  http.addHeader("Content-Type", "application/json");
  int ok1 = http.PUT(payload);
  http.end();

  http.begin(wifiClient, historyUrl);
  http.addHeader("Content-Type", "application/json");
  int ok2 = http.POST(payload);
  http.end();

  Serial.printf("[FB] Alert upload %s latest=%d history=%d\n", node.c_str(), ok1, ok2);
  return (ok1 == HTTP_CODE_OK || ok1 == HTTP_CODE_NO_CONTENT) && (ok2 == HTTP_CODE_OK || ok2 == HTTP_CODE_NO_CONTENT);
}
