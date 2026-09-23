// =============================================================
//  矿安智联 · 地面网关 (Surface Gateway)
//
//  硬件: ESP32-S3 N16R8 + Ra-02(SX1278, 433MHz) + OLED (SSD1306/SH1106)
//  作用: 接收井下节点 LoRa 报文 -> 解析 -> OLED 显示与报警 -> WiFi 上报 Firebase
//  链路: 主链路是井下->地面(语音/数据/心跳); 地面只回一种下行报文 ACK,<节点>,
//        告诉井下"地面收到了", 井下收到后把 OLED 切到回执画面。不回撤人命令。
//
//  报文格式(与井下节点 esp32s3-kws 完全一致):
//    VOICE,<节点>,<事件码>,<序号>           识别到语音命令, 例 VOICE,M1,HELP,12
//    DATA,<节点>,<mq4>,<mq7>,<flags>,<序号>  传感器遥测, flags bit0=CH4超限 bit1=CO超限
//    STATUS,<节点>,<状态>,<序号>             心跳, 例 STATUS,M1,KWS_READY,37
//    ACK,<节点>                              (下行) 收到确认, 例 ACK,M1
//  节点号: M1 = 应急语音节点, M2 = 离线转写节点
//
//  上报的云端字段与 web_dashboard/app.js 读取的形状一致:
//    /status/<mineN>  { inAlert, title, message, severity, ... }
//    /alerts/<mineN>/{latest,history}  仅在报警时写入, 留痕用
// =============================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
// 没有 include/secrets.h 也能编译: 用占位值, 并强制关闭云端上报
#define HAVE_SECRETS 0
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static const char *FIREBASE_DB_URL = "";
static const char *NTP_SERVER_1 = "ntp.aliyun.com";
static const char *NTP_SERVER_2 = "ntp1.aliyun.com";
#endif

// -------------------------------------------------------------
//  功能开关: 分步调试时可以先只开 LoRa, 再逐个打开外设
// -------------------------------------------------------------
#define ENABLE_OLED  1
#define ENABLE_CLOUD 1   // 需要 include/secrets.h; 缺这个文件时自动跳过

// -------------------------------------------------------------
//  LoRa 引脚与射频参数 (必须与井下节点一致)
// -------------------------------------------------------------
#define LORA_PIN_DIO0 9
#define LORA_PIN_NSS  10
#define LORA_PIN_MOSI 11
#define LORA_PIN_SCK  12
#define LORA_PIN_MISO 13
#define LORA_PIN_RST  14
static const long    LORA_FREQUENCY_HZ = 433E6;
static const uint8_t LORA_SYNC_WORD    = 0xA3;

// 注意: 这里不要调用 setSpreadingFactor / setSignalBandwidth / setCodingRate4。
// 井下节点用的是 arduino-LoRa 的默认值(SF7 / 125kHz / CR 4-5 / 前导码 8),
// 两端参数必须一致才能互通; 早期 surface_node.ino 里写了 SF9, 那样是收不到的。

// -------------------------------------------------------------
//  OLED 引脚
//  GPIO17/18/19 空闲(原计划给 SIM800L 短信, 该功能已取消)
// -------------------------------------------------------------
#define OLED_PIN_SDA 5
#define OLED_PIN_SCL 6

// -------------------------------------------------------------
//  有源蜂鸣器 (三线模块 VCC / IO / GND, 额定 5 V, 高电平触发)
//
//  模块的 IO 阈值是按 5 V 算的, ESP32 的 3.3 V 输出可能推不动它;
//  而且模块 IO 若内部上拉到 5 V, 直连还会往 GPIO 里倒灌电流。
//  两种接法对应下面这个宏:
//    ① 直连(先试这个): VCC→5V, IO→GPIO8, GND→GND          → 宏填 1
//    ② 加 NPN 三极管反相: 基极经 1kΩ 接 GPIO8, 发射极接 GND,
//       集电极接模块 IO 并 10kΩ 上拉到 5V                  → 宏填 0
//  ②的逻辑是"GPIO 拉低 → 集电极被上拉到 5V → 模块响", 属低电平有效。
// -------------------------------------------------------------
#define BUZZER_PIN          8
#define BUZZER_ACTIVE_HIGH  1

// OLED 驱动芯片: 0.96 英寸模块基本都是 SSD1306, 1.3 英寸模块大多是 SH1106。
// 选错了也能点亮, 但画面会整体偏移(常见是左边少 2 列), 所以按实物改这一行。
#define OLED_CTRL_SSD1306 0
#define OLED_CTRL_SH1106  1
#define OLED_CONTROLLER   OLED_CTRL_SSD1306   // 从井下节点移过来的 0.96 寸屏是 SSD1306

