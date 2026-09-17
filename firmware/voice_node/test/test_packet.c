// Host test for the voice event packet format shared by the underground node
// and the surface gateway.
//
//     cd firmware/voice_node/test && make
//
// The gateway acts on whatever survives this parser: it lights the OLED,
// sounds the buzzer and sends an SMS. Every test below is a way a radio link
// could hand it something it should refuse.

#include <stdio.h>
#include <string.h>

#include "../kws_packet.h"

static int failures = 0;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("  FAIL %s\n", what);
    failures++;
  }
}

static void expect_reject(const char *text, KwsPacketStatus want,
                          const char *what) {
  KwsPacket pkt;
  KwsPacketStatus got = kws_packet_parse(text, &pkt);
  if (got != want) {
    printf("  FAIL %s: %s -> %s (wanted %s)\n", what, text,
           kws_packet_status_text(got), kws_packet_status_text(want));
    failures++;
  }
}

static void test_round_trip(void) {
  printf("format then parse returns what went in\n");
  const struct { const char *node; uint8_t cmd, conf, seq; } cases[] = {
      {"M1", 2, 93, 17}, {"M2", 3, 100, 0},   {"M1", 0, 0, 255},
      {"M1", 255, 50, 1}, {"NODE12", 3, 80, 200},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char buf[KWS_PACKET_MAX];
    size_t n = kws_packet_format(buf, sizeof(buf), cases[i].node, cases[i].cmd,
                                 cases[i].conf, cases[i].seq);
    check(n > 0, "format produced output");

    KwsPacket pkt;
    KwsPacketStatus s = kws_packet_parse(buf, &pkt);
    if (s != KWS_PKT_OK) {
      printf("  FAIL %s rejected as %s\n", buf, kws_packet_status_text(s));
      failures++;
      continue;
    }
    check(strcmp(pkt.node, cases[i].node) == 0, "node id survived");
    check(pkt.cmd == cases[i].cmd, "cmd survived");
    check(pkt.conf == cases[i].conf, "conf survived");
    check(pkt.seq == cases[i].seq, "seq survived");
  }
}

static void test_known_vector(void) {
  printf("checksum is stable across builds\n");
  // Pinned so a change to the CRC on one side cannot pass unnoticed on the
  // other: both sketches compile this same header, but the value also ends up
  // in documentation and in the node's serial log.
  char buf[KWS_PACKET_MAX];
  kws_packet_format(buf, sizeof(buf), "M1", 2, 93, 17);
  const uint8_t crc = kws_crc8("VE,M1,2,93,17,", 14);
  char expected[KWS_PACKET_MAX];
  snprintf(expected, sizeof(expected), "VE,M1,2,93,17,%02X", crc);
  if (strcmp(buf, expected) != 0) {
    printf("  FAIL built %s, expected %s\n", buf, expected);
    failures++;
  }
  printf("  reference packet: %s\n", buf);
}

static void test_corruption_is_rejected(void) {
  printf("corrupted packets are refused, not guessed at\n");

  char good[KWS_PACKET_MAX];
  kws_packet_format(good, sizeof(good), "M1", 2, 93, 17);

  // Every single-character mutation of a valid packet must be rejected.
  // This is the case that matters: a radio flips one bit, and the gateway
  // must not announce a rescue call that nobody made.
  int accepted_corrupt = 0;
  for (size_t i = 0; i < strlen(good); i++) {
    for (char c = 32; c < 127; c++) {
      if (good[i] == c) continue;
      char mutated[KWS_PACKET_MAX];
      strcpy(mutated, good);
      mutated[i] = c;

      KwsPacket pkt;
      if (kws_packet_parse(mutated, &pkt) == KWS_PKT_OK) {
        printf("  FAIL accepted corrupted packet: %s (from %s)\n", mutated,
               good);
        accepted_corrupt++;
        failures++;
        if (accepted_corrupt > 5) return;
      }
    }
  }
  printf("  all single-character mutations rejected\n");
}

static void test_malformed(void) {
  printf("malformed input is classified, not crashed on\n");
  expect_reject("M1,1500,1500,1800,0", KWS_PKT_ERR_PREFIX, "sensor telemetry");
  expect_reject("VA,M1,17", KWS_PKT_ERR_PREFIX, "an ack");
  expect_reject("", KWS_PKT_ERR_PREFIX, "empty string");
  expect_reject("VE,", KWS_PKT_ERR_FIELDS, "prefix only");
  expect_reject("VE,M1,2,93,17", KWS_PKT_ERR_FIELDS, "missing checksum");
  expect_reject("VE,M1,2,93,17,3F,9", KWS_PKT_ERR_FIELDS, "extra field");
  expect_reject("VE,,2,93,17,3F", KWS_PKT_ERR_NODE, "empty node id");
  expect_reject("VE,VERYLONGNODE,2,93,17,3F", KWS_PKT_ERR_NODE, "node too long");
  expect_reject("VE,M1,x,93,17,3F", KWS_PKT_ERR_NUMBER, "non-numeric cmd");
  expect_reject("VE,M1,2,,17,3F", KWS_PKT_ERR_NUMBER, "empty conf");
  expect_reject("VE,M1,2,-5,17,3F", KWS_PKT_ERR_NUMBER, "negative conf");
  expect_reject("VE,M1,999,93,17,3F", KWS_PKT_ERR_RANGE, "cmd over 255");
  expect_reject("VE,M1,2,101,17,3F", KWS_PKT_ERR_RANGE, "confidence over 100");
  expect_reject("VE,M1,2,93,17,3", KWS_PKT_ERR_CRC, "one-digit checksum");
  expect_reject("VE,M1,2,93,17,3FA", KWS_PKT_ERR_CRC, "three-digit checksum");
  expect_reject("VE,M1,2,93,17,3f", KWS_PKT_ERR_CRC, "lowercase checksum");
  expect_reject("VE,M1,2,93,17,ZZ", KWS_PKT_ERR_CRC, "non-hex checksum");
  expect_reject("VE,M1,2,93,17,00", KWS_PKT_ERR_CRC, "wrong checksum");
}

static void test_ack(void) {
  printf("ack format\n");
  char ack[24];
  size_t n = kws_ack_format(ack, sizeof(ack), "M1", 17);
  check(n > 0 && strcmp(ack, "VA,M1,17") == 0, "ack text");

  // The node compares the ack as a whole string, so a truncating buffer must
  // report failure rather than hand back a prefix that would never match.
  char tiny[4];
  check(kws_ack_format(tiny, sizeof(tiny), "M1", 17) == 0,
        "ack refuses to truncate");
  char small[8];
  check(kws_packet_format(small, sizeof(small), "M1", 2, 93, 17) == 0,
        "packet refuses to truncate");
}

int main(void) {
  printf("kws_packet host tests\n\n");
  test_round_trip();
  test_known_vector();
  test_corruption_is_rejected();
  test_malformed();
  test_ack();

  printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
  return failures == 0 ? 0 : 1;
}
