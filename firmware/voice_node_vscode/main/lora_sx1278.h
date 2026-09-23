#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t lora_sx1278_init(void);
esp_err_t lora_sx1278_send(const void *data, size_t length);