// -------------------------------------------------------------
//  时间参数
// -------------------------------------------------------------
static const uint32_t NODE_OFFLINE_MS     = 15000;  // 3 个心跳周期(5s)没收到即判离线
static const uint32_t VOICE_ALARM_HOLD_MS = 30000;  // 语音报警保持时长, 超时自动解除
static const uint32_t BUZZER_ON_MS        = 500;    // 蜂鸣器间歇鸣叫: 响的时长
static const uint32_t BUZZER_OFF_MS       = 500;    //                    停的时长
static const uint32_t EVENT_DEDUP_MS      = 3000;   // 同一事件码 3 秒内重复只当作刷新
static const uint32_t CLOUD_STATUS_MIN_MS = 5000;   // 单节点状态上报的最小间隔
static const uint32_t LORA_RETRY_MS       = 5000;   // LoRa 初始化失败后的重试间隔
static const uint32_t ACK_DELAY_MS        = 500;    // 收到语音包后延后多久回执, 见 handleAck 注释
static const uint8_t  ACK_REPEAT          = 3;      // 回执连发次数, 见 sendAck 注释
static const uint32_t ACK_GAP_MS          = 150;    // 连发之间的间隔
static const uint32_t LORA_TX_SETTLE_MS   = 100;    // 发送后等包发完的时间, 见 transmitPacket
static const uint32_t WIFI_RETRY_MS       = 15000;  // WiFi 断线重连间隔
static const uint32_t WIFI_CONNECT_MS     = 12000;  // 单次连接超时
static const uint32_t NTP_TIMEOUT_MS      = 20000;  // 等待 NTP 对时的超时
static const uint32_t SCREEN_IDLE_MS      = 2000;   // 无状态变化时的重绘间隔

// -------------------------------------------------------------
//  节点状态
// -------------------------------------------------------------
enum NodeIndex { NODE_M1 = 0, NODE_M2 = 1, NODE_COUNT = 2 };

struct NodeState {
  const char *id;         // 空口报文里的节点号
  const char *cloudId;    // Firebase 路径里的节点名(网页/App 用这个)
  const char *label;      // OLED / 串口显示名
  uint16_t    mq4;
  uint16_t    mq7;
  uint8_t     flags;
  int         rssi;
  uint32_t    seq;
  bool        seen;             // 是否至少收到过一包
  bool        online;           // 心跳是否在超时窗口内
  bool        alert;            // 当前是否处于报警状态
  bool        voiceHold;        // 语音类报警, 需要超时自动解除
  bool        alertPushed;      // 本次报警是否已写入 history, 避免重复
  char        title[24];
  char        message[80];
  char        lastCode[12];     // 最近一次事件码, 去重用
  const char *severity;
  const char *category;
  uint32_t    lastSeenMs;
  uint32_t    lastAlarmMs;
  uint32_t    lastCodeMs;
  uint32_t    lastCloudMs;

  // ---- 非报警事件的限时提示画面(盖在状态页上, 到期自动回状态页) ----
  char        noticeTitle[24];
  char        noticeMessage[80];
  uint32_t    noticeUntilMs;
};

static NodeState nodes[NODE_COUNT] = {
  {"M1", "mine1", "节点1", 0, 0, 0, -120, 0, false, false, false, false, false,
   "待机", "等待数据", "", "info", "normal", 0, 0, 0, 0},
  {"M2", "mine2", "节点2", 0, 0, 0, -120, 0, false, false, false, false, false,
   "待机", "等待数据", "", "info", "normal", 0, 0, 0, 0},
};

static bool loraReady = false;
static bool oledReady = false;
static uint32_t loraRetryMs = 0;
static bool screenDirty = true;
static uint32_t lastScreenMs = 0;
static uint32_t alarmTotal = 0;

// 下行回执: 收到语音包后不立刻发, 记下时间到点再发(见 handleAck)
static uint32_t pendingAckAtMs = 0;   // 0 = 没有待发回执
static int8_t   pendingAckIdx  = -1;
static uint32_t ackSentTotal   = 0;

// 蜂鸣器状态(见 handleBuzzer)
static bool     buzzerOn        = false;   // 当前这一拍是否在响
static uint32_t buzzerNextMs    = 0;       // 下一次翻转的时刻
static bool     buzzerAnnounced = false;   // 本次报警是否已打过"开始鸣叫"日志

// 云端上报总开关: 需要"编译开关打开 + 找到 secrets.h + secrets.h 里的参数已填写"三者同时满足
static bool cloudEnabled = false;
static bool wifiConnected = false;
static bool wifiConnecting = false;
static bool ntpSynced = false;
static uint32_t wifiStageMs = 0;
static uint32_t ntpStartMs = 0;

#if ENABLE_OLED
#if OLED_CONTROLLER == OLED_CTRL_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif
#endif

static NetworkClientSecure tlsClient;
static HTTPClient http;

// -------------------------------------------------------------
//  前置声明
// -------------------------------------------------------------
static void initLoRa();
static void handleLoRa();
static void handleAck();
static void handlePacket(const String &packet, int rssi);
static void handleVoicePacket(const String &packet, int rssi);
static void handleDataPacket(const String &packet, int rssi);
static void handleStatusPacket(const String &packet, int rssi);
static void setEvent(uint8_t idx, const char *code, const char *title,
                     const char *message, const char *severity,
                     const char *category, bool alarm, bool voiceHold,
                     uint16_t holdMs);
static void markSeen(uint8_t idx, int rssi, uint32_t seq);
static void maintainNodes();
static void handleWifi();
static void startWifiAttempt();
static void buzzerApply(bool on);
static void handleBuzzer();
static void handleScreen();
static void pushStatus(uint8_t idx, bool force);
static void pushAlertHistory(uint8_t idx);

