/*
 * 矿山安全监测 - 井下离线语音关键词识别
 * ESP32-S3 N16R8 + INMP441
 * 软件唤醒词: 屈展
 * 命令词: 紧急救命 / 请求救援 / 有人被困
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include "OLED.h"
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "esp_log.h"
#include "esp32-hal-psram.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include <time.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

/* 节点1当前调试配置：只调语音 + LoRa；无 SIM、水位、LED、按键。 */
#define ENABLE_WIFI 0
#define ENABLE_LORA 1
#define ENABLE_MQ_SENSORS 0

#if ENABLE_WIFI
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include "onenet_secrets.h"
#include "web_assets.h"
#endif

/* INMP441 接线 */
#define I2S_PIN_BCK 15
#define I2S_PIN_WS  16
#define I2S_PIN_DIN 17

/* 0.96 英寸 SSD1306 I2C OLED，江科大驱动的 ESP32 移植版
   引脚按 GPIO分配.md：SDA=GPIO8，SCL=GPIO9 */
#define OLED_PIN_SDA 8
#define OLED_PIN_SCL 9
#define OLED_I2C_ADDRESS 0x3C

/* MQ 模拟输出接 ADC1，避免与 Wi-Fi、I2S 和 OLED 引脚冲突。 */
#define MQ4_PIN 4
#define MQ7_PIN 5
#define MQ_ADC_SAMPLES 16
#define MQ_UPLOAD_INTERVAL_MS 5000UL
#define MQ4_ALERT_THRESHOLD 1500
#define MQ7_ALERT_THRESHOLD 1500

/* SX1278 / Ra-02，433 MHz。引脚按 GPIO分配.md(2026-09-17 核对)：
   SCK=10 MOSI=11 MISO=12 NSS=13 RST=14 DIO0=21 */
#if ENABLE_LORA
#define LORA_PIN_DIO0 21
#define LORA_PIN_NSS  13
#define LORA_PIN_MOSI 11
#define LORA_PIN_SCK  10
#define LORA_PIN_MISO 12
#define LORA_PIN_RST  14
static const long LORA_FREQUENCY_HZ = 433E6;
static const uint8_t LORA_SYNC_WORD = 0xA3;
static const uint32_t LORA_TELEMETRY_INTERVAL_MS = 5000;
static const uint32_t LORA_TX_SETTLE_MS = 100;   // 发送后等包发完的时间, 见 sendLoRaPacket
static const uint32_t ACK_DISPLAY_MS = 3000;   // 回执画面停留时长(别太长, 免得盖住"请说命令")
static const char *NODE_ID = "M1";
#endif

enum OledState {
  OLED_BOOTING,
  OLED_WAIT_WAKE,
  OLED_LISTENING,
  OLED_JINJI_JIUMING,
  OLED_QINGQIU_JIUYUAN,
  OLED_YOUREN_BEIKUN,
  OLED_ACK_RECEIVED,
  OLED_ERROR,
};

static volatile OledState oled_requested_state = OLED_BOOTING;
static OledState oled_drawn_state = OLED_ERROR;
/* 屏不在(初始化探测失败)时必须彻底跳过绘制: 江科大驱动每次全屏刷新要做约 1000 次
   I2C 写, 屏不在时每次写都失败并打一行 E 级日志, 一次重画会阻塞主循环数秒,
   导致语音监听恢复和 LoRa 心跳/事件发送被推迟。 */
static bool oled_ready = false;

