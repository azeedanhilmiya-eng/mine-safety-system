#include "lora_sx1278.h"

#include <stdint.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_LNA                  0x0C
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_IRQ_FLAGS            0x12
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_VERSION              0x42

#define MODE_LONG_RANGE          0x80
#define MODE_SLEEP               0x00
#define MODE_STANDBY             0x01
#define MODE_TX                  0x03
#define IRQ_TX_DONE              0x08

static const char *TAG = "lora";
static spi_device_handle_t s_radio;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(reg | 0x80), value};
    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_transmit(s_radio, &transaction);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(reg & 0x7F), 0};
    uint8_t rx[2] = {0};
    spi_transaction_t transaction = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s_radio, &transaction);
    if (err == ESP_OK) *value = rx[1];
    return err;
}

static esp_err_t write_fifo(const uint8_t *data, size_t length)
{
    if (length == 0 || length > 255) return ESP_ERR_INVALID_SIZE;

    uint8_t tx[256];
    tx[0] = REG_FIFO | 0x80;
    for (size_t i = 0; i < length; ++i) tx[i + 1] = data[i];

    spi_transaction_t transaction = {
        .length = (length + 1) * 8,
        .tx_buffer = tx,
    };
    return spi_device_transmit(s_radio, &transaction);
}

esp_err_t lora_sx1278_init(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << PIN_LORA_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "RST GPIO failed");

    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_LORA_MOSI,
        .miso_io_num = PIN_LORA_MISO,
        .sclk_io_num = PIN_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_LORA_NSS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device_config, &s_radio),
                        TAG, "SX1278 add device failed");

    uint8_t version = 0;
    ESP_RETURN_ON_ERROR(read_reg(REG_VERSION, &version), TAG, "version read failed");
    if (version != 0x12) {
        ESP_LOGE(TAG, "SX1278 not found; REG_VERSION=0x%02X", version);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_SLEEP), TAG, "sleep failed");
    // 433 MHz with a 32 MHz crystal: FRF = 433000000 / (32000000 / 2^19) = 0x6C4000.
    ESP_RETURN_ON_ERROR(write_reg(REG_FRF_MSB, 0x6C), TAG, "frequency failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_FRF_MID, 0x40), TAG, "frequency failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_FRF_LSB, 0x00), TAG, "frequency failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_FIFO_TX_BASE_ADDR, 0x00), TAG, "FIFO base failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_LNA, 0x23), TAG, "LNA failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_MODEM_CONFIG_1, 0x72), TAG, "modem config failed"); // BW125, CR4/5, explicit header
    ESP_RETURN_ON_ERROR(write_reg(REG_MODEM_CONFIG_2, 0x74), TAG, "modem config failed"); // SF7, CRC enabled
    ESP_RETURN_ON_ERROR(write_reg(REG_MODEM_CONFIG_3, 0x04), TAG, "modem config failed"); // AGC on
    ESP_RETURN_ON_ERROR(write_reg(REG_PREAMBLE_MSB, 0x00), TAG, "preamble failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_PREAMBLE_LSB, 0x08), TAG, "preamble failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_SYNC_WORD, 0xA3), TAG, "sync word failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_PA_CONFIG, 0x8F), TAG, "PA config failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STANDBY), TAG, "standby failed");

    ESP_LOGI(TAG, "SX1278 ready: 433 MHz, sync=0xA3");
    return ESP_OK;
}

esp_err_t lora_sx1278_send(const void *data, size_t length)
{
    if (!data || length == 0 || length > 255) return ESP_ERR_INVALID_ARG;

    ESP_RETURN_ON_ERROR(write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STANDBY), TAG, "standby failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_IRQ_FLAGS, 0xFF), TAG, "IRQ clear failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_FIFO_ADDR_PTR, 0x00), TAG, "FIFO pointer failed");
    ESP_RETURN_ON_ERROR(write_fifo((const uint8_t *)data, length), TAG, "FIFO write failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_PAYLOAD_LENGTH, (uint8_t)length), TAG, "length failed");
    ESP_RETURN_ON_ERROR(write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_TX), TAG, "TX start failed");

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    uint8_t flags = 0;
    do {
        ESP_RETURN_ON_ERROR(read_reg(REG_IRQ_FLAGS, &flags), TAG, "IRQ read failed");
        if (flags & IRQ_TX_DONE) {
            write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE);
            write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STANDBY);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (xTaskGetTickCount() < deadline);

    write_reg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STANDBY);
    return ESP_ERR_TIMEOUT;
}

