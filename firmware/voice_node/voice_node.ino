// Voice node: push-to-talk keyword spotting on an ESP32-S3, feeding the
// existing LoRa link.
//
// NOT COMPILED IN THE ENVIRONMENT THIS WAS WRITTEN IN. The decision logic in
// kws_postprocess.c is covered by host tests (test/), and the model and
// kws_config.h are generated from the trained pipeline, but the I2S, frontend
// and TFLite-Micro glue below has only been written, not built. Expect to fix
// include paths for your component layout on the first build.
//
// Dependencies:
//   esp-tflite-micro   (Espressif's fork; brings ESP-NN int8 kernels and the
//                       micro_frontend the training features were computed with)
//   LoRa               (sandeepmistry/arduino-LoRa, already used by this repo)
//
// Serial protocol shared with voice_kws/tools/:
//   'r' -> capture, emit #WAV frame                (serial_record.py)
//   'p' -> capture, emit #WAV and #FEAT frames     (device_parity.py)
//   'i' -> print memory and model info
// Anything else is ignored; ordinary logs pass through between frames.

#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include <driver/i2s_std.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "microfrontend/lib/frontend.h"
#include "microfrontend/lib/frontend_util.h"
}

#include "kws_config.h"
#include "kws_model_data.h"
#include "kws_packet.h"
#include "kws_postprocess.h"

// ------------------------------------------------------------------ pins ---
// Chosen to avoid this node's existing wiring (LoRa 9-14, MQ4/MQ7/water 4-6,
// siren 18), the SPI flash pins (26-32), the octal-PSRAM pins (33-37), USB
// (19/20), UART0 (43/44) and the strapping pins (0/45/46).
static const gpio_num_t kI2sBclk = GPIO_NUM_16;
static const gpio_num_t kI2sWs = GPIO_NUM_15;
static const gpio_num_t kI2sData = GPIO_NUM_17;
static const int kButtonPin = 21;  // to GND, INPUT_PULLUP

// INMP441 delivers 24-bit samples left-aligned in a 32-bit slot. Shifting by
// 14 rather than 16 adds about 12 dB, which suits speech at arm's length.
// Check it with serial_record.py: peaks should stay under 30000 when someone
// shouts. If they clip, raise this; if everything is quiet, lower it.
static const int kSampleShift = 14;

// ------------------------------------------------------------- LoRa pins ---
static const char NODE_ID[] = "M1";
static const int LORA_SS = 10, LORA_MOSI = 11, LORA_SCK = 12;
static const int LORA_MISO = 13, LORA_RST = 14, LORA_DIO0 = 9;
static const uint8_t LORA_SYNC_WORD = 0xA3;

// ----------------------------------------------------------------- state ---
static i2s_chan_handle_t g_rx_chan = nullptr;
static FrontendState g_frontend;

static int16_t g_capture[KWS_PTT_CAPTURE_SAMPLES];
static int8_t g_features[KWS_NUM_WINDOWS][KWS_FEATURE_ELEMENTS];

// Sized from interpreter.arena_used_bytes() plus headroom; 'i' prints the
// real figure, which also goes in the memory report.
static const int kArenaSize = 48 * 1024;
alignas(16) static uint8_t g_arena[kArenaSize];

static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;

struct VoiceEvent {
  uint8_t cmd;
  uint8_t conf;  // percent
};
static QueueHandle_t g_event_queue = nullptr;

// The Arduino IDE injects prototypes for .ino files; a PlatformIO or ESP-IDF
// build compiles this as ordinary C++ and does not, so declare them here.
static void printInfo();
static KwsResult captureAndClassify(bool dump_wav, bool dump_features);

// -------------------------------------------------------------------- I2S ---
static bool setupI2s() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;  // keeps capturing while inference runs
  if (i2s_new_channel(&chan_cfg, nullptr, &g_rx_chan) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KWS_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = kI2sBclk,
          .ws = kI2sWs,
          .dout = I2S_GPIO_UNUSED,
          .din = kI2sData,
          .invert_flags = {false, false, false},
      },
  };
  // INMP441 with L/R tied to GND drives the left slot.
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  if (i2s_channel_init_std_mode(g_rx_chan, &std_cfg) != ESP_OK) return false;
  return i2s_channel_enable(g_rx_chan) == ESP_OK;
}