/* OLED 16x16 点阵字模，按 SSD1306 的上下两页排列。 */
static const uint8_t GLYPH_QU[32] = {
  0x00, 0xFF, 0x09, 0xE9, 0x09, 0x09, 0x09, 0xF9,
  0x09, 0x09, 0x09, 0xEF, 0x00, 0x00, 0x00, 0x00,
  0x18, 0x07, 0x3C, 0x21, 0x21, 0x21, 0x21, 0x3F,
  0x21, 0x21, 0x21, 0x21, 0x7C, 0x00, 0x00, 0x00,
};
static const uint8_t GLYPH_ZHAN[32] = {
  0x00, 0xFF, 0x09, 0x49, 0x49, 0xF9, 0x49, 0x49,
  0x49, 0xF9, 0x49, 0x49, 0x0F, 0x00, 0x00, 0x00,
  0x30, 0x0F, 0x02, 0x02, 0x7E, 0x23, 0x12, 0x06,
  0x0A, 0x13, 0x1A, 0x26, 0x22, 0x22, 0x00, 0x00,
};
static const uint8_t GLYPH_JIN[32] = {
  0x00, 0x1F, 0x80, 0x80, 0xDF, 0xA0, 0x81, 0xA3,
  0x95, 0x49, 0x15, 0x23, 0x21, 0x00, 0x00, 0x00,
  0x40, 0x24, 0x14, 0x04, 0x26, 0x46, 0x3D, 0x05,
  0x04, 0x14, 0x26, 0x4C, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GLYPH_JI[32] = {
  0x08, 0x04, 0x4A, 0x49, 0x49, 0x49, 0x49, 0x49,
  0x4D, 0x4B, 0x48, 0xF8, 0x00, 0x00, 0x00, 0x00,
  0x18, 0x02, 0x3A, 0x42, 0x42, 0x46, 0x5A, 0x42,
  0x42, 0x42, 0x72, 0x03, 0x08, 0x30, 0x00, 0x00,
};
static const uint8_t GLYPH_JIU[32] = {
  0x88, 0x08, 0xFF, 0x08, 0x89, 0x4A, 0x10, 0xEC,
  0x0B, 0x08, 0x08, 0xF8, 0x08, 0x08, 0x00, 0x00,
  0x24, 0x42, 0x3F, 0x02, 0x04, 0x48, 0x20, 0x10,
  0x0B, 0x04, 0x0B, 0x10, 0x20, 0x40, 0x00, 0x00,
};
static const uint8_t GLYPH_MING[32] = {
  0x20, 0x90, 0x90, 0x88, 0x94, 0x92, 0x11, 0x92,
  0x94, 0x88, 0x90, 0x90, 0x20, 0x20, 0x00, 0x00,
  0x00, 0x1F, 0x08, 0x08, 0x08, 0x1F, 0x00, 0x7F,
  0x00, 0x08, 0x10, 0x0F, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GLYPH_QING[32] = {
  0x21, 0xE6, 0x00, 0x00, 0x22, 0xAA, 0xAA, 0xAA,
  0xBF, 0xAA, 0xAA, 0xAA, 0x22, 0x20, 0x00, 0x00,
  0x00, 0x3F, 0x10, 0x08, 0x00, 0x7F, 0x0A, 0x0A,
  0x0A, 0x2A, 0x4A, 0x3F, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GLYPH_QIU[32] = {
  0x04, 0x14, 0x24, 0x44, 0x04, 0x84, 0xFF, 0x44,
  0x84, 0x44, 0x25, 0x14, 0x04, 0x00, 0x00, 0x00,
  0x08, 0x08, 0x04, 0x02, 0x21, 0x40, 0x3F, 0x00,
  0x00, 0x01, 0x02, 0x04, 0x08, 0x08, 0x00, 0x00,
};
static const uint8_t GLYPH_YUAN[32] = {
  0x08, 0xFF, 0x88, 0x48, 0x81, 0x93, 0x95, 0xF1,
  0x93, 0x94, 0x90, 0x94, 0x92, 0x80, 0x00, 0x00,
  0x41, 0x3F, 0x00, 0x20, 0x10, 0x4C, 0x43, 0x26,
  0x2A, 0x12, 0x2A, 0x26, 0x40, 0x40, 0x00, 0x00,
};
static const uint8_t GLYPH_YOU[32] = {
  0x02, 0x82, 0x42, 0xF2, 0x9E, 0x93, 0x92, 0x92,
  0x92, 0x92, 0xF2, 0x02, 0x02, 0x02, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x7F, 0x04, 0x04, 0x04, 0x04,
  0x24, 0x44, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GLYPH_REN[32] = {
  0x00, 0x00, 0x00, 0x00, 0x80, 0x60, 0x1F, 0x60,
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x10, 0x08, 0x06, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x06, 0x08, 0x10, 0x20, 0x40, 0x00, 0x00,
};
static const uint8_t GLYPH_BEI[32] = {
  0x84, 0x44, 0xF5, 0x4C, 0xA0, 0x00, 0xFC, 0x44,
  0xC4, 0x44, 0x7F, 0x44, 0xD4, 0x0C, 0x00, 0x00,
  0x00, 0x00, 0x7F, 0x00, 0x40, 0x30, 0x0F, 0x40,
  0x21, 0x16, 0x08, 0x16, 0x21, 0x40, 0x00, 0x00,
};
static const uint8_t GLYPH_KUN[32] = {
  0xFF, 0x01, 0x11, 0x11, 0x11, 0xD1, 0xFF, 0x51,
  0x91, 0x11, 0x11, 0x01, 0xFF, 0x00, 0x00, 0x00,
  0x7F, 0x20, 0x24, 0x22, 0x21, 0x20, 0x3F, 0x20,
  0x20, 0x21, 0x26, 0x20, 0x7F, 0x00, 0x00, 0x00,
};

static void requestOledState(OledState state) {
  oled_requested_state = state;
}

static void drawOledState(OledState state) {
  if (!oled_ready) {
    /* 没有屏: 记下当前状态即可, 不做任何 I2C 写 */
    oled_drawn_state = state;
    return;
  }

  OLED_Clear();

  switch (state) {
    case OLED_BOOTING:
      OLED_ShowString(1, 1, "OFFLINE KWS");
      OLED_ShowString(3, 1, "STARTING...");
      break;
    case OLED_WAIT_WAKE:
      OLED_ShowString(1, 1, "KWS READY");
      OLED_ShowChinese16(4, 48, GLYPH_QU);
      OLED_ShowChinese16(4, 64, GLYPH_ZHAN);
      break;
    case OLED_LISTENING:
      OLED_ShowString(1, 1, "WAKE UP");
      OLED_ShowString(3, 1, "SAY COMMAND");
      break;
    case OLED_JINJI_JIUMING:
      OLED_ShowString(1, 1, "DETECTED:");
      OLED_ShowChinese16(4, 32, GLYPH_JIN);
      OLED_ShowChinese16(4, 48, GLYPH_JI);
      OLED_ShowChinese16(4, 64, GLYPH_JIU);
      OLED_ShowChinese16(4, 80, GLYPH_MING);
      break;
    case OLED_QINGQIU_JIUYUAN:
      OLED_ShowString(1, 1, "DETECTED:");
      OLED_ShowChinese16(4, 32, GLYPH_QING);
      OLED_ShowChinese16(4, 48, GLYPH_QIU);
      OLED_ShowChinese16(4, 64, GLYPH_JIU);
      OLED_ShowChinese16(4, 80, GLYPH_YUAN);
      break;
    case OLED_YOUREN_BEIKUN:
      OLED_ShowString(1, 1, "DETECTED:");
      OLED_ShowChinese16(4, 32, GLYPH_YOU);
      OLED_ShowChinese16(4, 48, GLYPH_REN);
      OLED_ShowChinese16(4, 64, GLYPH_BEI);
      OLED_ShowChinese16(4, 80, GLYPH_KUN);
      break;
    case OLED_ACK_RECEIVED:
      /* 地面网关的收到确认。屏上只有这几个 16x16 自绘字模, 拼不出"地面已收到",
         所以用与 KWS READY 同风格的英文; 要中文需另加字模。 */
      OLED_ShowString(1, 1, "GROUND ACK");
      OLED_ShowString(3, 1, "RESCUE ON WAY");
      break;
    case OLED_ERROR:
      OLED_ShowString(1, 1, "START FAILED");
      OLED_ShowString(3, 1, "CHECK SERIAL");
      break;
  }
  oled_drawn_state = state;
}

static uint16_t latest_mq4 = 0;
static uint16_t latest_mq7 = 0;
static uint64_t latest_sensor_timestamp = 0;

#if ENABLE_LORA
static bool lora_ready = false;
static uint32_t next_lora_telemetry = 0;
static uint32_t lora_sequence = 0;
static volatile uint8_t pending_voice_event = 0;
/* 下行回执: 收到地面 ACK 后先显示回执画面, 到期再切回原来的画面 */
static uint32_t lora_downlink_count = 0;
static uint32_t ack_display_until = 0;                 // 0 = 当前没有回执画面
static OledState ack_restore_state = OLED_WAIT_WAKE;   // 回执结束后要恢复的画面
#endif

#if ENABLE_WIFI
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
static const uint32_t ONENET_RECONNECT_INTERVAL_MS = 5000;
static const uint32_t ONENET_TOKEN_LIFETIME_SEC = 7UL * 24UL * 60UL * 60UL;
static const char *ONENET_MQTT_HOST = "mqtts.heclouds.com";
static const uint16_t ONENET_MQTT_PORT = 1883;

static WiFiClient onenet_network;
static PubSubClient onenet_client(onenet_network);
static WebServer web_server(80);
static uint32_t next_onenet_reconnect = 0;
static uint32_t next_mq_upload = 0;
static bool web_server_started = false;
#endif

static uint16_t readAveragedAdc(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < MQ_ADC_SAMPLES; ++i) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return static_cast<uint16_t>(sum / MQ_ADC_SAMPLES);
}

static void sampleMqSensors() {
#if ENABLE_MQ_SENSORS
  latest_mq4 = readAveragedAdc(MQ4_PIN);
  latest_mq7 = readAveragedAdc(MQ7_PIN);
  latest_sensor_timestamp = static_cast<uint64_t>(millis());
#endif
}

static bool validateSrModelPartition() {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (part == nullptr) {
    Serial.println("[SR] 启动失败：分区表中没有 model 分区");
    return false;
  }

  uint32_t model_count = 0;
  char first_model[33] = {};
  if (esp_partition_read(part, 0, &model_count, sizeof(model_count)) != ESP_OK ||
      esp_partition_read(part, sizeof(model_count), first_model, 32) != ESP_OK) {
    Serial.println("[SR] 启动失败：无法读取 model 分区");
    return false;
  }

  Serial.printf("[SR] model 分区: offset=0x%08lX, size=%lu, 模型数=%lu, 首项=%s\n",
                static_cast<unsigned long>(part->address),
                static_cast<unsigned long>(part->size),
                static_cast<unsigned long>(model_count), first_model);
  if (model_count == 0 || model_count > 32 ||
      static_cast<uint8_t>(first_model[0]) == 0xFF || first_model[0] == '\0') {
    Serial.println("[SR] 启动失败：model 分区为空或模型包无效，请重新烧录 srmodels.bin");
    return false;
  }
  return true;
}

#if ENABLE_LORA
/* 带超时的发送。
   不能直接用库里的 LoRa.endPacket(): 它内部是
       while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0) { yield(); }
   没有超时。射频一旦异常(天线开路/短路、3.3V 供电不足), TX_DONE 永远不置位,
   主循环就永远停在这一行 —— 心跳停发、语音事件也发不出去, 地面侧看到的就是
   "节点1 离线"。endPacket 的等待也改不了(没有超时接口, isTransmitting/
   readRegister 都是私有), 所以异步启动发送, 固定等 LORA_TX_SETTLE_MS 让包发完,
   再 idle() 强制回待机: 无论射频是否正常都不会卡住。
   等待期间不能调 parsePacket —— 它发现芯片不在 RX 模式会强制切回 RX 打断发送。 */
static bool sendLoRaPacket(const String &packet) {
  if (!lora_ready) {
    Serial.printf("[LoRa] 未就绪，跳过: %s\n", packet.c_str());
    return false;
  }

  if (!LoRa.beginPacket()) {
    Serial.println("[LoRa] 无法开始发送");
    return false;
  }
  LoRa.print(packet);
  LoRa.endPacket(true);            // async: 只启动发送, 立即返回
  delay(LORA_TX_SETTLE_MS);        // 心跳 22 字节约 60ms, 100ms 足够
  LoRa.idle();                     // 幂等: 已完成就在待机, 没完成则强制放开
  Serial.printf("[LoRa] 已发送: %s\n", packet.c_str());
  return true;
}

/* 接收地面网关的下行报文。目前只有一种: ACK,<节点>
   endPacket() 后芯片回到 idle, parsePacket() 内部会切到 RX, 所以主循环里轮询即可。 */
static void handleLoRaDownlink() {
  if (!lora_ready) return;

  const int size = LoRa.parsePacket();
  if (size <= 0) return;

  String packet;
  packet.reserve(size + 1);
  while (LoRa.available()) packet += (char)LoRa.read();
  const int rssi = LoRa.packetRssi();
  packet.trim();
  Serial.printf("[LoRa] 收到下行: RSSI=%d 长度=%d  %s\n", rssi, size, packet.c_str());

  if (!packet.startsWith("ACK,")) {
    Serial.println("[LoRa] 未知下行报文，已忽略");
    return;
  }

  String target = packet.substring(4);
  target.trim();
  if (target.length() > 0 && target != NODE_ID) {
    Serial.printf("[回执] 目标 %s 不是本机(%s)，已忽略\n", target.c_str(), NODE_ID);
    return;
  }

  lora_downlink_count++;
  /* 记下当前画面, 回执到期后切回去; 连着收到多条回执时保持最初那个画面 */
  if (oled_requested_state != OLED_ACK_RECEIVED) {
    ack_restore_state = oled_requested_state;
  }
  requestOledState(OLED_ACK_RECEIVED);
  ack_display_until = millis() + ACK_DISPLAY_MS;
  Serial.printf("[回执] 地面网关已确认收到 (累计 %u 次)，屏幕保持 %u 秒\n",
                lora_downlink_count, (unsigned)(ACK_DISPLAY_MS / 1000));
}

/* 直接读 SX1278 的版本寄存器(0x42)，用于判断 LoRa 是接线问题还是芯片问题。 */
static uint8_t readSx1278Version() {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_PIN_NSS, LOW);
  SPI.transfer(0x42 & 0x7F);
  const uint8_t version = SPI.transfer(0x00);
  digitalWrite(LORA_PIN_NSS, HIGH);
  SPI.endTransaction();
  return version;
}

/* LoRa 失败时的引脚级自检：区分“MISO 断路悬空”和“MISO 被拉低/短路”，
   以及 RST/NSS 是否被外部拉死。 */
static void diagnoseLoRaPins() {
  pinMode(LORA_PIN_MISO, INPUT_PULLUP);
  delay(2);
  const int miso = digitalRead(LORA_PIN_MISO);
  const int rst = digitalRead(LORA_PIN_RST);
  const int nss = digitalRead(LORA_PIN_NSS);

  Serial.printf("[LoRa] 引脚自检: MISO(%d)=%d RST(%d)=%d NSS(%d)=%d\n",
                LORA_PIN_MISO, miso, LORA_PIN_RST, rst, LORA_PIN_NSS, nss);
  Serial.println(miso == 0
                   ? "[LoRa] MISO 为低 -> 该线被拉低/短路到 GND，或模块内部短路"
                   : "[LoRa] MISO 为高 -> 模块没驱动 MISO：没供电 / 没插好 / 转接板方向错 / 接线断路");
  if (rst == 0) {
    Serial.printf("[LoRa] RST 读回 0 -> GPIO%d 被拉死或短路，模块一直处于复位\n", LORA_PIN_RST);
  }
  if (nss == 0) {
    Serial.printf("[LoRa] NSS 读回 0 -> GPIO%d 被拉低或短路\n", LORA_PIN_NSS);
  }
}

static void initLoRa() {
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
  LoRa.setPins(LORA_PIN_NSS, LORA_PIN_RST, LORA_PIN_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY_HZ)) {
    /* 诊断：正常应读到 0x12。0x00=MISO 一直低(没供电或线接错)，
       0xFF=MISO 悬空(模块没响应)，其它值=不是 SX1278/Ra-02。 */
    Serial.printf("[LoRa] 未检测到 SX1278 (REG_VERSION=0x%02X)；检查 Ra-02 的 3.3V/GND、"
                  "NSS%d/MOSI%d/SCK%d/MISO%d/RST%d 以及转接板方向\n",
                  readSx1278Version(),
                  LORA_PIN_NSS, LORA_PIN_MOSI, LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_RST);
    diagnoseLoRaPins();
    return;
  }

  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  lora_ready = true;
  next_lora_telemetry = millis();
  Serial.println("[LoRa] SX1278 就绪: 433 MHz, sync=0xA3");
}

static void sendPendingVoiceEvent() {
  const uint8_t event = pending_voice_event;
  if (event == 0) return;
  pending_voice_event = 0;

  const char *event_code = nullptr;
  if (event == 1) event_code = "HELP";
  else if (event == 2) event_code = "RESCUE";
  else if (event == 3) event_code = "TRAPPED";
  /* 唤醒词命中: 也发给地面网关, 让它切到"请说命令"提示画面(非报警)。
     用字面量 4 是因为 CMD_QUZHAN 的 enum 在本函数之后才声明(与本函数里 1/2/3 同理)。 */
  else if (event == 4) event_code = "AWAKE";
  if (event_code == nullptr) return;

  const String packet = "VOICE," + String(NODE_ID) + "," + event_code + "," +
                        String(++lora_sequence);
  sendLoRaPacket(packet);
}

static void maintainLoRaTelemetry() {
  if (!lora_ready) return;
  const uint32_t now = millis();
  if ((int32_t)(now - next_lora_telemetry) < 0) return;

#if ENABLE_MQ_SENSORS
  sampleMqSensors();
  uint8_t flags = 0;
  if (latest_mq4 >= MQ4_ALERT_THRESHOLD) flags |= 0x01;
  if (latest_mq7 >= MQ7_ALERT_THRESHOLD) flags |= 0x02;

  const String packet = "DATA," + String(NODE_ID) + "," + String(latest_mq4) +
                        "," + String(latest_mq7) + "," + String(flags) + "," +
                        String(++lora_sequence);
#else
  const String packet = "STATUS," + String(NODE_ID) + ",KWS_READY," +
                        String(++lora_sequence);
#endif
  sendLoRaPacket(packet);
  next_lora_telemetry = now + LORA_TELEMETRY_INTERVAL_MS;
}
#endif

/* ESP-SR 固定要求 16kHz / 16bit / 单声道 */
#define I2S_SAMPLE_RATE 16000
#define SR_INPUT_FORMAT "M"
#define SR_INPUT_CHANNELS SR_CHANNELS_MONO
#define I2S_OUTPUT_CHANNELS I2S_SLOT_MODE_MONO

I2SClass i2s;

enum {
  CMD_JINJI_JIUMING = 1,
  CMD_QINGQIU_JIUYUAN = 2,
  CMD_YOUREN_BEIKUN = 3,
  CMD_QUZHAN = 4,
};

static const sr_cmd_t sr_commands[] = {
  {CMD_JINJI_JIUMING, "jin ji jiu ming"},
  {CMD_QINGQIU_JIUYUAN, "qing qiu jiu yuan"},
  {CMD_YOUREN_BEIKUN, "you ren bei kun"},
  {CMD_QUZHAN, "qu zhan"},
};

/*
 * 唤醒后持续接收命令。
 * 每次命中后短暂停止识别，滤掉同一句话的尾音，随后自动继续监听。
 */
static volatile bool command_window_open = false;
static volatile bool command_session_active = false;
static volatile bool command_resume_pending = false;
static volatile uint32_t command_resume_at = 0;
static volatile uint32_t command_session_expires_at = 0;
static const uint32_t COMMAND_DEBOUNCE_MS = 1000;
static const uint32_t COMMAND_SESSION_MS = 15000;
/* 唤醒词保持 0.72，三个命令词使用更灵敏的 0.68。 */
static const float WAKE_DETECTION_THRESHOLD = 0.72f;
static const float COMMAND_DETECTION_THRESHOLD = 0.68f;

void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  (void)phrase_id;

  switch (event) {
    case SR_EVENT_COMMAND:
      if (!command_window_open) {
        break;
      }

      /* 先关闭识别；即使队列中还有同一句的重复结果，也不会再次输出。 */
      command_window_open = false;

      if (!command_session_active) {
        /* 待机时只响应“屈展”，忽略其他命令。 */
        if (command_id == CMD_QUZHAN) {
          command_session_active = true;
          command_session_expires_at = millis() + COMMAND_SESSION_MS;
          ESP_SR.setDetectionThreshold(COMMAND_DETECTION_THRESHOLD);
          requestOledState(OLED_LISTENING);
          Serial.println("[KWS] 已唤醒，请说命令");
#if ENABLE_LORA
          /* 通知地面网关切到"请说命令"画面 */
          pending_voice_event = CMD_QUZHAN;
#endif
        }
      } else {
        if (command_id == CMD_JINJI_JIUMING) {
          Serial.println("[KWS] 紧急救命");
          requestOledState(OLED_JINJI_JIUMING);
          command_session_expires_at = millis() + COMMAND_SESSION_MS;
#if ENABLE_LORA
          pending_voice_event = CMD_JINJI_JIUMING;
#endif
        } else if (command_id == CMD_QINGQIU_JIUYUAN) {
          Serial.println("[KWS] 请求救援");
          requestOledState(OLED_QINGQIU_JIUYUAN);
          command_session_expires_at = millis() + COMMAND_SESSION_MS;
#if ENABLE_LORA
          pending_voice_event = CMD_QINGQIU_JIUYUAN;
#endif
        } else if (command_id == CMD_YOUREN_BEIKUN) {
          Serial.println("[KWS] 有人被困");
          requestOledState(OLED_YOUREN_BEIKUN);
          command_session_expires_at = millis() + COMMAND_SESSION_MS;
#if ENABLE_LORA
          pending_voice_event = CMD_YOUREN_BEIKUN;
#endif
        }
      }
      /* 库在命中后已经进入 OFF；消抖结束后由 loop() 恢复命令监听。 */
      command_resume_at = millis() + COMMAND_DEBOUNCE_MS;
      command_resume_pending = true;
      break;
    case SR_EVENT_TIMEOUT:
      command_resume_pending = false;
      /* 软件唤醒和命令共用 MultiNet，超时后始终开始下一轮监听。 */
      command_window_open = true;
      ESP_SR.setMode(SR_MODE_COMMAND);
      break;
    default:
      break;
  }
}

