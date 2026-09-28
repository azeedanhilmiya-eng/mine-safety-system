#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "onenet_ca.h"
#include "onenet_secrets.h"

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

const char ALERT_PHONE_NUMBER[] = "+94716643413";

const unsigned long WIFI_TIMEOUT_MS = 20000UL;
const unsigned long CLOUD_UPLOAD_INTERVAL_MS = 5000UL;
const unsigned long GSM_RETRY_MS = 30000UL;
const unsigned long SMS_COOLDOWN_MS = 30000UL;
const unsigned long BUZZER_ON_MS = 300UL;
const unsigned long BUZZER_OFF_MS = 300UL;
const uint8_t LORA_SYNC_WORD = 0xA3;
const unsigned long EVENT_HOLD_MS = 30000UL;   // 语音事件报警保持时长
const unsigned long NODE_ONLINE_MS = 15000UL;  // 多久没收到包算离线（节点1 每 5 s、节点2 每 4~6 s 一次心跳）

HardwareSerial sim800(1);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
struct CloudMessage { char json[1536]; };
QueueHandle_t cloudQueue = nullptr;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastCloudSnapshotMs = 0;

void cloudUploadTask(void *parameter);
void queueCloudSnapshot();
void maintainWiFi();

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
  uint32_t lastDoneSeq;      // 最近一条已收齐的消息序号（重复分片只回执、不重复显示）
  unsigned long lastDoneMs;
};
TextAssembly textRx[2] = {{"", 0, 0, 0, 0}, {"", 0, 0, 0, 0}};

bool wifiConnected = false;
bool gsmReady = false;
// 本方案不使用 SIM800L（短信通道取消）：ENABLE_GSM=false 时完全跳过 GSM 初始化，
// 连 AT 超时和模块复位延时都不做，主循环可以专心收 LoRa。
const bool ENABLE_GSM = false;
// 若将来重新启用 SIM800L：连续失败 GSM_MAX_ATTEMPTS 次后停止重试
// （否则 initializeGSM() 每次要阻塞约 9 秒，期间收不到 LoRa 包）
bool gsmGaveUp = false;
uint8_t gsmFailCount = 0;
unsigned long lastGsmAttemptMs = 0;
const uint8_t GSM_MAX_ATTEMPTS = 2;
bool buzzerState = false;
unsigned long nextBuzzerChangeMs = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) ;

  // 版本标识：烧录后看这一行就能确认是不是新版固件（新版支持 VOICE / TXT / STATUS 报文）
  Serial.println();
  Serial.println("[BOOT] SML-Gateway 网关固件 2026-09-24（支持 VOICE / TXT / STATUS）");

  pinMode(GSM_RST, OUTPUT);
  digitalWrite(GSM_RST, HIGH);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.setPins(OLED_SDA, OLED_SCL);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);

  drawSplashScreen();
  initializeLoRa();
  initializeGSM();
  cloudQueue = xQueueCreate(1, sizeof(CloudMessage));
  if (cloudQueue != nullptr) {
    if (xTaskCreatePinnedToCore(cloudUploadTask, "onenet-upload", 12288, nullptr,
                                1, nullptr, 0) != pdPASS) {
      vQueueDelete(cloudQueue);
      cloudQueue = nullptr;
      Serial.println("[OneNET] 无法启动上传任务，本地 LoRa 仍可运行");
    }
  } else {
    Serial.println("[OneNET] 无法创建上报队列，本地 LoRa 仍可运行");
  }
  connectWiFi();
  drawReadyScreen();
}

void loop() {
  handleLoRaPackets();
  handleBuzzer();
  maintainWiFi();
  if (millis() - lastCloudSnapshotMs >= CLOUD_UPLOAD_INTERVAL_MS) {
    lastCloudSnapshotMs = millis();
    queueCloudSnapshot();
  }

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

  if (ENABLE_GSM && !gsmReady && !gsmGaveUp && millis() - lastGsmAttemptMs > GSM_RETRY_MS) {
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
  String ssid = String(WIFI_SSID);
  if (ssid.length() == 0 || ssid.startsWith("YOUR_")) {
    wifiConnected = false;
    Serial.println("[WIFI] 未配置热点，LoRa 与本地显示继续运行");
    return;
  }

  Serial.println("[WIFI] 后台连接，LoRa 接收不等待网络");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
}

void maintainWiFi() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wifiConnected) {
    wifiConnected = connected;
    if (connected) {
      Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
      configTime(0, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
      queueCloudSnapshot();
    } else {
      Serial.println("[WIFI] Disconnected; local display and LoRa continue");
    }
    drawWiFiStatus(connected);
  }
  if (!connected && strlen(WIFI_SSID) > 0 &&
      strncmp(WIFI_SSID, "YOUR_", 5) != 0 &&
      millis() - lastWifiAttemptMs >= WIFI_TIMEOUT_MS) {
    WiFi.reconnect();
    lastWifiAttemptMs = millis();
  }
}

