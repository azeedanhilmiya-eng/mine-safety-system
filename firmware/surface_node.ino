#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>

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
const unsigned long GSM_RETRY_MS = 30000UL;
const unsigned long SMS_COOLDOWN_MS = 30000UL;
const unsigned long BUZZER_ON_MS = 300UL;
const unsigned long BUZZER_OFF_MS = 300UL;
const uint8_t LORA_SYNC_WORD = 0xA3;
const unsigned long EVENT_HOLD_MS = 30000UL;   // 语音事件报警保持时长
const unsigned long NODE_ONLINE_MS = 60000UL;  // 多久没收到包算离线

HardwareSerial sim800(1);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
NetworkClientSecure wifiClient;
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

  // ---- 语音 / 转写文本事件 ----
  String lastEvent;          // 最近事件的中文说明
  bool eventAlarm;           // 该事件是否属于紧急报警
  unsigned long lastEventMs; // 事件时刻（用于报警保持/自动解除）
  unsigned long lastSeenMs;  // 最近收到任意报文的时刻（判断在线）
};

MineState mines[2] = {
  {"M1", 0, 0, 0, 0, -120, false, 0, 0, "", false, 0, 0},
  {"M2", 0, 0, 0, 0, -120, false, 0, 0, "", false, 0, 0}
};

// 长文本分片重组缓冲：TXT,<节点>,<总片数>,<第几片>,<内容>,<序号>
struct TextAssembly {
  String text;
  uint8_t total;
  uint8_t received;
};
TextAssembly textRx[2] = {{"", 0, 0}, {"", 0, 0}};

bool wifiConnected = false;
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
  handleBuzzer();

  // 语音报警保持 30 秒后自动解除（避免蜂鸣器一直响）
  for (int i = 0; i < 2; ++i) {
    if (mines[i].eventAlarm && millis() - mines[i].lastEventMs >= EVENT_HOLD_MS) {
      mines[i].eventAlarm = false;
      mines[i].lastEvent = "";
      if (mines[i].flags == 0) {
        mines[i].inAlert = false;
      }
      Serial.printf("[EVENT] %s alarm auto-cleared\n", mines[i].nodeId.c_str());
      drawReadyScreen();
    }
  }

  if (!wifiConnected && millis() % 10000 < 50) {
    connectWiFi();
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
  // 没填 WiFi 就整体跳过：否则每次都会阻塞 20 秒，期间完全收不到 LoRa
  String ssid = String(WIFI_SSID);
  if (ssid.length() == 0 || ssid.startsWith("YOUR_")) {
    wifiConnected = false;
    return;
  }

  Serial.println("[WIFI] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
    handleLoRaPackets();   // 等 WiFi 的这段时间继续收 LoRa，避免丢包
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
  // 节点1：离线语音事件      VOICE,M1,HELP,<序号>
  if (packet.startsWith("VOICE,")) { handleVoicePacket(packet, rssi); return; }
  // 节点2：离线转写文本      TXT,M2,<总片数>,<第几片>,<内容>,<序号>
  if (packet.startsWith("TXT,"))   { handleTextPacket(packet, rssi);  return; }
  // 任一节点心跳             STATUS,M1,KWS_READY,<序号>
  if (packet.startsWith("STATUS,")){ handleStatusPacket(packet, rssi);return; }

  // 兼容旧格式： M1,<mq4>,<mq7>,<water>,<flags>
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
  mine.lastSeenMs = millis();

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
    if (!anyAlert) {
      drawReadyScreen();
    }
    if (wifiConnected) {
      uploadStatusToFirebase(mineIndex);
    }
  }
}

