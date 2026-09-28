"""离线自测, 不需要树莓派和 Ra-02:  python3 -m unittest -v test_node2"""

import random
import time
import unittest

import node2
import protocol as proto


class ProtocolTest(unittest.TestCase):
    def test_split_respects_bytes_and_utf8(self):
        text = "三号巷道风机有异响，" * 12 + "mixed ASCII 文本 😀 结尾。"
        parts = proto.split_utf8(text)
        self.assertEqual("".join(parts), text)
        for p in parts:
            self.assertLessEqual(len(p.encode("utf-8")), proto.PART_MAX_BYTES)
            p.encode("utf-8").decode("utf-8")   # 每片都是完整 UTF-8

    def test_split_prefers_punctuation(self):
        parts = proto.split_utf8("一二三四五六七八九十，" * 3, max_bytes=40)
        self.assertTrue(parts[0].endswith("，"))

    def test_split_random_never_exceeds(self):
        rnd = random.Random(1)
        alphabet = "矿井巷道，。abc ,😀"
        for _ in range(500):
            s = "".join(rnd.choice(alphabet) for _ in range(rnd.randint(1, 200)))
            parts = proto.split_utf8(s, max_bytes=rnd.randint(8, 90))
            self.assertEqual("".join(parts), s)
            self.assertTrue(all(p for p in parts))

    def test_long_text_becomes_multiple_messages(self):
        msgs = proto.split_messages("巷" * 400)            # 1200 字节, 每片 26 字 -> 16 片
        self.assertEqual([len(m) for m in msgs], [8, 8])

    def test_packet_fits_lora(self):
        pkt = proto.build_txt(65535, 8, 8, "巷" * 26)
        self.assertLessEqual(len(pkt), 255)

    def test_normalize(self):
        self.assertEqual(proto.normalize(" 风机\n异响\t\t了 "), "风机 异响 了")
        self.assertEqual(proto.split_messages("  \n "), [])

    def test_parse_ack(self):
        self.assertEqual(proto.parse_ack(b"ACK,M2,77,1"), (77, 1))
        self.assertIsNone(proto.parse_ack(b"ACK,M1"))        # 节点1 的回执
        self.assertIsNone(proto.parse_ack(b"ACK,M1,77,1"))
        self.assertIsNone(proto.parse_ack(b"STATUS,M1,KWS_READY,3"))
        self.assertIsNone(proto.parse_ack(b"\xff\xfe"))

    def test_packet_kind(self):
        self.assertEqual(proto.packet_kind(b"VOICE,M1,HELP,3"), ("VOICE", "M1"))
        self.assertEqual(proto.packet_kind(b"x"), ("", ""))


def run_until_idle(node, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        node.step()
        if node.idle:
            return True
        time.sleep(0.001)
    return False


class Node2Test(unittest.TestCase):
    def setUp(self):
        self._saved = (node2.ACK_TIMEOUT_S, node2.RETRY_BACKOFF_S, node2.FRAGMENT_GAP_S)
        node2.ACK_TIMEOUT_S = 0.02
        node2.RETRY_BACKOFF_S = (0.001, 0.002)
        node2.FRAGMENT_GAP_S = (0.001, 0.002)

    def tearDown(self):
        node2.ACK_TIMEOUT_S, node2.RETRY_BACKOFF_S, node2.FRAGMENT_GAP_S = self._saved

    def test_delivers_all_fragments(self):
        radio = node2.FakeRadio()
        n = node2.Node2(radio)
        n.submit("三号巷道风机有异响，" * 10)
        self.assertTrue(run_until_idle(n))
        self.assertEqual((n.sent_ok, n.sent_fail), (1, 0))

    def test_retries_on_loss(self):
        random.seed(3)
        radio = node2.FakeRadio(drop=0.4)
        n = node2.Node2(radio)
        for s in node2.DEMO_SENTENCES:
            n.submit(s)
        self.assertTrue(run_until_idle(n))
        self.assertEqual(n.sent_ok + n.sent_fail, len(node2.DEMO_SENTENCES))
        self.assertGreater(n.sent_ok, 0)

    def test_gives_up_after_max_retry(self):
        radio = node2.FakeRadio(drop=1.0)
        n = node2.Node2(radio)
        sent = []
        orig = radio.send
        radio.send = lambda p: (sent.append(p), orig(p))[1]
        n.submit("测试")
        self.assertTrue(run_until_idle(n))
        self.assertEqual((n.sent_ok, n.sent_fail), (0, 1))
        self.assertEqual(sum(p.startswith(b"TXT") for p in sent), 1 + node2.MAX_RETRY)

    def test_yields_to_voice(self):
        radio = node2.FakeRadio()
        n = node2.Node2(radio)
        radio._rx.append((b"VOICE,M1,HELP,5", -70, 8.0))
        n.submit("测试")
        n.step()
        self.assertGreater(n.hold_until - time.monotonic(), node2.P0_YIELD_S - 0.5)
        self.assertEqual(n.part_idx, 0)
        self.assertTrue(n.msg_parts is not None and not n.waiting_ack)


if __name__ == "__main__":
    unittest.main()