// =============================================================
//  setup
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=========================================");
  Serial.println("  矿安智联 · 地面网关 (Surface Gateway)");
  Serial.println("  LoRa: NSS10 MOSI11 SCK12 MISO13 RST14 DIO0 9");
  Serial.println("  OLED: SDA5 SCL6");
  Serial.println("  Buzzer: GPIO8 (5V 有源, 高电平触发)");
  Serial.println("=========================================");

#if ENABLE_OLED
  Wire.setPins(OLED_PIN_SDA, OLED_PIN_SCL);
  Wire.begin(OLED_PIN_SDA, OLED_PIN_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(0x3C * 2);   // 0.96/1.3 寸模块基本都是 0x3C, 个别是 0x3D
  oledReady = u8g2.begin();
  Serial.printf("[OLED] %s (驱动=%s, SDA%d/SCL%d)\n",
                oledReady ? "就绪" : "未检测到, 检查接线与地址 0x3C",
                (OLED_CONTROLLER == OLED_CTRL_SH1106) ? "SH1106" : "SSD1306",
                OLED_PIN_SDA, OLED_PIN_SCL);
#else
  Serial.println("[OLED] 已按编译开关关闭");
#endif

  // 有源蜂鸣器: 上电短鸣一声自检, 顺便验证模块和触发电平对不对
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerApply(false);
  buzzerApply(true);
  delay(120);
  buzzerApply(false);
  Serial.printf("[蜂鸣器] 就绪: GPIO%d, %s触发%s\n", BUZZER_PIN,
                BUZZER_ACTIVE_HIGH ? "高电平" : "低电平",
                BUZZER_ACTIVE_HIGH ? "" : " (经三极管反相)");

  initLoRa();

#if ENABLE_CLOUD && HAVE_SECRETS
  // 参数没填就别去连: 否则每 15s 重试一次, 白白刷屏
  cloudEnabled = (strncmp(WIFI_SSID, "YOUR_", 5) != 0) && (strlen(FIREBASE_DB_URL) > 8);
#endif

  if (cloudEnabled) {
    Serial.printf("[WiFi] 目标热点: %s\n", WIFI_SSID);
    startWifiAttempt();
  } else if (!ENABLE_CLOUD) {
    Serial.println("[WiFi] 云端上报已按编译开关(ENABLE_CLOUD)关闭");
  } else if (!HAVE_SECRETS) {
    Serial.println("[WiFi] 未找到 include/secrets.h, 跳过云端上报; "
                   "复制 secrets.example.h 为 secrets.h 并填写即可启用");
  } else {
    Serial.println("[WiFi] secrets.h 里的 WIFI_SSID / FIREBASE_DB_URL 还没填, 跳过云端上报");
  }

  screenDirty = true;
  handleScreen();
}

// =============================================================
//  loop
// =============================================================
void loop() {
  handleLoRa();
  handleAck();
  handleWifi();
  maintainNodes();
  handleBuzzer();
  handleScreen();
}

// =============================================================
//  LoRa 接收
// =============================================================
static void initLoRa() {
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  LoRa.setPins(LORA_PIN_NSS, LORA_PIN_RST, LORA_PIN_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY_HZ)) {
    Serial.println("[LoRa] 初始化失败: 检查 Ra-02 供电(3.3V)、"
                   "NSS10/MOSI11/SCK12/MISO13/RST14 与天线");
    loraReady = false;
    loraRetryMs = millis() + LORA_RETRY_MS;
    screenDirty = true;   // 让 OLED 立刻显示"LoRa未就绪"
    return;
  }
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  loraReady = true;
  screenDirty = true;
  Serial.println("[LoRa] 就绪: 433MHz, sync=0xA3, 接收模式");
}

static void handleLoRa() {
  if (!loraReady) {
    if ((int32_t)(millis() - loraRetryMs) >= 0) initLoRa();
    return;
  }

  const int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  String packet;
  packet.reserve(packetSize + 1);
  while (LoRa.available()) packet += (char)LoRa.read();
  const int rssi = LoRa.packetRssi();
  packet.trim();

  Serial.printf("[LoRa] RSSI=%d dBm  长度=%d  %s\n", rssi, packetSize, packet.c_str());
  handlePacket(packet, rssi);
}

// =============================================================
//  下行回执 (地面 -> 井下)  报文: ACK,<节点>
//
//  LoRa 是半双工: 两端同时发谁也收不到。井下发完 VOICE 后短时间不会再发
//  (下一个 5s 心跳之前), 所以收到语音包后延后 ACK_DELAY_MS 再回, 避开它的
//  发送时隙。井下收到后把 OLED 切到 "GROUND ACK / RESCUE ON WAY" 画面。
// =============================================================

/* 带超时的发送。
   不能直接用库里的 LoRa.endPacket(): 它内部是
       while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0) { yield(); }
   没有任何超时。射频一旦异常(天线开路/短路、3.3V 供电不足), TX_DONE 永远不置位,
   整个 loop 就停在这一行 —— 表现是 OLED 画面冻结、不再收包、连报警都收不到,
   看起来就像"节点1 一直离线"(实测复现过, 按 RST 才恢复)。
   endPacket 的等待也改不了(没有超时接口, isTransmitting/readRegister 都是私有),
   所以这里异步启动发送, 固定等 LORA_TX_SETTLE_MS 让包发完, 再 idle() 强制回待机:
   无论射频是否正常都不会卡住, 且保证下一轮 parsePacket 能继续收。
   注意这段等待期间不能调 parsePacket —— 它发现芯片不在 RX 模式会强制切回 RX,
   把还没发完的包打断。 */
