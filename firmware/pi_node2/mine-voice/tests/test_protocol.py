import unittest

import numpy as np

from mine_voice.audio import _resample_linear
from mine_voice.protocol import build_text_packets, parse_ack, split_utf8


class ProtocolTests(unittest.TestCase):
    def test_utf8_is_not_cut(self):
        parts = split_utf8("一二三四五", 7)
        self.assertEqual(parts, ["一二", "三四", "五"])
        self.assertTrue(all(len(part.encode("utf-8")) <= 7 for part in parts))

    def test_packets(self):
        packets = build_text_packets("M2", 77, "三号巷道风机有异响", 12)
        self.assertGreater(len(packets), 1)
        self.assertTrue(packets[0].startswith("TXT,M2,77,1/"))
        text = "".join(packet.split(",", 4)[4] for packet in packets)
        self.assertEqual(text, "三号巷道风机有异响")

    def test_ack(self):
        self.assertEqual(parse_ack(b"ACK,M2,77,2").part, 2)
        self.assertIsNone(parse_ack(b"VOICE,M1,HELP,1"))

    def test_resample_44100_to_16000(self):
        source = np.zeros(44100, dtype=np.int16)
        output = _resample_linear(source, 44100, 16000)
        self.assertEqual(output.dtype, np.int16)
        self.assertEqual(output.size, 16000)


if __name__ == "__main__":
    unittest.main()