#if ENABLE_WIFI
static String urlEncode(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

static String createOneNetToken() {
  const time_t now = time(nullptr);
  if (now <= 1700000000) return "";

  const String version = "2018-10-31";
  const String method = "sha1";
  const String resource = "products/" + String(ONENET_PRODUCT_ID) +
                          "/devices/" + String(ONENET_DEVICE_NAME);
  const String expiry = String(static_cast<unsigned long>(now) +
                               ONENET_TOKEN_LIFETIME_SEC);
  const String string_to_sign = expiry + "\n" + method + "\n" + resource +
                                "\n" + version;

  unsigned char decoded_key[96];
  size_t decoded_key_length = 0;
  if (mbedtls_base64_decode(
        decoded_key, sizeof(decoded_key), &decoded_key_length,
        reinterpret_cast<const unsigned char *>(ONENET_DEVICE_SECRET),
        strlen(ONENET_DEVICE_SECRET)) != 0) {
    Serial.println("[OneNET] 设备密钥 Base64 解码失败");
    return "";
  }

  unsigned char signature[20];
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (mbedtls_md_hmac(
        md_info, decoded_key, decoded_key_length,
        reinterpret_cast<const unsigned char *>(string_to_sign.c_str()),
        string_to_sign.length(), signature) != 0) {
    Serial.println("[OneNET] Token 签名失败");
    return "";
  }

  unsigned char base64_signature[64];
  size_t base64_length = 0;
  if (mbedtls_base64_encode(
        base64_signature, sizeof(base64_signature) - 1, &base64_length,
        signature, sizeof(signature)) != 0) {
    Serial.println("[OneNET] Token Base64 编码失败");
    return "";
  }
  base64_signature[base64_length] = '\0';

  return "version=" + urlEncode(version) +
         "&res=" + urlEncode(resource) +
         "&et=" + expiry +
         "&method=" + method +
         "&sign=" + urlEncode(String(reinterpret_cast<char *>(base64_signature)));
}

static void sendGzipAsset(const char *content_type, const uint8_t *data, size_t length) {
  web_server.sendHeader("Content-Encoding", "gzip");
  web_server.sendHeader("Cache-Control", "public, max-age=3600");
  web_server.setContentLength(length);
  web_server.send(200, content_type, "");
  WiFiClient client = web_server.client();
  client.write(data, length);
}

static void startWebServer() {
  if (web_server_started) return;
  web_server.on("/", HTTP_GET, []() {
    sendGzipAsset("text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  web_server.on("/index.html", HTTP_GET, []() {
    sendGzipAsset("text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  web_server.on("/app.js", HTTP_GET, []() {
    sendGzipAsset("text/javascript; charset=utf-8", APP_JS_GZ, APP_JS_GZ_LEN);
  });
  web_server.on("/favicon.jpeg", HTTP_GET, []() {
    sendGzipAsset("image/jpeg", FAVICON_JPEG_GZ, FAVICON_JPEG_GZ_LEN);
  });
  web_server.on("/api/sensors", HTTP_GET, []() {
    const uint64_t timestamp = time(nullptr) > 1700000000
      ? static_cast<uint64_t>(time(nullptr)) * 1000ULL
      : latest_sensor_timestamp;
    String response = "{\"productId\":\"" + String(ONENET_PRODUCT_ID) +
                      "\",\"deviceName\":\"" + String(ONENET_DEVICE_NAME) +
                      "\",\"mq4\":" + String(latest_mq4) +
                      ",\"mq7\":" + String(latest_mq7) +
                      ",\"updatedAt\":" + String(timestamp) + "}";
    web_server.sendHeader("Cache-Control", "no-store");
    web_server.send(200, "application/json; charset=utf-8", response);
  });
  web_server.onNotFound([]() {
    web_server.send(404, "text/plain; charset=utf-8", "Not found");
  });
  web_server.begin();
  web_server_started = true;
  Serial.printf("[Web] 仪表盘已启动: http://%s/\n", WiFi.localIP().toString().c_str());
}

static bool connectOneNet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (onenet_client.connected()) return true;

  // A fixed OneNET Token avoids blocking initial connection when a phone
  // hotspot or local network does not allow NTP. If no fixed Token is
  // configured, fall back to generating one from the device secret.
  const String token = strlen(ONENET_DEVICE_TOKEN) > 0
                         ? String(ONENET_DEVICE_TOKEN)
                         : createOneNetToken();
  if (token.isEmpty()) {
    Serial.println("[OneNET] 等待网络校时...");
    return false;
  }

  Serial.println("[OneNET] 正在连接...");
  const bool connected = onenet_client.connect(
    ONENET_DEVICE_NAME, ONENET_PRODUCT_ID, token.c_str());
  if (connected) {
    Serial.println("[OneNET] 已连接，设备将自动激活");
  } else {
    Serial.printf("[OneNET] 连接失败, MQTT state=%d\n", onenet_client.state());
  }
  return connected;
}

static void uploadMqSensors() {
  if (!onenet_client.connected()) return;

  sampleMqSensors();
  const String topic = "$sys/" + String(ONENET_PRODUCT_ID) + "/" +
                       String(ONENET_DEVICE_NAME) + "/thing/property/post";
  String payload = "{\"id\":\"" + String(millis()) +
                   "\",\"version\":\"1.0\",\"params\":{";
  payload += "\"mq4\":{\"value\":" + String(latest_mq4) + "},";
  payload += "\"mq7\":{\"value\":" + String(latest_mq7) + "}}}";

  if (onenet_client.publish(topic.c_str(), payload.c_str())) {
    Serial.printf("[OneNET] 已上报 mq4=%u, mq7=%u\n", latest_mq4, latest_mq7);
  } else {
    Serial.printf("[OneNET] 上报失败, MQTT state=%d\n", onenet_client.state());
  }
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WiFi] 正在连接热点 %s", WIFI_SSID);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] 连接成功, IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
    sampleMqSensors();
    startWebServer();
  } else {
    Serial.printf("[WiFi] 连接失败 (status=%d)，继续语音识别\n",
                  (int)WiFi.status());
    WiFi.disconnect(false, false);
  }
}