// =============================================================
//  语音事件 / 离线转写文本
// =============================================================
static int utf8CharLen(uint8_t c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 按像素宽度折行绘制 UTF-8 文本（中英混排），最多 maxLines 行
static void drawWrappedCN(int x, int y, const String &text, int maxWidth, int maxLines, int lineHeight) {
  String line = "";
  int lineNo = 0;
  for (int i = 0; i < (int)text.length() && lineNo < maxLines; ) {
    int len = utf8CharLen((uint8_t)text[i]);
    if (i + len > (int)text.length()) len = 1;
    String ch = text.substring(i, i + len);
    i += len;
    String candidate = line + ch;
    if (line.length() > 0 && u8g2.getUTF8Width(candidate.c_str()) > maxWidth) {
      u8g2.drawUTF8(x, y + lineNo * lineHeight, line.c_str());
      lineNo++;
      line = ch;
    } else {
      line = candidate;
    }
  }
  if (lineNo < maxLines && line.length() > 0) {
    u8g2.drawUTF8(x, y + lineNo * lineHeight, line.c_str());
  }
}

// 事件码 → 是否报警
static bool eventIsAlarm(const String &code) {
  return !(code == "NORMAL" || code == "OK" || code == "ACK" ||
           code == "RX" || code == "TEST" || code == "KWS_READY");
}

// 事件码 → 中文
static String eventToChinese(const String &code) {
  if (code == "HELP") return "紧急救命";
  if (code == "RESCUE") return "请求救援";
  if (code == "TRAPPED") return "有人被困";
  if (code == "EVACUATE") return "撤离";
  if (code == "GAS") return "瓦斯超限";
  if (code == "WATER") return "透水";
  if (code == "COLLAPSE") return "塌方";
  if (code == "FIRE") return "起火";
  if (code == "NORMAL") return "一切正常";
  if (code == "ACK") return "收到";
  if (code == "TEST") return "通话测试";
  if (code == "KWS_READY") return "语音就绪";
  return code;   // 未登记的事件码原样显示
}

// 自由转写文本：出现这些词就按报警处理
static bool textHasAlarmWord(const String &t) {
  return t.indexOf("救命") >= 0 || t.indexOf("救援") >= 0 || t.indexOf("被困") >= 0 ||
         t.indexOf("故障") >= 0 || t.indexOf("超限") >= 0 || t.indexOf("漏水") >= 0 ||
         t.indexOf("断电") >= 0 || t.indexOf("塌方") >= 0 || t.indexOf("起火") >= 0 ||
         t.indexOf("紧急") >= 0 || t.indexOf("危险") >= 0 || t.indexOf("撤离") >= 0;
}

void drawEventScreen(int index, const String &title, const String &text, int rssi) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(0, 12, title.c_str());
  String info = mines[index].nodeId + " " + String(rssi) + "dBm";
  u8g2.drawUTF8(0, 26, info.c_str());
  drawWrappedCN(0, 42, text, 126, 2, 12);
  u8g2.sendBuffer();
}

void reportEvent(int index, const String &chinese, bool alarm, int rssi) {
  MineState &mine = mines[index];
  mine.lastEvent = chinese;
  mine.eventAlarm = alarm;
  mine.lastEventMs = millis();
  mine.lastSeenMs = millis();
  mine.rssi = rssi;

  Serial.printf("[EVENT] %s %s alarm=%d rssi=%d\n",
                mine.nodeId.c_str(), chinese.c_str(), alarm ? 1 : 0, rssi);

  if (alarm) {
    mine.inAlert = true;
    drawEventScreen(index, "!! 语音报警 !!", chinese, rssi);
    if (gsmReady && millis() - mine.lastSmsMs >= SMS_COOLDOWN_MS) {
      sendSmsEvent(index, chinese);
      mine.lastSmsMs = millis();
    }
  } else {
    drawEventScreen(index, "语音信息", chinese, rssi);
  }

  if (wifiConnected) {
    uploadEventToFirebase(index, chinese, alarm);
  }
}

// VOICE,<节点>,<事件码>,<序号>
void handleVoicePacket(const String &packet, int rssi) {
  int i1 = packet.indexOf(',');
  int i2 = packet.indexOf(',', i1 + 1);
  if (i1 < 0 || i2 < 0) return;

  String nodeId = packet.substring(i1 + 1, i2);
  nodeId.trim();
  int idx = (nodeId == "M1") ? 0 : (nodeId == "M2") ? 1 : -1;
  if (idx < 0) return;

  int i3 = packet.indexOf(',', i2 + 1);
  String code = (i3 < 0) ? packet.substring(i2 + 1) : packet.substring(i2 + 1, i3);
  code.trim();
  reportEvent(idx, eventToChinese(code), eventIsAlarm(code), rssi);
}

// STATUS,<节点>,<状态>,<序号> —— 心跳只更新在线时刻，不刷屏
void handleStatusPacket(const String &packet, int rssi) {
  int i1 = packet.indexOf(',');
  if (i1 < 0) return;
  int i2 = packet.indexOf(',', i1 + 1);
  String nodeId = (i2 < 0) ? packet.substring(i1 + 1) : packet.substring(i1 + 1, i2);
  nodeId.trim();
  int idx = (nodeId == "M1") ? 0 : (nodeId == "M2") ? 1 : -1;
  if (idx < 0) return;
  mines[idx].lastSeenMs = millis();
  mines[idx].rssi = rssi;
  Serial.printf("[HEARTBEAT] %s rssi=%d\n", mines[idx].nodeId.c_str(), rssi);
}

