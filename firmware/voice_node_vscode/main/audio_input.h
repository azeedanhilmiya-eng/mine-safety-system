#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000

esp_err_t audio_input_init(void);
esp_err_t audio_input_read(int16_t *samples, size_t sample_count);

