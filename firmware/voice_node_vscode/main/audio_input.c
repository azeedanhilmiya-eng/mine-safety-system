#include "audio_input.h"

#include <stdlib.h>
#include "board_pins.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx_handle;
static int32_t *s_raw_samples;
static size_t s_raw_capacity;

esp_err_t audio_input_init(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_handle), TAG,
                        "cannot create I2S RX channel");

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DATA_IN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &config), TAG,
                        "cannot configure INMP441 I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG,
                        "cannot enable INMP441 I2S");

    ESP_LOGI(TAG, "INMP441 ready: 16 kHz, mono, BCLK=%d WS=%d SD=%d",
             PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DATA_IN);
    return ESP_OK;
}

esp_err_t audio_input_read(int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_count > s_raw_capacity) {
        int32_t *new_buffer = heap_caps_realloc(
            s_raw_samples, sample_count * sizeof(int32_t), MALLOC_CAP_8BIT);
        if (!new_buffer) {
            return ESP_ERR_NO_MEM;
        }
        s_raw_samples = new_buffer;
        s_raw_capacity = sample_count;
    }

    size_t bytes_read = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_read(
                            s_rx_handle,
                            s_raw_samples,
                            sample_count * sizeof(int32_t),
                            &bytes_read,
                            portMAX_DELAY),
                        TAG, "I2S read failed");

    const size_t received = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < received; ++i) {
        // INMP441 returns signed 24-bit samples in a 32-bit slot. The shift
        // converts them to signed 16-bit PCM and provides moderate gain.
        int32_t value = s_raw_samples[i] >> 13;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        samples[i] = (int16_t)value;
    }
    for (size_t i = received; i < sample_count; ++i) {
        samples[i] = 0;
    }

    return received == sample_count ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