// Records exactly `count` samples, converting to int16 and removing DC.
// The INMP441 has a substantial DC offset; left in, it lands in the lowest mel
// channels and the model sees a bias that was never in the training data.
static void record(int16_t *dst, size_t count) {
  static int32_t dc = 0;
  int32_t block[256];
  size_t written = 0;

  while (written < count) {
    size_t bytes = 0;
    size_t want = (count - written) * sizeof(int32_t);
    if (want > sizeof(block)) want = sizeof(block);
    if (i2s_channel_read(g_rx_chan, block, want, &bytes, portMAX_DELAY) != ESP_OK) {
      break;
    }
    const size_t n = bytes / sizeof(int32_t);
    for (size_t i = 0; i < n && written < count; i++, written++) {
      int32_t s = block[i] >> kSampleShift;
      dc += (s - dc) >> 8;
      int32_t hp = s - dc;
      dst[written] = (int16_t)constrain(hp, -32768, 32767);
    }
  }
}

// --------------------------------------------------------------- frontend ---
static bool setupFrontend() {
  FrontendConfig config;
  config.window.size_ms = KWS_WINDOW_SIZE_MS;
  config.window.step_size_ms = KWS_WINDOW_STEP_MS;
  config.filterbank.num_channels = KWS_NUM_CHANNELS;
  config.filterbank.lower_band_limit = KWS_LOWER_BAND_LIMIT;
  config.filterbank.upper_band_limit = KWS_UPPER_BAND_LIMIT;
  config.noise_reduction.smoothing_bits = KWS_SMOOTHING_BITS;
  config.noise_reduction.even_smoothing = KWS_EVEN_SMOOTHING;
  config.noise_reduction.odd_smoothing = KWS_ODD_SMOOTHING;
  config.noise_reduction.min_signal_remaining = KWS_MIN_SIGNAL_REMAIN;
  config.pcan_gain_control.enable_pcan = KWS_ENABLE_PCAN;
  config.pcan_gain_control.strength = KWS_PCAN_STRENGTH;
  config.pcan_gain_control.offset = KWS_PCAN_OFFSET;
  config.pcan_gain_control.gain_bits = KWS_GAIN_BITS;
  config.log_scale.enable_log = KWS_ENABLE_LOG;
  config.log_scale.scale_shift = KWS_SCALE_SHIFT;
  return FrontendPopulateState(&config, &g_frontend, KWS_SAMPLE_RATE);
}

// One 1 s window -> KWS_FEATURE_ELEMENTS int8 features.
//
// FrontendReset() is not optional. The frontend carries noise-estimation and
// PCAN gain state across calls; the TensorFlow op that produced the training
// features always starts from a clean state, so every window here must too.
// tools/device_parity.py exists to catch it if this is ever removed.
static bool computeFeatures(const int16_t *pcm, int8_t *out) {
  FrontendReset(&g_frontend);

  size_t consumed = 0;
  int frame = 0;
  while (frame < KWS_NUM_FRAMES && consumed < KWS_CLIP_SAMPLES) {
    size_t used = 0;
    FrontendOutput slice = FrontendProcessSamples(
        &g_frontend, pcm + consumed, KWS_CLIP_SAMPLES - consumed, &used);
    consumed += used;
    if (slice.size == 0) continue;  // not a whole window yet

    if ((int)slice.size != KWS_NUM_CHANNELS) return false;
    kws_quantize_features(slice.values, KWS_NUM_CHANNELS,
                          out + frame * KWS_NUM_CHANNELS);
    frame++;
  }

  // A short capture is padded with the frontend's "no energy" value rather
  // than with zeros, which in int8 feature space means moderate energy.
  for (; frame < KWS_NUM_FRAMES; frame++) {
    memset(out + frame * KWS_NUM_CHANNELS, -128, KWS_NUM_CHANNELS);
  }
  return true;
}

