#include "kws_postprocess.h"

int8_t kws_quantize_feature(uint16_t raw) {
  int32_t v = ((int32_t)raw * KWS_VALUE_SCALE + KWS_VALUE_DIV / 2) / KWS_VALUE_DIV;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (int8_t)(v - 128);
}

void kws_quantize_features(const uint16_t *raw, int count, int8_t *out) {
  for (int i = 0; i < count; i++) {
    out[i] = kws_quantize_feature(raw[i]);
  }
}

float kws_dequantize_prob(int8_t q) {
  return ((float)q - (float)KWS_OUTPUT_ZERO_POINT) * KWS_OUTPUT_SCALE;
}

static int argmax_i8(const int8_t *v, int n) {
  int best = 0;
  for (int i = 1; i < n; i++) {
    if (v[i] > v[best]) best = i;
  }
  return best;
}

KwsResult kws_vote(const int8_t window_out[KWS_NUM_WINDOWS][KWS_NUM_CLASSES]) {
  KwsResult r = {-1, 0.0f, 0, 0};

  int picks[KWS_NUM_WINDOWS];
  for (int w = 0; w < KWS_NUM_WINDOWS; w++) {
    picks[w] = argmax_i8(window_out[w], KWS_NUM_CLASSES);
  }

  // Most-voted class; ties break towards the lower index, which is the
  // conservative direction because silence and unknown sort first.
  int best_label = -1, best_votes = 0;
  for (int c = 0; c < KWS_NUM_CLASSES; c++) {
    int votes = 0;
    for (int w = 0; w < KWS_NUM_WINDOWS; w++) {
      if (picks[w] == c) votes++;
    }
    if (votes > best_votes) {
      best_votes = votes;
      best_label = c;
    }
  }

  if (best_votes < 2) {
    // Every window disagreed: whatever was said, it was not said clearly.
    r.label = -1;
    r.votes = best_votes;
    return r;
  }

  float conf = 0.0f;
  for (int w = 0; w < KWS_NUM_WINDOWS; w++) {
    if (picks[w] != best_label) continue;
    float p = kws_dequantize_prob(window_out[w][best_label]);
    if (p > conf) conf = p;
  }

  r.label = best_label;
  r.votes = best_votes;
  r.confidence = conf;
  r.accepted = kws_is_command(best_label) && conf >= KWS_CONF_THRESHOLD;
  return r;
}
