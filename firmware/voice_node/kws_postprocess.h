// Decision logic for the keyword spotter: feature quantisation, window voting
// and the accept/reject rule.
//
// Deliberately free of ESP-IDF, Arduino and TFLite dependencies so it can be
// compiled and tested on a PC.  `test/test_postprocess.c` checks it against
// vectors exported from the Python training pipeline, which is the only way to
// be sure the board and the training set agree on what a feature is.
#pragma once

#include <stdint.h>

#include "kws_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int label;          // winning class index, or -1 when nothing won
  float confidence;   // probability of the winning class
  int accepted;       // 1 = raise a LoRa event, 0 = stay quiet
  int votes;          // how many windows agreed on `label`
} KwsResult;

// uint16 micro-frontend output -> int8 model input.
// Integer maths only, byte-identical to kws/frontend.py:quantize().
int8_t kws_quantize_feature(uint16_t raw);

void kws_quantize_features(const uint16_t *raw, int count, int8_t *out);

// int8 model output -> probability.
float kws_dequantize_prob(int8_t q);

// Majority vote over the overlapping windows.
//
// `window_out[w][c]` is the raw int8 output tensor of window w.  A class wins
// when at least two windows pick it; a three-way disagreement is a reject,
// because a keyword said clearly lands in at least two of three windows that
// are only 200 ms apart.  The reported confidence is the highest probability
// among the windows that voted for the winner.
//
// The result is accepted only when the winner is a command class (silence and
// unknown never fire) and its confidence reaches KWS_CONF_THRESHOLD.
KwsResult kws_vote(const int8_t window_out[KWS_NUM_WINDOWS][KWS_NUM_CLASSES]);

#ifdef __cplusplus
}
#endif
