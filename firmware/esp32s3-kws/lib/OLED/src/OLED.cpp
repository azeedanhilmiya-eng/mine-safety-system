#include "OLED.h"

#include <Wire.h>
#include "OLED_Font.h"

static TwoWire *oled_wire = nullptr;
static uint8_t oled_address = 0x3C;

static bool OLED_Write(uint8_t control, uint8_t value) {
  if (oled_wire == nullptr) {
    return false;
  }
  oled_wire->beginTransmission(oled_address);
  oled_wire->write(control);
  oled_wire->write(value);
  return oled_wire->endTransmission() == 0;
}

static bool OLED_WriteCommand(uint8_t command) {
  return OLED_Write(0x00, command);
}

static bool OLED_WriteData(uint8_t data) {
  return OLED_Write(0x40, data);
}

static void OLED_SetCursor(uint8_t y, uint8_t x) {
  OLED_WriteCommand(0xB0 | y);
  OLED_WriteCommand(0x10 | ((x & 0xF0) >> 4));
  OLED_WriteCommand(x & 0x0F);
}

void OLED_Clear(void) {
  for (uint8_t page = 0; page < 8; ++page) {
    OLED_SetCursor(page, 0);
    for (uint8_t x = 0; x < 128; ++x) {
      OLED_WriteData(0x00);
    }
  }
}

void OLED_ShowChar(uint8_t line, uint8_t column, char value) {
  if (line < 1 || line > 4 || column < 1 || column > 16 ||
      value < ' ' || value > '~') {
    return;
  }

  const uint8_t index = static_cast<uint8_t>(value - ' ');
  OLED_SetCursor((line - 1) * 2, (column - 1) * 8);
  for (uint8_t i = 0; i < 8; ++i) {
    OLED_WriteData(OLED_F8x16[index][i]);
  }
  OLED_SetCursor((line - 1) * 2 + 1, (column - 1) * 8);
  for (uint8_t i = 0; i < 8; ++i) {
    OLED_WriteData(OLED_F8x16[index][i + 8]);
  }
}

void OLED_ShowString(uint8_t line, uint8_t column, const char *value) {
  if (value == nullptr) {
    return;
  }
  for (uint8_t i = 0; value[i] != '\0' && column + i <= 16; ++i) {
    OLED_ShowChar(line, column + i, value[i]);
  }
}

void OLED_ShowChinese16(uint8_t page, uint8_t x,
                        const uint8_t glyph[32]) {
  if (glyph == nullptr || page > 6 || x > 112) {
    return;
  }

  OLED_SetCursor(page, x);
  for (uint8_t i = 0; i < 16; ++i) {
    OLED_WriteData(glyph[i]);
  }
  OLED_SetCursor(page + 1, x);
  for (uint8_t i = 0; i < 16; ++i) {
    OLED_WriteData(glyph[i + 16]);
  }
}

bool OLED_Init(TwoWire &wire, int sda, int scl, uint8_t address) {
  oled_wire = &wire;
  oled_address = address;
  oled_wire->begin(sda, scl);
  oled_wire->setClock(400000);

  oled_wire->beginTransmission(oled_address);
  if (oled_wire->endTransmission() != 0) {
    return false;
  }

  const uint8_t init_commands[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
    0xDB, 0x30, 0xA4, 0xA6, 0x8D, 0x14, 0xAF,
  };
  for (uint8_t command : init_commands) {
    OLED_WriteCommand(command);
  }
  OLED_Clear();
  return true;
}