static void maintainWiFi() {
  static uint32_t nextReconnect = 0;
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!web_server_started) startWebServer();
    nextReconnect = now + WIFI_RECONNECT_INTERVAL_MS;
    return;
  }
  if ((int32_t)(now - nextReconnect) >= 0) {
    Serial.println("[WiFi] 已断开，开始重连...");
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    nextReconnect = now + WIFI_RECONNECT_INTERVAL_MS;
  }
}

static void maintainOneNet() {
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t now = millis();
  if (!onenet_client.connected()) {
    if ((int32_t)(now - next_onenet_reconnect) >= 0) {
      connectOneNet();
      next_onenet_reconnect = now + ONENET_RECONNECT_INTERVAL_MS;
    }
    return;
  }

  onenet_client.loop();
  if ((int32_t)(now - next_mq_upload) >= 0) {
    uploadMqSensors();
    next_mq_upload = now + MQ_UPLOAD_INTERVAL_MS;
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[KWS] 启动中...");

  oled_ready = OLED_Init(Wire, OLED_PIN_SDA, OLED_PIN_SCL, OLED_I2C_ADDRESS);
  if (oled_ready) {
    drawOledState(OLED_BOOTING);
  } else {
    Serial.println("[OLED] 未检测到屏幕，请检查接线和地址0x3C（本次运行将跳过所有屏幕刷新）");
  }

#if ENABLE_MQ_SENSORS
  analogReadResolution(12);
  analogSetPinAttenuation(MQ4_PIN, ADC_11db);
  analogSetPinAttenuation(MQ7_PIN, ADC_11db);
  sampleMqSensors();
#else
  Serial.println("[MQ] 已停用：GPIO4/GPIO5 无需接线");
#endif

#if ENABLE_LORA
  initLoRa();
#endif

#if ENABLE_WIFI
  connectWiFi();
  onenet_client.setServer(ONENET_MQTT_HOST, ONENET_MQTT_PORT);
  onenet_client.setBufferSize(512);
  onenet_client.setKeepAlive(30);
  onenet_client.setSocketTimeout(2);
#endif

  Serial.println("[KWS] 正在检查 PSRAM...");
  if (!esp_psram_is_initialized() || esp_psram_get_size() == 0) {
    /* Arduino 启动阶段应当已经初始化 PSRAM。这里不再手动调用
       esp_psram_init()，避免无 PSRAM 或型号配置错误时无提示卡死。 */
    Serial.println("[KWS] 启动失败：未检测到 PSRAM；请核对模组是否为 N16R8");
    requestOledState(OLED_ERROR);
    return;
  }
  Serial.printf("[KWS] PSRAM 正常: %u bytes\n",
                static_cast<unsigned>(esp_psram_get_size()));

  Serial.println("[I2S] 正在初始化 INMP441...");
  i2s.setTimeout(1000);
  i2s.setPins(I2S_PIN_BCK, I2S_PIN_WS, -1, I2S_PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_OUTPUT_CHANNELS, I2S_STD_SLOT_LEFT) ||
      !i2s.configureRX(I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                       I2S_OUTPUT_CHANNELS, I2S_RX_TRANSFORM_32_TO_16)) {
    Serial.println("[I2S] 启动失败：请检查 GPIO15/16/17 和 INMP441 供电");
    requestOledState(OLED_ERROR);
    return;
  }
  Serial.println("[I2S] 接口初始化完成");

  if (!validateSrModelPartition()) {
    requestOledState(OLED_ERROR);
    return;
  }
  Serial.println("[SR] 正在加载中文命令词模型...");
  ESP_SR.onEvent(onSrEvent);
  if (!ESP_SR.begin(i2s, sr_commands, sizeof(sr_commands) / sizeof(sr_cmd_t),
                    SR_INPUT_CHANNELS, SR_MODE_COMMAND, SR_INPUT_FORMAT)) {
    Serial.println("[SR] 启动失败! 请确认 model 分区已烧入 srmodels.bin");
    requestOledState(OLED_ERROR);
  } else {
    command_window_open = true;
    if (!ESP_SR.setDetectionThreshold(WAKE_DETECTION_THRESHOLD)) {
      Serial.println("[KWS] 警告：识别阈值设置失败");
    }
    Serial.println("[KWS] 就绪，请说：屈展");
    requestOledState(OLED_WAIT_WAKE);
  }
}

