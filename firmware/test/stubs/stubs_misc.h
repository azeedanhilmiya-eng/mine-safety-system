#pragma once
#include "Arduino.h"

// --- SPI / LoRa ---
class SPIClass {
 public:
  void begin(int, int, int, int) {}
};
extern SPIClass SPI;

class LoRaClass {
 public:
  bool begin(long) { return true; }
  void setPins(int, int, int) {}
  void setSyncWord(uint8_t) {}
  void enableCrc() {}
  void beginPacket() {}
  void endPacket() {}
  void print(const char *) {}
  void print(const String &) {}
  int parsePacket() { return 0; }
  int available() { return 0; }
  int read() { return -1; }
  int packetRssi() { return -70; }
  void receive() {}
};
extern LoRaClass LoRa;

// --- WiFi ---
#define WIFI_STA 1
#define WL_CONNECTED 3
class WiFiClass {
 public:
  void mode(int) {}
  void begin(const char *, const char *) {}
  int status() { return WL_CONNECTED; }
};
extern WiFiClass WiFi;

class WiFiClientSecure {
 public:
  void setInsecure() {}
};

// --- HTTP ---
#define HTTP_CODE_OK 200
#define HTTP_CODE_NO_CONTENT 204
class HTTPClient {
 public:
  void begin(WiFiClientSecure &, const String &) {}
  void addHeader(const char *, const char *) {}
  int PUT(const String &) { return 200; }
  int POST(const String &) { return 200; }
  void end() {}
};

// --- Wire / U8g2 ---
class TwoWire {
 public:
  void setPins(int, int) {}
  void begin(int, int) {}
};
extern TwoWire Wire;

typedef const uint8_t *u8g2_font_t;
extern const uint8_t u8g2_font_6x10_tf[];
extern const uint8_t u8g2_font_ncenB14_tr[];
extern const uint8_t u8g2_font_wqy12_t_gb2312[];
#define U8G2_R0 0
#define U8X8_PIN_NONE 255

class U8G2_SH1106_128X64_NONAME_F_HW_I2C {
 public:
  U8G2_SH1106_128X64_NONAME_F_HW_I2C(int, uint8_t) {}
  void begin() {}
  void clearBuffer() {}
  void sendBuffer() {}
  void setFont(const uint8_t *) {}
  void drawStr(int, int, const char *) {}
  void drawUTF8(int, int, const char *) {}
};