static bool transmitPacket(const String &packet) {
  if (!loraReady) return false;

  if (!LoRa.beginPacket()) {
    Serial.println("[LoRa] 发送忙, 本次跳过");
    return false;
  }
  LoRa.print(packet);
  LoRa.endPacket(true);        // async: 只启动发送, 立即返回
  delay(LORA_TX_SETTLE_MS);    // 6 字节包空中时间约 36ms, 100ms 足够
  LoRa.idle();                 // 幂等: 已完成就在待机, 没完成则强制放开
  return true;
}

/* 连发 ACK_REPEAT 次。
   实测单发送达率只有 1/4 左右: 井下主循环里还跑着语音识别和 OLED 全屏刷新,
   接收窗口时断时续, 单包很容易正好落在它不在 RX 的瞬间。连发几次跨约 0.6s,
   只要有一次落在井下的监听窗口内就成。 */
static void sendAck(uint8_t idx) {
  if (!loraReady) return;
  const NodeState &n = nodes[idx];
  const String packet = String("ACK,") + n.id;

  uint8_t sent = 0;
  for (uint8_t i = 0; i < ACK_REPEAT; i++) {
    if (transmitPacket(packet)) sent++;
    if (i + 1 < ACK_REPEAT) delay(ACK_GAP_MS);
  }
  if (sent) ackSentTotal++;
  Serial.printf("[回执] 向 %s(%s) 连发 %u/%u 次 (累计 %u 轮)\n",
                n.label, n.id, sent, (unsigned)ACK_REPEAT, ackSentTotal);
}

static void handleAck() {
  if (pendingAckAtMs == 0 || !loraReady) return;
  if ((int32_t)(millis() - pendingAckAtMs) < 0) return;   // 还没到点

  const int8_t idx = pendingAckIdx;
  pendingAckAtMs = 0;
  pendingAckIdx = -1;
  if (idx >= 0) sendAck((uint8_t)idx);
}

static void handlePacket(const String &packet, int rssi) {
  if (packet.startsWith("VOICE,"))       handleVoicePacket(packet, rssi);
  else if (packet.startsWith("DATA,"))   handleDataPacket(packet, rssi);
  else if (packet.startsWith("STATUS,")) handleStatusPacket(packet, rssi);
  else Serial.println("[LoRa] 未知报文类型, 已忽略");
}

/* 把报文里的节点号映射成下标; 不认识的节点返回 -1 */
static int nodeIndexOf(const String &id) {
  if (id == "M1") return NODE_M1;
  if (id == "M2") return NODE_M2;
  return -1;
}

/* 心跳/数据包共用的"到达标记": 更新在线状态、RSSI 与序号 */
static void markSeen(uint8_t idx, int rssi, uint32_t seq) {
  NodeState &n = nodes[idx];
  n.lastSeenMs = millis();
  n.rssi = rssi;
  n.seq = seq;

  if (!n.seen) {
    n.seen = true;
    n.online = true;
    screenDirty = true;
    Serial.printf("[在线] %s(%s) 首次收到报文\n", n.label, n.id);
    return;
  }
  if (!n.online) {
    n.online = true;
    screenDirty = true;
    Serial.printf("[在线] %s(%s) 恢复在线\n", n.label, n.id);
  }
}

// =============================================================
//  VOICE,<节点>,<事件码>,<序号>
// =============================================================
struct EventMap {
  const char *code;
  const char *title;
  const char *message;
  const char *severity;
  const char *category;
  bool        alarm;
  uint16_t    holdMs;   // 非报警事件在 OLED 上停留的毫秒数(0 = 不弹提示画面)
};

static const EventMap kEventMap[] = {
  /* AWAKE: 井下节点识别到唤醒词"屈展"时发来, 让网关也切到"请说命令"画面 */
  {"AWAKE",    "语音已唤醒", "请说命令：紧急救命 请求救援 有人被困", "info", "voice", false, 15000},
  {"HELP",     "语音呼救", "紧急救命", "critical", "voice",    true,  0},
  {"RESCUE",   "语音呼救", "请求救援", "critical", "voice",    true,  0},
  {"TRAPPED",  "语音呼救", "有人被困", "critical", "voice",    true,  0},
  {"EVACUATE", "撤离指令", "撤离",     "critical", "voice",    true,  0},
  {"GAS",      "瓦斯超限", "瓦斯超限", "critical", "gas",      true,  0},
  {"WATER",    "透水报警", "透水",     "critical", "water",    true,  0},
  {"COLLAPSE", "塌方报警", "塌方",     "critical", "collapse", true,  0},
  {"FIRE",     "火灾报警", "起火",     "critical", "fire",     true,  0},
  {"NORMAL",   "一切正常", "现场正常", "info",     "normal",   false, 3000},
  {"OK",       "收到反馈", "收到",     "info",     "ack",      false, 2000},
  {"ACK",      "收到反馈", "收到",     "info",     "ack",      false, 2000},
  {"RX",       "接收确认", "接收确认", "info",     "ack",      false, 2000},
  {"TEST",     "通话测试", "通话测试", "info",     "test",     false, 3000},
};