void loop() {
#if ENABLE_LORA
  handleLoRaDownlink();

  /* 回执画面到期: 切回收到回执之前的画面。
     期间若又识别到语音(画面已被改掉), 就不动它, 避免把新画面顶回旧的。 */
  if (ack_display_until != 0 && (int32_t)(millis() - ack_display_until) >= 0) {
    ack_display_until = 0;
    if (oled_requested_state == OLED_ACK_RECEIVED) {
      requestOledState(ack_restore_state);
    }
  }

  sendPendingVoiceEvent();
  maintainLoRaTelemetry();
#endif

#if ENABLE_WIFI
  maintainWiFi();
  maintainOneNet();
  web_server.handleClient();
#endif

  if (command_session_active &&
      (int32_t)(millis() - command_session_expires_at) >= 0) {
    command_session_active = false;
    ESP_SR.setDetectionThreshold(WAKE_DETECTION_THRESHOLD);
    requestOledState(OLED_WAIT_WAKE);
    Serial.println("[KWS] 已休眠，请说：屈展");
  }

  const OledState requested_state = oled_requested_state;
  if (requested_state != oled_drawn_state) {
    drawOledState(requested_state);
  }

  if (command_resume_pending &&
      (int32_t)(millis() - command_resume_at) >= 0) {
    command_resume_pending = false;
    command_window_open = true;
    ESP_SR.setMode(SR_MODE_COMMAND);
  }

  delay(20);
}
