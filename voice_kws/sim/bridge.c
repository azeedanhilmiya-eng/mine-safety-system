// Flat C entry points so the host demo can call the firmware's own code.
//
// The point of the demo is to show what the board would do, so it must not
// reimplement the board. Everything below forwards into kws_postprocess.c and
// kws_packet.h -- the same sources the ESP32 compiles -- and the only thing
// this file adds is a calling convention ctypes can reach.

#include <stdint.h>
#include <string.h>

#include "kws_config.h"
#include "kws_packet.h"
#include "kws_postprocess.h"

// ------------------------------------------------------------ decision ----

void sim_vote(const int8_t *windows, KwsResult *out) {
  int8_t buf[KWS_NUM_WINDOWS][KWS_NUM_CLASSES];
  memcpy(buf, windows, sizeof(buf));
  *out = kws_vote(buf);
}

int8_t sim_quantize_feature(uint16_t raw) { return kws_quantize_feature(raw); }

float sim_dequantize_prob(int8_t q) { return kws_dequantize_prob(q); }

int sim_is_command(int label) { return kws_is_command(label); }

// -------------------------------------------------------------- packet ----

int sim_packet_format(char *out, int out_len, const char *node, int cmd,
                      int conf, int seq) {
  return (int)kws_packet_format(out, (size_t)out_len, node, (uint8_t)cmd,
                                (uint8_t)conf, (uint8_t)seq);
}

int sim_ack_format(char *out, int out_len, const char *node, int seq) {
  return (int)kws_ack_format(out, (size_t)out_len, node, (uint8_t)seq);
}

// Returns the KwsPacketStatus; fields are only written when it is KWS_PKT_OK.
int sim_packet_parse(const char *text, char *node_out, int *cmd, int *conf,
                     int *seq) {
  KwsPacket pkt;
  KwsPacketStatus status = kws_packet_parse(text, &pkt);
  if (status == KWS_PKT_OK) {
    strcpy(node_out, pkt.node);
    *cmd = pkt.cmd;
    *conf = pkt.conf;
    *seq = pkt.seq;
  }
  return (int)status;
}

const char *sim_status_text(int status) {
  return kws_packet_status_text((KwsPacketStatus)status);
}

// ------------------------------------------------------- configuration ----
// Read from the generated header rather than re-declared here, so the demo
// cannot disagree with the firmware about the threshold or the label table.

int sim_num_windows(void) { return KWS_NUM_WINDOWS; }
int sim_num_classes(void) { return KWS_NUM_CLASSES; }
int sim_num_frames(void) { return KWS_NUM_FRAMES; }
int sim_num_channels(void) { return KWS_NUM_CHANNELS; }
int sim_clip_samples(void) { return KWS_CLIP_SAMPLES; }
int sim_capture_samples(void) { return KWS_PTT_CAPTURE_SAMPLES; }
int sim_sample_rate(void) { return KWS_SAMPLE_RATE; }
float sim_threshold(void) { return KWS_CONF_THRESHOLD; }
int sim_window_offset_ms(int i) {
  return (i >= 0 && i < KWS_NUM_WINDOWS) ? kKwsWindowOffsetMs[i] : -1;
}
const char *sim_label(int i) {
  return (i >= 0 && i < KWS_NUM_CLASSES) ? kKwsLabels[i] : "?";
}
const char *sim_label_zh(int i) {
  return (i >= 0 && i < KWS_NUM_CLASSES) ? kKwsLabelsZh[i] : "?";
}