// ---------------------------------------------------------------- model ----
static bool setupModel() {
  const tflite::Model *model = tflite::GetModel(g_kws_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) return false;

  // Only the operators this model actually uses, so the unused kernels stay
  // out of flash.
  static tflite::MicroMutableOpResolver<7> resolver;
  if (resolver.AddConv2D() != kTfLiteOk) return false;
  if (resolver.AddDepthwiseConv2D() != kTfLiteOk) return false;
  if (resolver.AddMean() != kTfLiteOk) return false;  // GlobalAveragePooling2D
  if (resolver.AddFullyConnected() != kTfLiteOk) return false;
  if (resolver.AddReshape() != kTfLiteOk) return false;
  if (resolver.AddSoftmax() != kTfLiteOk) return false;
  if (resolver.AddQuantize() != kTfLiteOk) return false;

  static tflite::MicroInterpreter interpreter(model, resolver, g_arena,
                                              kArenaSize);
  if (interpreter.AllocateTensors() != kTfLiteOk) return false;

  g_interpreter = &interpreter;
  g_input = interpreter.input(0);
  g_output = interpreter.output(0);

  // The exporter reports input scale 1.0 / zero point 0, which is what lets
  // the feature bytes be copied in unchanged. If that ever stops holding, the
  // copy below would silently feed the model rescaled values.
  if (g_input->type != kTfLiteInt8 || g_output->type != kTfLiteInt8) return false;
  if (g_input->bytes != KWS_FEATURE_ELEMENTS) return false;
  return true;
}

static void runWindow(const int8_t *features, int8_t *out) {
  memcpy(g_input->data.int8, features, KWS_FEATURE_ELEMENTS);
  if (g_interpreter->Invoke() != kTfLiteOk) {
    memset(out, -128, KWS_NUM_CLASSES);
    return;
  }
  memcpy(out, g_output->data.int8, KWS_NUM_CLASSES);
}

// ----------------------------------------------------------- serial dumps ---
static void emitWav(const int16_t *pcm, size_t count) {
  Serial.printf("#WAV %d %u\n", KWS_SAMPLE_RATE, (unsigned)count);
  Serial.write((const uint8_t *)pcm, count * sizeof(int16_t));
  Serial.println("#END");
}

static void emitFeatures(const int8_t *features) {
  Serial.printf("#FEAT %d\n", KWS_FEATURE_ELEMENTS);
  Serial.write((const uint8_t *)features, KWS_FEATURE_ELEMENTS);
  Serial.println("#END");
}

// ------------------------------------------------------------- inference ---
// Captures once and votes across the overlapping windows. Three windows 200 ms
// apart make the result far less sensitive to when the button was pressed
// relative to when the word was spoken.
static KwsResult captureAndClassify(bool dump_wav, bool dump_features) {
  const uint32_t t_start = micros();
  record(g_capture, KWS_PTT_CAPTURE_SAMPLES);
  const uint32_t t_recorded = micros();

  if (dump_wav) emitWav(g_capture, KWS_PTT_CAPTURE_SAMPLES);

  int8_t window_out[KWS_NUM_WINDOWS][KWS_NUM_CLASSES];
  uint32_t t_feat = 0, t_inf = 0;

  for (int w = 0; w < KWS_NUM_WINDOWS; w++) {
    const size_t offset =
        (size_t)KWS_SAMPLE_RATE * kKwsWindowOffsetMs[w] / 1000;

    uint32_t a = micros();
    if (!computeFeatures(g_capture + offset, g_features[w])) {
      Serial.println("[VOICE] frontend failed");
      memset(window_out[w], -128, KWS_NUM_CLASSES);
      continue;
    }
    uint32_t b = micros();
    runWindow(g_features[w], window_out[w]);
    uint32_t c = micros();

    t_feat += b - a;
    t_inf += c - b;
  }

  // Parity checks compare the first window, the one aligned to the start of
  // the capture, because that is the slice device_parity.py recomputes.
  if (dump_features) emitFeatures(g_features[0]);

  KwsResult r = kws_vote(window_out);

  const char *name = (r.label >= 0) ? kKwsLabels[r.label] : "disagree";
  Serial.printf("[VOICE] label=%s conf=%.3f votes=%d accepted=%d "
                "t_rec=%lums t_feat=%luus t_inf=%luus arena=%u\n",
                name, r.confidence, r.votes, r.accepted,
                (unsigned long)((t_recorded - t_start) / 1000),
                (unsigned long)t_feat, (unsigned long)t_inf,
                (unsigned)g_interpreter->arena_used_bytes());
  return r;
}

