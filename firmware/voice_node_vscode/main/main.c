#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_input.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora_sx1278.h"
#include "model_path.h"

#define NODE_ID                 "M1"
#define MIN_CONFIDENCE          0.55f
#define COMMAND_WINDOW_MS       2500

typedef struct {
    int command_id;
    const char *spoken;
    const char *event_code;
} command_mapping_t;

static const command_mapping_t COMMANDS[] = {
    {1, "救命", "HELP"},
    {2, "撤离", "EVACUATE"},
    {3, "瓦斯", "GAS"},
    {4, "透水", "WATER"},
};

static const char *TAG = "voice_node";
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_model_data;
static uint32_t s_event_sequence;
static bool s_lora_ready;

static const command_mapping_t *find_command(int command_id)
{
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        if (COMMANDS[i].command_id == command_id) return &COMMANDS[i];
    }
    return NULL;
}

static void blink_feedback(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        gpio_set_level(PIN_STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(90));
        gpio_set_level(PIN_STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

static void transmit_voice_event(const command_mapping_t *command, float confidence)
{
    char packet[96];
    const unsigned confidence_percent = (unsigned)(confidence * 100.0f + 0.5f);
    const uint32_t sequence = ++s_event_sequence;

    // ASCII keeps the packet small and avoids OLED font/encoding problems.
    // Example: VOICE,M1,HELP,91,1
    int length = snprintf(packet, sizeof(packet), "VOICE,%s,%s,%u,%lu",
                          NODE_ID, command->event_code, confidence_percent,
                          (unsigned long)sequence);
    if (length <= 0 || (size_t)length >= sizeof(packet)) {
        ESP_LOGE(TAG, "event packet formatting failed");
        return;
    }

    ESP_LOGI(TAG, "识别=%s event=%s confidence=%u%% seq=%lu",
             command->spoken, command->event_code, confidence_percent,
             (unsigned long)sequence);

    if (!s_lora_ready) {
        ESP_LOGW(TAG, "LoRa unavailable; result shown on serial only");
        return;
    }

    esp_err_t err = lora_sx1278_send(packet, (size_t)length);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LoRa TX: %s", packet);
    } else {
        ESP_LOGE(TAG, "LoRa TX failed: %s", esp_err_to_name(err));
    }
}

static void configure_commands(void)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(s_multinet, s_model_data));
    ESP_ERROR_CHECK(esp_mn_commands_clear());
    ESP_ERROR_CHECK(esp_mn_commands_add(1, "jiu ming"));
    ESP_ERROR_CHECK(esp_mn_commands_add(2, "che li"));
    ESP_ERROR_CHECK(esp_mn_commands_add(3, "wa si"));
    ESP_ERROR_CHECK(esp_mn_commands_add(4, "tou shui"));

    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors) {
        ESP_LOGE(TAG, "one or more Chinese command phrases could not be parsed");
        abort();
    }
    s_multinet->print_active_speech_commands(s_model_data);
}

static void audio_feed_task(void *argument)
{
    (void)argument;
    const int chunk_samples = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *audio = heap_caps_malloc(chunk_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!audio) abort();

    while (true) {
        if (audio_input_read(audio, chunk_samples) == ESP_OK) {
            s_afe->feed(s_afe_data, audio);
        } else {
            ESP_LOGE(TAG, "audio input failed; check microphone wiring");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void command_detect_task(void *argument)
{
    (void)argument;
    bool listening = false;
    bool wait_for_release = false;
    TickType_t listen_deadline = 0;

    while (true) {
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            blink_feedback(5);
            continue;
        }

        const bool button_pressed = gpio_get_level(PIN_PTT_BUTTON) == 0;
        if (!button_pressed) {
            wait_for_release = false;
        }
        if (button_pressed && !listening && !wait_for_release) {
            listening = true;
            listen_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_WINDOW_MS);
            s_multinet->clean(s_model_data);
            gpio_set_level(PIN_STATUS_LED, 1);
            ESP_LOGI(TAG, "正在监听：请说 救命/撤离/瓦斯/透水");
        }

        if (!listening) continue;

        esp_mn_state_t state = s_multinet->detect(s_model_data, result->data);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *recognition = s_multinet->get_results(s_model_data);
            gpio_set_level(PIN_STATUS_LED, 0);
            listening = false;
            wait_for_release = true;

            if (recognition && recognition->num > 0) {
                const command_mapping_t *command = find_command(recognition->command_id[0]);
                const float confidence = recognition->prob[0];
                if (command && confidence >= MIN_CONFIDENCE) {
                    transmit_voice_event(command, confidence);
                    blink_feedback((unsigned)command->command_id);
                } else {
                    ESP_LOGW(TAG, "低置信度或未知命令: %.3f", confidence);
                    blink_feedback(5);
                }
            }
            continue;
        }

        if (!button_pressed || state == ESP_MN_STATE_TIMEOUT ||
            xTaskGetTickCount() >= listen_deadline) {
            gpio_set_level(PIN_STATUS_LED, 0);
            listening = false;
            wait_for_release = true;
            s_multinet->clean(s_model_data);
            ESP_LOGW(TAG, "未识别到有效命令");
        }
    }
}

void app_main(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << PIN_PTT_BUTTON) | (1ULL << PIN_STATUS_LED),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_config));
    gpio_set_direction(PIN_PTT_BUTTON, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_STATUS_LED, 0);

    ESP_ERROR_CHECK(audio_input_init());
    s_lora_ready = lora_sx1278_init() == ESP_OK;
    if (!s_lora_ready) {
        ESP_LOGW(TAG, "continuing without LoRa so voice recognition can be tested");
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "ESP-SR model partition not found; use full Upload, not app-only upload");
        abort();
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) abort();
    // The physical PTT button opens the command window; no wake word is needed.
    afe_config->wakenet_init = false;
    afe_config->aec_init = false;
    s_afe = esp_afe_handle_from_config(afe_config);
    if (!s_afe) abort();
    s_afe_data = s_afe->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!s_afe || !s_afe_data) abort();

    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!model_name) {
        ESP_LOGE(TAG, "Chinese MultiNet model is not present in model partition");
        abort();
    }
    s_multinet = esp_mn_handle_from_name(model_name);
    if (!s_multinet) abort();
    s_model_data = s_multinet->create(model_name, 6000);
    if (!s_model_data) abort();

    if (s_afe->get_fetch_chunksize(s_afe_data) !=
        s_multinet->get_samp_chunksize(s_model_data)) {
        ESP_LOGE(TAG, "AFE and MultiNet audio frame sizes do not match");
        abort();
    }
    ESP_LOGI(TAG, "model=%s free PSRAM=%u bytes", model_name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    configure_commands();

    ESP_LOGI(TAG, "voice node ready; hold GPIO7 button and speak a command");
    if (xTaskCreatePinnedToCore(audio_feed_task, "audio_feed", 6 * 1024, NULL, 6, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(command_detect_task, "command_detect", 10 * 1024, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "not enough memory to create audio tasks");
        abort();
    }
}
