// ESP-IDF, FreeRTOS, micro-frontend and TFLite-Micro surfaces used by the
// voice node. Same rule as the Arduino stubs: declarations only.
#pragma once

#include "Arduino.h"

// ----------------------------------------------------------------- esp ----
#define ESP_OK 0
typedef int esp_err_t;

#define MALLOC_CAP_INTERNAL 0x800
#define MALLOC_CAP_8BIT 0x004
size_t heap_caps_get_largest_free_block(uint32_t caps);

class EspClass {
 public:
  uint32_t getFreeHeap() { return 0; }
  uint32_t getPsramSize() { return 0; }
  uint32_t getFreePsram() { return 0; }
  uint32_t getFlashChipSize() { return 0; }
  const char *getChipModel() { return "ESP32-S3"; }
  uint8_t getChipRevision() { return 0; }
  uint8_t getChipCores() { return 2; }
};
extern EspClass ESP;

// ------------------------------------------------------------- freertos ----
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffUL

QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void *, unsigned long);
BaseType_t xQueueReceive(QueueHandle_t, void *, unsigned long);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned,
                                   void *, unsigned, TaskHandle_t *, int);

// ------------------------------------------------------------------ i2s ----
typedef int gpio_num_t;
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define I2S_GPIO_UNUSED ((gpio_num_t)-1)
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_MONO 1
#define I2S_STD_SLOT_LEFT 1

typedef void *i2s_chan_handle_t;

struct i2s_chan_config_t {
  int id;
  int role;
  unsigned dma_desc_num;
  unsigned dma_frame_num;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(num, role) \
  i2s_chan_config_t { (num), (role), 6, 240 }

struct i2s_std_clk_config_t { unsigned sample_rate_hz; };
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) i2s_std_clk_config_t { (rate) }

struct i2s_std_slot_config_t {
  int data_bit_width;
  int slot_mode;
  int slot_mask;
};
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
  i2s_std_slot_config_t { (bits), (mode), 0 }

struct i2s_std_gpio_invert_t { bool mclk, bclk, ws; };
struct i2s_std_gpio_config_t {
  gpio_num_t mclk, bclk, ws, dout, din;
  i2s_std_gpio_invert_t invert_flags;
};
struct i2s_std_config_t {
  i2s_std_clk_config_t clk_cfg;
  i2s_std_slot_config_t slot_cfg;
  i2s_std_gpio_config_t gpio_cfg;
};

esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *,
                          i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *,
                           uint32_t);

// ----------------------------------------------------------- frontend ------
struct FrontendWindowConfig { int size_ms, step_size_ms; };
struct FrontendFilterbankConfig {
  int num_channels;
  float lower_band_limit, upper_band_limit;
};
struct FrontendNoiseReductionConfig {
  int smoothing_bits;
  float even_smoothing, odd_smoothing, min_signal_remaining;
};
struct FrontendPcanConfig {
  int enable_pcan;
  float strength, offset;
  int gain_bits;
};
struct FrontendLogConfig { int enable_log, scale_shift; };

struct FrontendConfig {
  FrontendWindowConfig window;
  FrontendFilterbankConfig filterbank;
  FrontendNoiseReductionConfig noise_reduction;
  FrontendPcanConfig pcan_gain_control;
  FrontendLogConfig log_scale;
};
struct FrontendState { int opaque; };
struct FrontendOutput {
  const uint16_t *values;
  size_t size;
};

int FrontendPopulateState(const FrontendConfig *, FrontendState *, int);
void FrontendReset(FrontendState *);
FrontendOutput FrontendProcessSamples(FrontendState *, const int16_t *, size_t,
                                      size_t *);

// ------------------------------------------------------------ tflite -------
#define TFLITE_SCHEMA_VERSION 3
typedef enum { kTfLiteOk = 0, kTfLiteError = 1 } TfLiteStatus;
typedef enum { kTfLiteNoType = 0, kTfLiteInt8 = 9 } TfLiteType;

struct TfLiteTensorData { int8_t *int8; };
struct TfLiteTensor {
  TfLiteType type;
  size_t bytes;
  TfLiteTensorData data;
};

namespace tflite {

class Model {
 public:
  unsigned version() const { return TFLITE_SCHEMA_VERSION; }
};
const Model *GetModel(const void *);

class MicroOpResolver {};

template <unsigned N>
class MicroMutableOpResolver : public MicroOpResolver {
 public:
  TfLiteStatus AddConv2D() { return kTfLiteOk; }
  TfLiteStatus AddDepthwiseConv2D() { return kTfLiteOk; }
  TfLiteStatus AddMean() { return kTfLiteOk; }
  TfLiteStatus AddFullyConnected() { return kTfLiteOk; }
  TfLiteStatus AddReshape() { return kTfLiteOk; }
  TfLiteStatus AddSoftmax() { return kTfLiteOk; }
  TfLiteStatus AddQuantize() { return kTfLiteOk; }
};

class MicroInterpreter {
 public:
  MicroInterpreter(const Model *, const MicroOpResolver &, uint8_t *, size_t) {}
  TfLiteStatus AllocateTensors() { return kTfLiteOk; }
  TfLiteStatus Invoke() { return kTfLiteOk; }
  TfLiteTensor *input(int) { return nullptr; }
  TfLiteTensor *output(int) { return nullptr; }
  size_t arena_used_bytes() const { return 0; }
};

}  // namespace tflite