static void handleVoicePacket(const String &packet, int rssi) {
  // VOICE,<节点>,<事件码>,<序号>
  const int i1 = packet.indexOf(',');
  const int i2 = packet.indexOf(',', i1 + 1);
  if (i1 < 0 || i2 < 0) {
    Serial.println("[VOICE] 字段不足, 已忽略");
    return;
  }
  const int i3 = packet.indexOf(',', i2 + 1);

  String nodeId = packet.substring(i1 + 1, i2);
  nodeId.trim();
  const int idx = nodeIndexOf(nodeId);
  if (idx < 0) {
    Serial.printf("[VOICE] 未知节点 %s, 已忽略\n", nodeId.c_str());
    return;
  }

  String code = (i3 < 0) ? packet.substring(i2 + 1) : packet.substring(i2 + 1, i3);
  code.trim();
  const uint32_t seq = (i3 < 0) ? 0 : (uint32_t)packet.substring(i3 + 1).toInt();

  markSeen(idx, rssi, seq);

  /* 语音事件都回一个确认, 让井下知道地面收到了; 延后发出, 见 handleAck */
  pendingAckIdx = (int8_t)idx;
  pendingAckAtMs = millis() + ACK_DELAY_MS;

  for (const EventMap &e : kEventMap) {
    if (code == e.code) {
      /* 报警画面只显示中文短语本身; RSSI 和序号在串口日志里看 */
      setEvent(idx, e.code, e.title, e.message, e.severity, e.category, e.alarm,
               e.alarm, e.holdMs);
      return;
    }
  }

  // 未登记的事件码: 原样显示, 按报警处理更安全
  char msg[80];
  snprintf(msg, sizeof(msg), "%s", code.c_str());
  setEvent(idx, code.c_str(), "未知事件", msg, "warning", "unknown", true, true, 0);
}

// =============================================================
//  DATA,<节点>,<mq4>,<mq7>,<flags>,<序号>
// =============================================================
static void handleDataPacket(const String &packet, int rssi) {
  const int i1 = packet.indexOf(',');
  const int i2 = packet.indexOf(',', i1 + 1);
  const int i3 = packet.indexOf(',', i2 + 1);
  const int i4 = packet.indexOf(',', i3 + 1);
  const int i5 = packet.indexOf(',', i4 + 1);
  if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0) {
    Serial.println("[DATA] 字段不足, 已忽略");
    return;
  }

  String nodeId = packet.substring(i1 + 1, i2);
  nodeId.trim();
  const int idx = nodeIndexOf(nodeId);
  if (idx < 0) {
    Serial.printf("[DATA] 未知节点 %s, 已忽略\n", nodeId.c_str());
    return;
  }

  NodeState &n = nodes[idx];
  n.mq4 = (uint16_t)packet.substring(i2 + 1, i3).toInt();
  n.mq7 = (uint16_t)packet.substring(i3 + 1, i4).toInt();
  n.flags = (uint8_t)packet.substring(i4 + 1, i5).toInt();
  markSeen(idx, rssi, (uint32_t)packet.substring(i5 + 1).toInt());

  Serial.printf("[DATA] %s CH4=%u CO=%u flags=0x%02X rssi=%d\n",
                n.id, n.mq4, n.mq7, n.flags, rssi);

  char msg[80];
  if (n.flags != 0) {
    char detail[40] = "";
    if (n.flags & 0x01) strncat(detail, "CH4超限 ", sizeof(detail) - strlen(detail) - 1);
    if (n.flags & 0x02) strncat(detail, "CO超限 ", sizeof(detail) - strlen(detail) - 1);
    snprintf(msg, sizeof(msg), "%sCH4=%u CO=%u", detail, n.mq4, n.mq7);
    // 单项超限按 warning, 两项同时超限按 critical 升级
    setEvent(idx, "DATA", "气体超限", msg,
             (n.flags & 0x03) == 0x03 ? "critical" : "warning",
             (n.flags & 0x01) ? "gas" : "co", true, false, 0);
  } else {
    snprintf(msg, sizeof(msg), "CH4=%u CO=%u 正常", n.mq4, n.mq7);
    // 数据包回到正常值即解除环境类报警; 语音报警仍靠超时解除
    if (n.alert && !n.voiceHold) {
      setEvent(idx, "NORMAL", "环境正常", msg, "info", "normal", false, false, 0);
    } else if (!n.alert) {
      snprintf(n.title, sizeof(n.title), "环境正常");
      snprintf(n.message, sizeof(n.message), "%s", msg);
      n.severity = "info";
      n.category = "normal";
      screenDirty = true;
    }
  }

  pushStatus(idx, false);
}

