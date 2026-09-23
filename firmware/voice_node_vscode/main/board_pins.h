#pragma once

#include "driver/gpio.h"

// INMP441: VDD=3V3, GND=GND, L/R=GND (left channel)
#define PIN_I2S_BCLK        GPIO_NUM_15
#define PIN_I2S_WS          GPIO_NUM_16
#define PIN_I2S_DATA_IN     GPIO_NUM_17

// Press-to-talk button: GPIO7 to GND, internal pull-up enabled.
#define PIN_PTT_BUTTON      GPIO_NUM_7

// Optional status LED: GPIO21 -> 330 ohm -> LED -> GND.
#define PIN_STATUS_LED      GPIO_NUM_21

// Ai-Thinker Ra-02 / SX1278, 433 MHz.
#define PIN_LORA_DIO0       GPIO_NUM_9
#define PIN_LORA_NSS        GPIO_NUM_10
#define PIN_LORA_MOSI       GPIO_NUM_11
#define PIN_LORA_SCK        GPIO_NUM_12
#define PIN_LORA_MISO       GPIO_NUM_13
#define PIN_LORA_RST        GPIO_NUM_14