void initializeGSM() {
  if (!ENABLE_GSM || gsmGaveUp) {
    return;
  }
  lastGsmAttemptMs = millis();
  Serial.println("[GSM] Resetting module...");
  digitalWrite(GSM_RST, LOW);
  delay(200);
  digitalWrite(GSM_RST, HIGH);
  delay(1500);

  sim800.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
  delay(2000);

  if (sendAT("AT", "OK", 5000) && sendAT("AT+CPIN?", "READY", 8000) && sendAT("AT+CMGF=1", "OK", 5000)) {
    gsmReady = true;
    gsmFailCount = 0;
    Serial.println("[GSM] Ready");
  } else {
    gsmReady = false;
    gsmFailCount++;
    Serial.printf("[GSM] Initialization failed (%u/%u)\n", gsmFailCount, GSM_MAX_ATTEMPTS);
    if (gsmFailCount >= GSM_MAX_ATTEMPTS) {
      gsmGaveUp = true;
      Serial.println("[GSM] 连续失败，已停止重试：不接 SIM800L 不影响 LoRa 收包与显示；接好模块后重启设备即可");
    }
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
    queueCloudSnapshot();
  } else {
    bool anyAlert = mines[0].inAlert || mines[1].inAlert;
    if (!anyAlert) {
      drawReadyScreen();
    }
    queueCloudSnapshot();
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

  queueCloudSnapshot();
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

// 逐片回执（新协议）：ACK,<节点>,<序号>,<片号>
// 节点2（rpi-node2/node2.py）每片发出后等这条回执，2 秒收不到就重发，最多 3 次。
static void sendAck(const String &nodeId, uint32_t seq, uint8_t part) {
  char buf[40];
  snprintf(buf, sizeof(buf), "ACK,%s,%lu,%u", nodeId.c_str(), (unsigned long)seq, (unsigned)part);
  LoRa.beginPacket();
  LoRa.print(buf);
  const bool ok = (LoRa.endPacket() == 1);
  Serial.printf("[回执] %s %s\n", ok ? "已发送" : "发送失败", buf);
}

// TXT 两种格式都支持：
//   新：TXT,<节点>,<序号>,<片号>/<总片数>,<正文>        （node2.py / protocol.py，正文可含逗号）
//   旧：TXT,<节点>,<总片数>,<片号>,<正文>,<序号>
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

  const String f2 = packet.substring(i2 + 1, i3);   // 新：序号      旧：总片数
  const String f3 = packet.substring(i3 + 1, i4);   // 新：片号/总片数  旧：片号
  String body = packet.substring(i4 + 1);           // 新：正文      旧：正文,序号

  uint32_t seq = 0;
  uint8_t total = 0, part = 0;

  const int slash = f3.indexOf('/');
  if (slash > 0) {
    // ---- 新格式 ----
    part = (uint8_t)f3.substring(0, slash).toInt();
    total = (uint8_t)f3.substring(slash + 1).toInt();
    seq = (uint32_t)f2.toInt();
  } else {
    // ---- 旧格式（正文里最后一个逗号后面是序号）----
    total = (uint8_t)f2.toInt();
    part = (uint8_t)f3.toInt();
    const int lastComma = body.lastIndexOf(',');
    if (lastComma > 0) {
      seq = (uint32_t)body.substring(lastComma + 1).toInt();
      body = body.substring(0, lastComma);
    }
  }

  mines[idx].lastSeenMs = millis();
  if (total == 0 || part == 0) return;

  // 已收齐的消息，60 秒内再收到它的分片：只回执，不重复显示
  if (seq > 0 && seq == textRx[idx].lastDoneSeq &&
      (millis() - textRx[idx].lastDoneMs) < 60000UL) {
    Serial.printf("[TXT] %s seq=%lu 第 %u/%u 片重复到达，仅回执\n",
                  nodeId.c_str(), (unsigned long)seq, part, total);
    sendAck(nodeId, seq, part);
    return;
  }

  if (part == 1) { textRx[idx].text = ""; textRx[idx].received = 0; }
  textRx[idx].total = total;
  textRx[idx].text += body;
  textRx[idx].received++;

  Serial.printf("[TXT] %s seq=%lu %u/%u: %s\n",
                nodeId.c_str(), (unsigned long)seq, part, total, body.c_str());

  // 新协议要逐片回执（旧协议的 seq 解析不到时 seq=0，不回执）
  if (seq > 0) sendAck(nodeId, seq, part);

  if (textRx[idx].received >= total) {
    String full = textRx[idx].text;
    textRx[idx].text = "";
    textRx[idx].received = 0;
    textRx[idx].lastDoneSeq = seq;
    textRx[idx].lastDoneMs = millis();
    reportEvent(idx, full, textHasAlarmWord(full), rssi);
  }
}

// JSON 文本转义，保留井下转写里的引号与反斜杠。
static String jsonEscape(const String &s) {
  String out = "";
  for (int i = 0; i < (int)s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

// OneNET 的 m1_rx_event / m2_text 属性最长 255 字节；按 UTF-8 边界截断云端副本。
// 原始文本仍保留在 mines[].lastEvent，供本地 OLED 和串口使用。
static String cloudTextUtf8(const String &text) {
  constexpr int kMaxBytes = 255;
  if (text.length() <= kMaxBytes) return text;
  int end = kMaxBytes;
  while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) --end;
  return text.substring(0, end);
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
  String bottom = String("WiFi") + (wifiConnected ? "通" : "断");
  if (ENABLE_GSM) {
    bottom += String(" GSM") + (gsmReady ? "通" : "断");
  }
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

void queueCloudSnapshot() {
  if (cloudQueue == nullptr || !wifiConnected) return;
  const unsigned long now = millis();
  const bool m1Online = mines[0].lastSeenMs > 0 && now - mines[0].lastSeenMs < NODE_ONLINE_MS;
  const bool m2Online = mines[1].lastSeenMs > 0 && now - mines[1].lastSeenMs < NODE_ONLINE_MS;

  String body = "{\"id\":\"gw-" + String(now) + "\",\"version\":\"1.0\",\"params\":{";
  body += "\"m1_lora_online\":{\"value\":" + String(m1Online ? "true" : "false") + "},";
  body += "\"m1_rssi\":{\"value\":" + String(mines[0].rssi) + "},";
  body += "\"m1_alarm\":{\"value\":" + String(mines[0].inAlert ? "true" : "false") + "},";
  body += "\"m1_rx_event\":{\"value\":\"" + jsonEscape(cloudTextUtf8(mines[0].lastEvent)) + "\"},";
  body += "\"m2_lora_online\":{\"value\":" + String(m2Online ? "true" : "false") + "},";
  body += "\"m2_rssi\":{\"value\":" + String(mines[1].rssi) + "},";
  body += "\"m2_alarm\":{\"value\":" + String(mines[1].inAlert ? "true" : "false") + "},";
  body += "\"m2_text\":{\"value\":\"" + jsonEscape(cloudTextUtf8(mines[1].lastEvent)) + "\"},";
  body += "\"gw_wifi\":{\"value\":true},";
  body += "\"gw_uptime\":{\"value\":" + String(now / 1000UL) + "}}}";

  CloudMessage message = {};
  if (body.length() >= sizeof(message.json)) {
    Serial.println("[OneNET] 快照过长，已跳过本次上报");
    return;
  }
  body.toCharArray(message.json, sizeof(message.json));
  xQueueOverwrite(cloudQueue, &message); // only keep the newest snapshot if WAN is slow
}

void cloudUploadTask(void *parameter) {
  (void)parameter;
  NetworkClientSecure client;
  client.setCACert(ONENET_ROOT_CA);
  const String url = "https://open.iot.10086.cn/studio/http/device/thing/property/post?topic=%24sys%2F" +
                     String(ONENET_PRODUCT_ID) + "%2F" + String(ONENET_DEVICE_NAME) +
                     "%2Fthing%2Fproperty%2Fpost&protocol=HTTP";
  CloudMessage message = {};
  while (true) {
    if (xQueueReceive(cloudQueue, &message, portMAX_DELAY) != pdTRUE) continue;
    if (WiFi.status() != WL_CONNECTED) continue;
    if (time(nullptr) < 1700000000) {
      Serial.println("[OneNET] 等待校时后再验证 HTTPS 证书");
      continue;
    }
    HTTPClient request;
    request.setTimeout(4000);
    if (!request.begin(client, url)) {
      Serial.println("[OneNET] HTTPS 初始化失败");
      continue;
    }
    request.addHeader("Content-Type", "application/json");
    request.addHeader("token", ONENET_DEVICE_TOKEN);
    const int httpCode = request.POST((uint8_t *)message.json, strlen(message.json));
    const String response = request.getString();
    request.end();
    const int marker = response.indexOf("\"errno\"");
    const int colon = marker < 0 ? -1 : response.indexOf(':', marker);
    const int cloudCode = colon < 0 ? -1 : response.substring(colon + 1).toInt();
    Serial.printf("[OneNET] 网关快照 HTTP=%d errno=%d\n", httpCode, cloudCode);
  }
}