// =============================================================
//  STATUS,<节点>,<状态>,<序号>  心跳: 只更新在线状态, 不报警也不解除报警
// =============================================================
static void handleStatusPacket(const String &packet, int rssi) {
  const int i1 = packet.indexOf(',');
  const int i2 = packet.indexOf(',', i1 + 1);
  if (i1 < 0) {
    Serial.println("[STATUS] 字段不足, 已忽略");
    return;
  }
  String nodeId = packet.substring(i1 + 1, (i2 < 0) ? -1 : i2);
  nodeId.trim();
  const int idx = nodeIndexOf(nodeId);
  if (idx < 0) {
    Serial.printf("[STATUS] 未知节点 %s, 已忽略\n", nodeId.c_str());
    return;
  }

  const int i3 = (i2 < 0) ? -1 : packet.indexOf(',', i2 + 1);
  String state = (i2 < 0) ? "?" : ((i3 < 0) ? packet.substring(i2 + 1)
                                            : packet.substring(i2 + 1, i3));
  state.trim();
  const uint32_t seq = (i3 < 0) ? 0 : (uint32_t)packet.substring(i3 + 1).toInt();

  markSeen(idx, rssi, seq);
  Serial.printf("[心跳] %s(%s) %s seq=%u rssi=%d\n",
                nodes[idx].label, nodes[idx].id, state.c_str(), seq, rssi);

  pushStatus(idx, false);
}

// =============================================================
//  报警状态机
// =============================================================
static void setEvent(uint8_t idx, const char *code, const char *title,
                     const char *message, const char *severity,
                     const char *category, bool alarm, bool voiceHold,
                     uint16_t holdMs) {
  NodeState &n = nodes[idx];
  const uint32_t now = millis();

  // 同一事件码短时间重复到达(井下节点应急包会连发), 只刷新保持时间, 不重复响铃/上报
  const bool duplicate = alarm && n.alert && strncmp(n.lastCode, code, sizeof(n.lastCode)) == 0 &&
                         (now - n.lastCodeMs) < EVENT_DEDUP_MS;
  if (duplicate) {
    n.lastAlarmMs = now;
    n.lastCodeMs = now;
    Serial.printf("[去重] %s %s 重复到达, 只刷新保持时间\n", n.label, code);
    return;
  }

  // 同一次报警期间(事件码与等级都没变)只写一条 history, 避免持续报警刷出一堆重复记录;
  // 解除后再次报警、或等级升级(如单项超限→双项超限)才算新的一次, 会再写一条。
  const bool newEpisode = !n.alert ||
                          strncmp(n.lastCode, code, sizeof(n.lastCode)) != 0 ||
                          strcmp(n.severity, severity) != 0;

  snprintf(n.title, sizeof(n.title), "%s", title);
  snprintf(n.message, sizeof(n.message), "%s", message);
  snprintf(n.lastCode, sizeof(n.lastCode), "%s", code);
  n.severity = severity;
  n.category = category;
  n.alert = alarm;
  n.voiceHold = alarm && voiceHold;
  n.lastCodeMs = now;
  if (newEpisode) n.alertPushed = false;
  screenDirty = true;

  if (alarm) {
    n.lastAlarmMs = now;
    n.noticeUntilMs = 0;          // 报警画面优先, 取消未到期的提示画面
    alarmTotal++;
    Serial.printf("[报警] %s(%s) %s | %s | 累计 %u 次\n",
                  n.label, n.id, n.title, n.message, alarmTotal);
    pushStatus(idx, true);       // 报警立即上报, 不等 5s 节流
    pushAlertHistory(idx);
  } else {
    // 非报警事件: 在 OLED 上弹一个限时提示画面(例如唤醒后的"请说命令")
    if (holdMs > 0) {
      snprintf(n.noticeTitle, sizeof(n.noticeTitle), "%s", title);
      snprintf(n.noticeMessage, sizeof(n.noticeMessage), "%s", message);
      n.noticeUntilMs = now + holdMs;
      Serial.printf("[提示] %s %s (%us)\n", n.label, n.title, (unsigned)(holdMs / 1000));
    }
    Serial.printf("[事件] %s(%s) %s | %s\n", n.label, n.id, n.title, n.message);
    pushStatus(idx, true);
  }
}

/* ---------------- 蜂鸣器 ----------------
   任一节点处于报警状态就间歇鸣叫(响 500ms / 停 500ms), 报警全部解除后立刻停。
   极性由 BUZZER_ACTIVE_HIGH 决定, 见文件头的两种接法说明。 */
static void buzzerApply(bool on) {
#if BUZZER_ACTIVE_HIGH
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BUZZER_PIN, on ? LOW : HIGH);
#endif
}

static void handleBuzzer() {
  bool anyAlarm = false;
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    if (nodes[i].alert) { anyAlarm = true; break; }
  }

  if (!anyAlarm) {
    if (buzzerAnnounced) {
      buzzerAnnounced = false;
      buzzerOn = false;
      buzzerApply(false);
      Serial.println("[蜂鸣器] 报警结束, 停止鸣叫");
    }
    return;
  }

  const uint32_t now = millis();
  if (!buzzerAnnounced) {          // 刚进入报警: 立刻响第一拍
    buzzerAnnounced = true;
    buzzerOn = true;
    buzzerApply(true);
    buzzerNextMs = now + BUZZER_ON_MS;
    Serial.println("[蜂鸣器] 报警: 开始间歇鸣叫");
    return;
  }

  if ((int32_t)(now - buzzerNextMs) < 0) return;
  buzzerOn = !buzzerOn;
  buzzerApply(buzzerOn);
  buzzerNextMs = now + (buzzerOn ? BUZZER_ON_MS : BUZZER_OFF_MS);
}