// TXT,<节点>,<总片数>,<第几片>,<内容>,<序号>
void handleTextPacket(const String &packet, int rssi) {
  int i1 = packet.indexOf(',');
  int i2 = packet.indexOf(',', i1 + 1);
  int i3 = packet.indexOf(',', i2 + 1);
  int i4 = packet.indexOf(',', i3 + 1);
  if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0) return;

  String nodeId = packet.substring(i1 + 1, i2);
  nodeId.trim();
  int idx = (nodeId == "M1") ? 0 : (nodeId == "M2") ? 1 : -1;
  if (idx < 0) return;

  uint8_t total = (uint8_t)packet.substring(i2 + 1, i3).toInt();
  uint8_t part = (uint8_t)packet.substring(i3 + 1, i4).toInt();
  String body = packet.substring(i4 + 1);

  mines[idx].lastSeenMs = millis();
  if (total == 0 || part == 0) return;
  if (part == 1) { textRx[idx].text = ""; textRx[idx].received = 0; }
  textRx[idx].total = total;
  textRx[idx].text += body;
  textRx[idx].received++;

  Serial.printf("[TXT] %s %u/%u: %s\n", nodeId.c_str(), part, total, body.c_str());

  if (textRx[idx].received >= total) {
    String full = textRx[idx].text;
    textRx[idx].text = "";
    textRx[idx].received = 0;
    reportEvent(idx, full, textHasAlarmWord(full), rssi);
  }
}

// 去掉 JSON 里会破坏格式的字符
static String jsonEscape(const String &s) {
  String out = "";
  for (int i = 0; i < (int)s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') continue;
    out += c;
  }
  return out;
}

bool uploadEventToFirebase(int index, const String &eventText, bool alarm) {
  String node = mines[index].nodeId;
  node.toLowerCase();
  String payload = "{\"nodeId\":\"" + mines[index].nodeId + "\",";
  payload += "\"event\":\"" + jsonEscape(eventText) + "\",";
  payload += "\"alarm\":" + String(alarm ? "true" : "false") + ",";
  payload += "\"rssi\":" + String(mines[index].rssi) + ",";
  payload += "\"updatedAt\":" + String(millis()) + "}";

  String latestUrl = String(FIREBASE_DB_URL) + "/alerts/" + node + "/latest.json";
  http.begin(wifiClient, latestUrl);
  http.addHeader("Content-Type", "application/json");
  int ok1 = http.PUT(payload);
  http.end();

  String historyUrl = String(FIREBASE_DB_URL) + "/alerts/" + node + "/history.json";
  http.begin(wifiClient, historyUrl);
  http.addHeader("Content-Type", "application/json");
  int ok2 = http.POST(payload);
  http.end();

  Serial.printf("[FB] Event %s latest=%d history=%d\n", node.c_str(), ok1, ok2);
  return (ok1 == HTTP_CODE_OK || ok1 == HTTP_CODE_NO_CONTENT);
}

// 语音/文本事件短信
void sendSmsEvent(int index, const String &eventText) {
  const MineState &mine = mines[index];
  String body = "MINE SAFETY ALERT!\n";
  body += "Node " + mine.nodeId + ": " + eventText + "\n";
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
      Serial.println("[SMS] Event sent");
      return;
    }
    if (response.indexOf("ERROR") >= 0) {
      Serial.println("[SMS] Event failed");
      return;
    }
    delay(50);
  }
  Serial.println("[SMS] Event timeout");
}

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
  bool activeAlert = mines[0].inAlert || mines[1].inAlert;
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
  const bool online1 = (mines[0].lastSeenMs > 0) && (millis() - mines[0].lastSeenMs < NODE_ONLINE_MS);
  const bool online2 = (mines[1].lastSeenMs > 0) && (millis() - mines[1].lastSeenMs < NODE_ONLINE_MS);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(0, 12, "矿安监测网关");
  u8g2.drawUTF8(0, 26, (String("节点1 ") + (online1 ? "在线" : "离线")).c_str());
  u8g2.drawUTF8(0, 40, (String("节点2 ") + (online2 ? "在线" : "离线")).c_str());
  String bottom = String("WiFi") + (wifiConnected ? "通" : "断") +
                  " GSM" + (gsmReady ? "通" : "断");
  u8g2.drawUTF8(0, 54, bottom.c_str());
  u8g2.sendBuffer();
}

void drawWiFiStatus(bool connected) {
  // 有事件画面时不要顶掉它（否则 WiFi/GSM 状态刷新会把报警盖掉）
  if (!mines[0].eventAlarm && !mines[1].eventAlarm) {
    drawReadyScreen();
  }
}

void drawGsmStatus(bool ready) {
  if (!mines[0].eventAlarm && !mines[1].eventAlarm) {
    drawReadyScreen();
  }
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
