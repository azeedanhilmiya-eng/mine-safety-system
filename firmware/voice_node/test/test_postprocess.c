// Host test: replays vectors exported from the Python training pipeline
// through the firmware's decision logic.
//
//     cd firmware/voice_node/test && make
//
// A failure here means the board would make different decisions from the ones
// the reported accuracy and false-trigger numbers were measured with.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../kws_postprocess.h"
#include "kws_test_vectors.h"

static int failures = 0;

static void fail(const char *what, int index) {
  printf("  FAIL %s [case %d]\n", what, index);
  failures++;
}

static void test_config_matches_vectors(void) {
  printf("config threshold\n");
  if (fabsf(KWS_CONF_THRESHOLD - KWS_TEST_THRESHOLD) > 1e-6f) {
    printf("  FAIL kws_config.h threshold %g != vector threshold %g\n"
           "       regenerate both: tools/export_c_array.py and "
           "tools/export_test_vectors.py\n",
           (double)KWS_CONF_THRESHOLD, (double)KWS_TEST_THRESHOLD);
    failures++;
  }
}

static void test_feature_quantisation(void) {
  printf("feature quantisation (%d cases)\n", KWS_NUM_FEATURE_VECTORS);
  for (int i = 0; i < KWS_NUM_FEATURE_VECTORS; i++) {
    const KwsFeatureVector *v = &kKwsFeatureVectors[i];
    int8_t got = kws_quantize_feature(v->raw);
    if (got != v->expected) {
      printf("  raw=%u expected=%d got=%d\n", v->raw, v->expected, got);
      fail("quantize_feature", i);
    }
  }
}

static void test_vote(void) {
  printf("window voting (%d cases)\n", KWS_NUM_VOTE_VECTORS);
  for (int i = 0; i < KWS_NUM_VOTE_VECTORS; i++) {
    const KwsVoteVector *v = &kKwsVoteVectors[i];
    KwsResult r = kws_vote(v->window_out);

    if (r.label != v->expected_label) {
      printf("  label expected=%d got=%d\n", v->expected_label, r.label);
      fail("vote label", i);
      continue;
    }
    if (r.votes != v->expected_votes) {
      printf("  votes expected=%d got=%d\n", v->expected_votes, r.votes);
      fail("vote count", i);
    }
    if (fabsf(r.confidence - v->expected_confidence) > 1e-6f) {
      printf("  confidence expected=%.9g got=%.9g\n",
             (double)v->expected_confidence, (double)r.confidence);
      fail("vote confidence", i);
    }
    if (r.accepted != v->expected_accepted) {
      printf("  accepted expected=%d got=%d (label=%d conf=%.4f thr=%.4f)\n",
             v->expected_accepted, r.accepted, r.label, (double)r.confidence,
             (double)KWS_CONF_THRESHOLD);
      fail("vote accepted", i);
    }
  }
}

static void test_non_command_never_fires(void) {
  printf("silence and unknown never raise an event\n");
  for (int c = 0; c < KWS_NUM_CLASSES; c++) {
    if (kws_is_command(c)) continue;
    int8_t windows[KWS_NUM_WINDOWS][KWS_NUM_CLASSES];
    memset(windows, -128, sizeof(windows));
    for (int w = 0; w < KWS_NUM_WINDOWS; w++) windows[w][c] = 127;

    KwsResult r = kws_vote(windows);
    if (r.accepted) {
      printf("  class %d (%s) fired at full confidence\n", c, kKwsLabels[c]);
      failures++;
    }
  }
}

static void test_batch_quantisation(void) {
  printf("batch quantisation matches the scalar path\n");
  uint16_t raw[KWS_FEATURE_ELEMENTS];
  int8_t out[KWS_FEATURE_ELEMENTS];
  for (int i = 0; i < KWS_FEATURE_ELEMENTS; i++) {
    raw[i] = (uint16_t)((i * 37u) % 4096u);
  }
  kws_quantize_features(raw, KWS_FEATURE_ELEMENTS, out);
  for (int i = 0; i < KWS_FEATURE_ELEMENTS; i++) {
    if (out[i] != kws_quantize_feature(raw[i])) {
      fail("quantize_features", i);
      break;
    }
  }
}

int main(void) {
  printf("kws_postprocess host tests\n\n");
  test_config_matches_vectors();
  test_feature_quantisation();
  test_vote();
  test_non_command_never_fires();
  test_batch_quantisation();

  printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
  return failures == 0 ? 0 : 1;
}
