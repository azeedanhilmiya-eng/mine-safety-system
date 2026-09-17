// Voice event packet, shared verbatim by the underground node and the surface
// gateway.
//
// Header-only and free of Arduino types on purpose: the two sketches live in
// different folders, and a format implemented twice is a format that drifts.
// test/test_packet.c exercises it on the host.
//
//   event  node -> gateway   VE,<node>,<cmd>,<conf>,<seq>,<CRC>
//                            e.g. VE,M1,2,93,17,C0
//   ack    gateway -> node   VA,<node>,<seq>
//                            e.g. VA,M1,17
//
// <cmd>  index into kKwsLabels, which is the index in kws/config.py LABELS
// <conf> confidence in percent, 0..100
// <seq>  wraps at 255; the gateway de-duplicates retransmissions on it
// <CRC>  CRC-8 (poly 0x07, init 0x00) over every character up to and
//        including the comma before the checksum, as two uppercase hex digits
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_PACKET_PREFIX "VE,"
#define KWS_ACK_PREFIX "VA,"
#define KWS_PACKET_MAX 48
#define KWS_NODE_ID_MAX 8

typedef enum {
  KWS_PKT_OK = 0,
  KWS_PKT_ERR_PREFIX,     // not a voice event at all
  KWS_PKT_ERR_FIELDS,     // wrong number of comma-separated fields
  KWS_PKT_ERR_NODE,       // node id empty or too long
  KWS_PKT_ERR_NUMBER,     // a numeric field was empty or not a number
  KWS_PKT_ERR_RANGE,      // a numeric field was out of range
  KWS_PKT_ERR_CRC,        // checksum malformed or mismatched
} KwsPacketStatus;

typedef struct {
  char node[KWS_NODE_ID_MAX];
  uint8_t cmd;
  uint8_t conf;   // percent
  uint8_t seq;
} KwsPacket;

static inline uint8_t kws_crc8(const char *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint8_t)data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Returns the number of characters written, or 0 if `out` is too small.
static inline size_t kws_packet_format(char *out, size_t out_len,
                                       const char *node, uint8_t cmd,
                                       uint8_t conf, uint8_t seq) {
  char body[KWS_PACKET_MAX];
  int n = snprintf(body, sizeof(body), KWS_PACKET_PREFIX "%s,%u,%u,%u,",
                   node, (unsigned)cmd, (unsigned)conf, (unsigned)seq);
  if (n < 0 || (size_t)n >= sizeof(body)) return 0;

  int total = snprintf(out, out_len, "%s%02X", body,
                       kws_crc8(body, (size_t)n));
  if (total < 0 || (size_t)total >= out_len) return 0;
  return (size_t)total;
}

static inline size_t kws_ack_format(char *out, size_t out_len,
                                    const char *node, uint8_t seq) {
  int n = snprintf(out, out_len, KWS_ACK_PREFIX "%s,%u", node, (unsigned)seq);
  if (n < 0 || (size_t)n >= out_len) return 0;
  return (size_t)n;
}

// Parses one unsigned decimal field spanning [begin, end).
static inline int kws__parse_u8(const char *begin, const char *end,
                                unsigned long limit, uint8_t *out,
                                KwsPacketStatus *status) {
  if (begin >= end) {
    *status = KWS_PKT_ERR_NUMBER;
    return 0;
  }
  unsigned long value = 0;
  for (const char *p = begin; p < end; p++) {
    if (*p < '0' || *p > '9') {
      *status = KWS_PKT_ERR_NUMBER;
      return 0;
    }
    value = value * 10 + (unsigned long)(*p - '0');
    if (value > limit) {
      *status = KWS_PKT_ERR_RANGE;
      return 0;
    }
  }
  *out = (uint8_t)value;
  return 1;
}

static inline int kws__hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;  // lowercase is rejected: the format says uppercase
}

// Strict parse: anything not exactly on format is rejected rather than
// guessed at. A radio link hands you corrupted bytes eventually, and a
// half-understood packet is how a gateway announces a rescue that nobody
// called for.
static inline KwsPacketStatus kws_packet_parse(const char *text,
                                               KwsPacket *out) {
  const size_t prefix_len = strlen(KWS_PACKET_PREFIX);
  if (strncmp(text, KWS_PACKET_PREFIX, prefix_len) != 0) {
    return KWS_PKT_ERR_PREFIX;
  }
  const size_t len = strlen(text);
  if (len >= KWS_PACKET_MAX) return KWS_PKT_ERR_FIELDS;

  // Field boundaries: VE,<node>,<cmd>,<conf>,<seq>,<crc>
  const char *field[5];  // starts of node, cmd, conf, seq, crc
  field[0] = text + prefix_len;
  int found = 1;
  for (const char *p = field[0]; *p && found < 5; p++) {
    if (*p == ',') field[found++] = p + 1;
  }
  if (found != 5) return KWS_PKT_ERR_FIELDS;

  const char *end = text + len;
  const char *node_end = field[1] - 1;
  const char *cmd_end = field[2] - 1;
  const char *conf_end = field[3] - 1;
  const char *seq_end = field[4] - 1;

  // A sixth comma means extra fields; the checksum is the last one.
  if (memchr(field[4], ',', (size_t)(end - field[4])) != NULL) {
    return KWS_PKT_ERR_FIELDS;
  }

  const size_t node_len = (size_t)(node_end - field[0]);
  if (node_len == 0 || node_len >= KWS_NODE_ID_MAX) return KWS_PKT_ERR_NODE;

  KwsPacketStatus status = KWS_PKT_OK;
  uint8_t cmd = 0, conf = 0, seq = 0;
  if (!kws__parse_u8(field[1], cmd_end, 255, &cmd, &status)) return status;
  if (!kws__parse_u8(field[2], conf_end, 100, &conf, &status)) return status;
  if (!kws__parse_u8(field[3], seq_end, 255, &seq, &status)) return status;

  if ((size_t)(end - field[4]) != 2) return KWS_PKT_ERR_CRC;
  const int hi = kws__hex_digit(field[4][0]);
  const int lo = kws__hex_digit(field[4][1]);
  if (hi < 0 || lo < 0) return KWS_PKT_ERR_CRC;

  const uint8_t want = (uint8_t)((hi << 4) | lo);
  const uint8_t got = kws_crc8(text, (size_t)(field[4] - text));
  if (want != got) return KWS_PKT_ERR_CRC;

  memcpy(out->node, field[0], node_len);
  out->node[node_len] = '\0';
  out->cmd = cmd;
  out->conf = conf;
  out->seq = seq;
  return KWS_PKT_OK;
}

static inline const char *kws_packet_status_text(KwsPacketStatus s) {
  switch (s) {
    case KWS_PKT_OK: return "ok";
    case KWS_PKT_ERR_PREFIX: return "not a voice packet";
    case KWS_PKT_ERR_FIELDS: return "bad field count";
    case KWS_PKT_ERR_NODE: return "bad node id";
    case KWS_PKT_ERR_NUMBER: return "non-numeric field";
    case KWS_PKT_ERR_RANGE: return "field out of range";
    case KWS_PKT_ERR_CRC: return "crc mismatch";
    default: return "unknown";
  }
}

#ifdef __cplusplus
}
#endif
