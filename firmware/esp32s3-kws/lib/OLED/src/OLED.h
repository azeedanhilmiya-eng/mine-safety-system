#pragma once

#include <Arduino.h>
#include <Wire.h>

bool OLED_Init(TwoWire &wire, int sda, int scl, uint8_t address = 0x3C);
void OLED_Clear(void);
void OLED_ShowChar(uint8_t line, uint8_t column, char value);
void OLED_ShowString(uint8_t line, uint8_t column, const char *value);
void OLED_ShowChinese16(uint8_t page, uint8_t x, const uint8_t glyph[32]);