/* 在线判离线 + 语音报警超时自动解除 */
static void maintainNodes() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    NodeState &n = nodes[i];

    if (n.seen && n.online && (now - n.lastSeenMs) > NODE_OFFLINE_MS) {
      n.online = false;
      screenDirty = true;
      Serial.printf("[离线] %s(%s) 超过 %lus 未收到心跳\n",
                    n.label, n.id, (unsigned long)(NODE_OFFLINE_MS / 1000));
    }

    if (n.voiceHold && n.alert && (now - n.lastAlarmMs) >= VOICE_ALARM_HOLD_MS) {
      n.alert = false;
      n.voiceHold = false;
      snprintf(n.title, sizeof(n.title), "报警已解除");
      snprintf(n.message, sizeof(n.message), "语音报警保持 %lus 后自动解除",
               (unsigned long)(VOICE_ALARM_HOLD_MS / 1000));
      n.severity = "info";
      n.category = "normal";
      screenDirty = true;
      Serial.printf("[解除] %s(%s) 语音报警超时自动解除\n", n.label, n.id);
      pushStatus(i, true);
    }
  }
}

// =============================================================
//  WiFi 与 NTP 对时(全程非阻塞, 不影响收 LoRa)
// =============================================================
static void startWifiAttempt() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnecting = true;
  wifiStageMs = millis();
  Serial.println("[WiFi] 连接中...");
}

static void handleWifi() {
  if (!cloudEnabled) return;
  const uint32_t now = millis();

  if (wifiConnected) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      wifiConnecting = false;
      wifiStageMs = now;
      screenDirty = true;
      Serial.println("[WiFi] 连接断开, 稍后重连");
      return;
    }
    if (!ntpSynced) {
      if (ntpStartMs == 0) {
        ntpStartMs = now;
        configTime(8 * 3600, 0, NTP_SERVER_1, NTP_SERVER_2);   // 东八区
        Serial.println("[NTP] 开始对时...");
      } else if (time(nullptr) > 1600000000) {
        ntpSynced = true;
        screenDirty = true;
        Serial.printf("[NTP] 对时完成, 当前时间戳 %llu\n",
                      (unsigned long long)((uint64_t)time(nullptr) * 1000ULL));
      } else if (now - ntpStartMs > NTP_TIMEOUT_MS) {
        ntpStartMs = now;   // 超时后重试, 但不再刷屏
        Serial.println("[NTP] 对时超时, 稍后重试(上报时间戳暂记 0)");
      }
    }
    return;
  }

  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      wifiConnecting = false;
      tlsClient.setInsecure();   // 演示用: 跳过 TLS 证书校验; 正式部署应改用 setCACert()
      screenDirty = true;
      Serial.printf("[WiFi] 已连接, IP=%s\n", WiFi.localIP().toString().c_str());
    } else if (now - wifiStageMs > WIFI_CONNECT_MS) {
      wifiConnecting = false;
      wifiStageMs = now;
      WiFi.disconnect();
      Serial.println("[WiFi] 连接超时, 稍后重试");
    }
    return;
  }

  if (now - wifiStageMs > WIFI_RETRY_MS) startWifiAttempt();
}

// =============================================================
//  Firebase 上报(Realtime Database REST)
// =============================================================
static uint64_t epochMs() {
  const time_t t = time(nullptr);
  if (t < 1600000000) return 0;   // 还没对上 NTP
  return (uint64_t)t * 1000ULL;
}

/* JSON 字符串转义: 中文(UTF-8)原样保留, 只处理会破坏格式的引号与反斜杠 */
static String jsonEscape(const char *s) {
  String out;
  for (const char *p = s; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\') continue;
    out += *p;
  }
  return out;
}

static String buildPayload(uint8_t idx) {
  const NodeState &n = nodes[idx];
  String p = "{";
  p += "\"nodeId\":\"" + String(n.id) + "\",";
  p += "\"inAlert\":" + String(n.alert ? "true" : "false") + ",";
  p += "\"active\":" + String(n.alert ? "true" : "false") + ",";      // 手机 App 读这个字段
  p += "\"online\":" + String(n.online ? "true" : "false") + ",";
  p += "\"severity\":\"" + String(n.severity) + "\",";                // critical / warning / info
  p += "\"title\":\"" + jsonEscape(n.title) + "\",";
  p += "\"message\":\"" + jsonEscape(n.message) + "\",";
  p += "\"type\":\"" + String(n.category) + "\",";
  p += "\"mq4\":" + String(n.mq4) + ",";
  p += "\"mq7\":" + String(n.mq7) + ",";
  p += "\"water\":0,";                                               // 当前节点没有水位传感器
  p += "\"flags\":" + String(n.flags) + ",";
  p += "\"rssi\":" + String(n.rssi) + ",";
  p += "\"seq\":" + String(n.seq) + ",";
  p += "\"updatedAt\":" + String((unsigned long long)epochMs());
  p += "}";
  return p;
}

static int httpPut(const String &url, const String &body) {
  http.begin(tlsClient, url);
  http.addHeader("Content-Type", "application/json");
  const int code = http.PUT(body);
  http.end();
  return code;
}

static int httpPost(const String &url, const String &body) {
  http.begin(tlsClient, url);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  http.end();
  return code;
}