// ------------------------------------------------------------- LoRa event ---
// Format and checksum live in kws_packet.h, which the surface gateway compiles
// too, so the two ends cannot drift apart. test/test_packet.c covers it.
static void sendVoiceEvent(uint8_t cmd, uint8_t conf) {
  static uint8_t seq = 0;
  seq++;

  char packet[KWS_PACKET_MAX];
  if (kws_packet_format(packet, sizeof(packet), NODE_ID, cmd, conf, seq) == 0) {
    Serial.println("[VOICE-TX] packet too long, dropped");
    return;
  }

  char expected_ack[24];
  if (kws_ack_format(expected_ack, sizeof(expected_ack), NODE_ID, seq) == 0) {
    Serial.println("[VOICE-TX] ack buffer too small, dropped");
    return;
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    LoRa.beginPacket();
    LoRa.print(packet);
    LoRa.endPacket();
    Serial.printf("[VOICE-TX] %s (try %d)\n", packet, attempt + 1);

    LoRa.receive();
    const unsigned long deadline = millis() + 900;
    while (millis() < deadline) {
      if (LoRa.parsePacket()) {
        String reply;
        while (LoRa.available()) reply += (char)LoRa.read();
        if (reply == expected_ack) {
          Serial.println("[VOICE-TX] ACK");
          return;
        }
      }
      delay(5);
    }
    // Back off by a random amount so a retry does not keep colliding with the
    // node's own 5 s telemetry slot.
    delay(random(150, 400));
  }
  Serial.println("[VOICE-TX] no ACK after retry");
}

// ------------------------------------------------------------------ tasks ---
// Voice runs on core 0, which is idle on this node (the underground firmware
// never brings up WiFi). LoRa stays on core 1 inside loop(), and events cross
// over by queue, so the SPI bus only ever has one user.
static void voiceTask(void *) {
  pinMode(kButtonPin, INPUT_PULLUP);
  bool was_pressed = false;

  for (;;) {
    const bool pressed = digitalRead(kButtonPin) == LOW;
    if (pressed && !was_pressed) {
      delay(30);  // debounce
      if (digitalRead(kButtonPin) == LOW) {
        Serial.println("[VOICE] listening...");
        KwsResult r = captureAndClassify(false, false);
        if (r.accepted) {
          VoiceEvent ev{(uint8_t)r.label, (uint8_t)(r.confidence * 100.0f)};
          xQueueSend(g_event_queue, &ev, 0);
        }
      }
    }
    was_pressed = pressed;

    if (Serial.available()) {
      const int c = Serial.read();
      if (c == 'r') captureAndClassify(true, false);
      else if (c == 'p') captureAndClassify(true, true);
      else if (c == 'i') printInfo();
    }
    delay(10);
  }
}

static void printInfo() {
  Serial.printf("[INFO] model %u bytes, arena %u/%u used, classes %d\n",
                g_kws_model_data_len,
                (unsigned)g_interpreter->arena_used_bytes(), kArenaSize,
                KWS_NUM_CLASSES);
  Serial.printf("[INFO] threshold %.2f, windows %d, capture %d ms\n",
                KWS_CONF_THRESHOLD, KWS_NUM_WINDOWS, KWS_PTT_CAPTURE_MS);
  Serial.printf("[INFO] heap %u, largest block %u, psram %u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT),
                (unsigned)ESP.getPsramSize());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[INIT] voice node");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("[ERROR] LoRa init failed");
  } else {
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();
    Serial.println("[OK] LoRa ready");
  }

  if (!setupI2s()) Serial.println("[ERROR] I2S init failed");
  if (!setupFrontend()) Serial.println("[ERROR] frontend init failed");
  if (!setupModel()) Serial.println("[ERROR] model init failed");
  printInfo();

  g_event_queue = xQueueCreate(4, sizeof(VoiceEvent));
  xTaskCreatePinnedToCore(voiceTask, "voice", 8192, nullptr, 3, nullptr, 0);
  Serial.println("[OK] hold the button and speak");
}

void loop() {
  // Existing telemetry (MQ-4, MQ-7, water level) belongs here, unchanged.

  VoiceEvent ev;
  if (xQueueReceive(g_event_queue, &ev, 0) == pdTRUE) {
    sendVoiceEvent(ev.cmd, ev.conf);
  }
  delay(10);
}