/* 写 /status/<mineN>: 网页仪表盘和手机 App 都监听这里 */
static void pushStatus(uint8_t idx, bool force) {
  if (!cloudEnabled || !wifiConnected) return;
  NodeState &n = nodes[idx];
  const uint32_t now = millis();
  if (!force && (now - n.lastCloudMs) < CLOUD_STATUS_MIN_MS) return;
  n.lastCloudMs = now;

  const String url = String(FIREBASE_DB_URL) + "/status/" + String(n.cloudId) + ".json";
  const int code = httpPut(url, buildPayload(idx));
  Serial.printf("[云端] status/%s 写入 %s (HTTP %d)\n",
                n.cloudId, (code == 200 || code == 204) ? "成功" : "失败", code);
}

/* 报警留痕: 写 /alerts/<mineN>/latest 与 history; 每次报警只写一次 */
static void pushAlertHistory(uint8_t idx) {
  if (!cloudEnabled || !wifiConnected) return;
  NodeState &n = nodes[idx];
  if (n.alertPushed) return;
  n.alertPushed = true;

  const String body = buildPayload(idx);
  const int latest = httpPut(String(FIREBASE_DB_URL) + "/alerts/" + String(n.cloudId) +
                             "/latest.json", body);
  const int history = httpPost(String(FIREBASE_DB_URL) + "/alerts/" + String(n.cloudId) +
                               "/history.json", body);
  Serial.printf("[云端] alerts/%s latest=%d history=%d\n", n.cloudId, latest, history);
}

// =============================================================
//  OLED 显示
// =============================================================
#if ENABLE_OLED
/* 按像素宽度折行绘制 UTF-8 文本(中英混排) */
static void drawWrappedCN(int x, int y, const String &text, int maxWidth,
                          int maxLines, int lineHeight) {
  String line;
  int lineNo = 0;
  for (int i = 0; i < (int)text.length() && lineNo < maxLines;) {
    uint8_t c = (uint8_t)text[i];
    int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
    if (i + len > (int)text.length()) len = 1;
    const String ch = text.substring(i, i + len);
    i += len;
    const String candidate = line + ch;
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

static const char *nodeStatusText(const NodeState &n) {
  if (!n.seen) return "等待";
  return n.online ? "在线" : "离线";
}
#endif

static void handleScreen() {
#if ENABLE_OLED
  if (!oledReady) return;
  const uint32_t now = millis();
  if (!screenDirty && (now - lastScreenMs) < SCREEN_IDLE_MS) return;
  lastScreenMs = now;
  screenDirty = false;

  const NodeState *alarmNode = nullptr;
  const NodeState *noticeNode = nullptr;
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    if (nodes[i].alert) { alarmNode = &nodes[i]; break; }
  }
  // 显示优先级: 报警画面 > 限时提示画面(如"请说命令") > 状态页
  if (alarmNode == nullptr) {
    for (uint8_t i = 0; i < NODE_COUNT; i++) {
      if (nodes[i].noticeUntilMs != 0 &&
          (int32_t)(nodes[i].noticeUntilMs - now) > 0) {
        noticeNode = &nodes[i];
        break;
      }
    }
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);

  if (alarmNode != nullptr) {
    u8g2.drawUTF8(0, 12, "!! 报  警 !!");
    u8g2.drawHLine(0, 15, 128);
    String head = String(alarmNode->label) + " " + alarmNode->title;
    u8g2.drawUTF8(0, 29, head.c_str());
    drawWrappedCN(0, 43, alarmNode->message, 126, 2, 12);
  } else if (noticeNode != nullptr) {
    u8g2.drawUTF8(0, 12, noticeNode->noticeTitle);
    u8g2.drawHLine(0, 15, 128);
    String who = String(noticeNode->label) + " " + nodeStatusText(*noticeNode);
    u8g2.drawUTF8(0, 28, who.c_str());
    drawWrappedCN(0, 42, noticeNode->noticeMessage, 126, 2, 12);
  } else {
    u8g2.drawUTF8(0, 12, "矿安监测网关");
    u8g2.drawHLine(0, 15, 128);
    // 节点行: 在线时把 RSSI 也显示出来, 方便判断天线/距离
    const NodeState &n1 = nodes[NODE_M1];
    const NodeState &n2 = nodes[NODE_M2];
    String l1 = String("节点1 ") + nodeStatusText(n1);
    if (n1.online) l1 += String(" ") + n1.rssi + "dBm";
    String l2 = String("节点2 ") + nodeStatusText(n2);
    if (n2.online) l2 += String(" ") + n2.rssi + "dBm";
    u8g2.drawUTF8(0, 29, l1.c_str());
    u8g2.drawUTF8(0, 42, l2.c_str());
    // 第四行: LoRa 收发状态 + 云端状态(没接数据线时, 这行是唯一的诊断入口)
    String l3;
    if (!loraReady) {
      l3 = "LoRa未就绪!";
    } else if (!cloudEnabled) {
      l3 = "LoRa通 本地模式";
    } else {
      l3 = String("LoRa通 云") + (wifiConnected ? "通" : "断");
    }
    u8g2.drawUTF8(0, 55, l3.c_str());
  }

  u8g2.sendBuffer();
#endif
}
