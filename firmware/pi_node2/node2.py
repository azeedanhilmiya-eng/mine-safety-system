#!/usr/bin/env python3
"""
矿安智联 · 井下节点2 (树莓派 5 + SX1278) —— 日常文本 LoRa 发送端

把一句句文字切成 TXT 分片发给地面网关, 逐片等网关的 ACK, 超时重发;
同时每 4~6 s 发一次 STATUS 心跳, 网关据此显示"节点2 在线"。

文字来源 (ASR 接入前先用这两种联调, 对应方案第八章"先用固定文本验证分片重组"):
    python3 node2.py                      # 从标准输入逐行读, 每行是一句; 输入结束(Ctrl-D)后发完即退出
    echo "三号巷道风机有异响" | python3 node2.py
    python3 node2.py --demo               # 每 20 s 循环发一句内置巡检语料, 适合长时间挂测
    python3 node2.py --fake-radio         # 不接 Ra-02, 在本机模拟网关回执(调逻辑用)
以后接离线 ASR 时, 让 ASR 程序每识别完一句就往 stdout 打一行, 用管道接进来即可:
    python3 asr.py | python3 node2.py

信道礼让(方案 P0/P2 优先级): 节点2 与节点1 同频, 能听到节点1 的包。
一旦听到别的节点的 VOICE 应急包, 暂停自己的所有发送 3 s, 把信道让给应急包和网关回执。
"""

from __future__ import annotations

import argparse
import logging
import queue
import random
import signal
import sys
import threading
import time
from collections import deque
from typing import Deque, List, Optional, Tuple

import protocol as proto

log = logging.getLogger("node2")

# ---------------- 时间参数 ----------------
HEARTBEAT_MIN_S = 4.0     # 心跳间隔随机取 4~6 s: 网关 15 s 没收到判离线;
HEARTBEAT_MAX_S = 6.0     # 加随机是为了不和节点1 的 5 s 心跳长期撞在同一时刻
ACK_TIMEOUT_S = 2.0       # 网关收到后约 0.2 s 回执; 网关正在上传云端时会晚一些
MAX_RETRY = 3             # 单片最多重发 3 次(方案 7.1)
RETRY_BACKOFF_S = (0.2, 0.8)
FRAGMENT_GAP_S = (0.05, 0.15)
LBT_BACKOFF_S = (0.05, 0.2)     # 信道忙时的退避
P0_YIELD_S = 3.0          # 听到别的节点的 VOICE 后让出信道的时长
RX_GUARD_S = 0.05         # 刚听到任何包后稍等, 对方可能马上要回执
LOOP_SLEEP_S = 0.005

DEMO_SENTENCES = [
    "三号巷道风机有异响，请派人检查",
    "二号采区巡检完毕，一切正常",
    "主运输皮带跑偏，已停机处理",
    "五号联络巷顶板有少量掉渣，已设置警戒",
    "水泵房排水正常，水位稳定",
]


class FakeRadio:
    """不接 Ra-02 时的替身: 打印发出的包, 并模拟网关对 TXT 分片回 ACK。
    drop 为模拟丢包率(0~1), 用来验证重发逻辑。"""

    def __init__(self, drop: float = 0.0):
        self.drop = drop
        self._rx: Deque[Tuple[bytes, int, float]] = deque()

    def begin(self) -> None:
        log.info("使用模拟射频 (--fake-radio), 不会真正发射")

    def close(self) -> None:
        pass

    def channel_busy(self) -> bool:
        return False

    def receive(self) -> Optional[Tuple[bytes, int, float]]:
        return self._rx.popleft() if self._rx else None

    def send(self, payload: bytes) -> bool:
        log.info("[模拟空口] %s", payload.decode("utf-8", "replace"))
        if random.random() < self.drop:
            log.info("[模拟空口] 该包被模拟丢弃")
            return True
        kind, node = proto.packet_kind(payload)
        if kind == "TXT":
            _, _, seq, frac, _ = payload.decode("utf-8").split(",", 4)
            part = frac.split("/")[0]
            self._rx.append((f"ACK,{node},{seq},{part}".encode(), -60, 9.0))
        return True


class Node2:
    def __init__(self, radio, state: str = "TXT_READY"):
        self.radio = radio
        self.state = state
        self.inbox: "queue.Queue[str]" = queue.Queue()
        self.seq = random.randint(1, 9999)   # 随机起点: 重启后不会和网关记住的上一条序号撞车

        now = time.monotonic()
        self.next_heartbeat = now + 1.0
        self.hold_until = 0.0                # 信道礼让截止时刻, 期间不发任何包

        # 当前正在发送的消息
        self.pending: Deque[List[str]] = deque()  # 待发消息(每条是分片列表)
        self.msg_parts: Optional[List[str]] = None
        self.msg_seq = 0
        self.msg_started = 0.0
        self.part_idx = 0          # 正在发的片(0 起)
        self.attempts = 0          # 当前片已发送次数
        self.retries_total = 0
        self.waiting_ack = False
        self.ack_deadline = 0.0
        self.next_send = 0.0

        self.sent_ok = 0
        self.sent_fail = 0

    # ---------------- 对外接口 ----------------
    def submit(self, text: str) -> None:
        """线程安全: 任何线程都可以塞一句话进来。"""
        self.inbox.put(text)

    @property
    def idle(self) -> bool:
        return self.msg_parts is None and not self.pending and self.inbox.empty()

    def step(self) -> None:
        now = time.monotonic()
        self._poll_rx(now)
        self._heartbeat(now)
        self._send_text(now)

    # ---------------- 接收 ----------------
    def _poll_rx(self, now: float) -> None:
        while True:
            pkt = self.radio.receive()
            if pkt is None:
                return
            data, rssi, snr = pkt
            text = data.decode("utf-8", "replace")
            ack = proto.parse_ack(data)
            if ack is not None:
                self._on_ack(now, ack, rssi)
                continue

            kind, node = proto.packet_kind(data)
            if kind == "VOICE" and node != proto.NODE_ID:
                # 节点1 的应急包: 网关 0.5 s 后要给它连发回执, 这段时间别占信道
                self.hold_until = max(self.hold_until, now + P0_YIELD_S)
                log.info("[礼让] 听到 %s 应急包 (%s, RSSI %d), 暂停发送 %.0f s",
                         node, text, rssi, P0_YIELD_S)
            else:
                self.hold_until = max(self.hold_until, now + RX_GUARD_S)
                log.debug("[旁听] %s RSSI=%d SNR=%.1f", text, rssi, snr)

    def _on_ack(self, now: float, ack: Tuple[int, int], rssi: int) -> None:
        seq, part = ack
        if self.msg_parts is None or seq != self.msg_seq or part != self.part_idx + 1:
            log.debug("[回执] 过期回执 seq=%d part=%d, 忽略", seq, part)
            return
        log.info("[回执] seq=%d 第 %d/%d 片已确认 (RSSI %d)",
                 seq, part, len(self.msg_parts), rssi)
        self.waiting_ack = False
        self.part_idx += 1
        self.attempts = 0
        if self.part_idx >= len(self.msg_parts):
            self.sent_ok += 1
            log.info("[完成] seq=%d 共 %d 片, 用时 %.2f s, 重发 %d 次: %s",
                     self.msg_seq, len(self.msg_parts), now - self.msg_started,
                     self.retries_total, "".join(self.msg_parts))
            self.msg_parts = None
        else:
            self.next_send = now + random.uniform(*FRAGMENT_GAP_S)

    # ---------------- 发送 ----------------
    def _can_transmit(self, now: float) -> bool:
        if now < self.hold_until:
            return False
        if self.radio.channel_busy():
            self.hold_until = now + random.uniform(*LBT_BACKOFF_S)
            return False
        return True

    def _heartbeat(self, now: float) -> None:
        # 等回执期间不发心跳, 免得和网关的回执撞上; 最多推迟 ACK_TIMEOUT_S
        if now < self.next_heartbeat or self.waiting_ack or not self._can_transmit(now):
            return
        self.seq = proto.next_seq(self.seq)
        pkt = proto.build_status(self.state, self.seq)
        if self.radio.send(pkt):
            log.debug("[心跳] %s", pkt.decode())
        self.next_heartbeat = now + random.uniform(HEARTBEAT_MIN_S, HEARTBEAT_MAX_S)

    def _load_next_message(self, now: float) -> None:
        while not self.inbox.empty():
            text = self.inbox.get_nowait()
            msgs = proto.split_messages(text)
            if not msgs:
                continue
            if len(msgs) > 1:
                log.info("[分片] 文本较长, 拆成 %d 条消息发送", len(msgs))
            self.pending.extend(msgs)
        if self.msg_parts is None and self.pending:
            self.msg_parts = self.pending.popleft()
            self.seq = proto.next_seq(self.seq)
            self.msg_seq = self.seq
            self.msg_started = now
            self.part_idx = 0
            self.attempts = 0
            self.retries_total = 0
            self.waiting_ack = False
            self.next_send = now
            log.info("[新消息] seq=%d 共 %d 片: %s",
                     self.msg_seq, len(self.msg_parts), "".join(self.msg_parts))

    def _send_text(self, now: float) -> None:
        self._load_next_message(now)
        if self.msg_parts is None:
            return

        if self.waiting_ack:
            if now < self.ack_deadline:
                return
            self.waiting_ack = False
            if self.attempts > MAX_RETRY:
                self.sent_fail += 1
                log.error("[失败] seq=%d 第 %d/%d 片重发 %d 次仍无回执, 放弃本条: %s",
                          self.msg_seq, self.part_idx + 1, len(self.msg_parts), MAX_RETRY,
                          "".join(self.msg_parts))
                self.msg_parts = None
                return
            self.next_send = now + random.uniform(*RETRY_BACKOFF_S)
            log.warning("[超时] seq=%d 第 %d 片无回执, 准备第 %d 次重发",
                        self.msg_seq, self.part_idx + 1, self.attempts)
            return

        if now < self.next_send or not self._can_transmit(now):
            return

        total = len(self.msg_parts)
        pkt = proto.build_txt(self.msg_seq, self.part_idx + 1, total, self.msg_parts[self.part_idx])
        if self.attempts > 0:
            self.retries_total += 1
        self.attempts += 1
        if not self.radio.send(pkt):
            # 射频没报发送完成: 按一次失败处理, 走同样的重发流程
            log.error("[发送] 射频未确认发送完成 seq=%d part=%d", self.msg_seq, self.part_idx + 1)
        else:
            log.info("[发送] %s (%d 字节)", pkt.decode("utf-8"), len(pkt))
        self.waiting_ack = True
        self.ack_deadline = time.monotonic() + ACK_TIMEOUT_S


def stdin_reader(node: Node2, done: threading.Event) -> None:
    for line in sys.stdin:
        line = line.strip()
        if line:
            node.submit(line)
    done.set()


def demo_feeder(node: Node2, interval: float, stop: threading.Event) -> None:
    i = 0
    while not stop.wait(0 if i == 0 else interval):
        node.submit(DEMO_SENTENCES[i % len(DEMO_SENTENCES)])
        i += 1


def main() -> int:
    ap = argparse.ArgumentParser(description="矿安智联 井下节点2: 文本 -> LoRa TXT 分片")
    ap.add_argument("--demo", action="store_true", help="循环发送内置巡检语料, 不读标准输入")
    ap.add_argument("--demo-interval", type=float, default=20.0, help="--demo 每句间隔秒数")
    ap.add_argument("--fake-radio", action="store_true", help="不接 Ra-02, 模拟网关回执")
    ap.add_argument("--fake-drop", type=float, default=0.0, help="--fake-radio 的模拟丢包率 0~1")
    ap.add_argument("--reset-gpio", type=int, default=25, help="Ra-02 RESET 接的 BCM 编号, -1 表示不接")
    ap.add_argument("--gpiochip", type=int, default=None, help="默认先试 gpiochip4 再试 gpiochip0")
    ap.add_argument("--spi", default="0.0", help="SPI 总线.片选, 默认 0.0 即 /dev/spidev0.0")
    ap.add_argument("--state", default="TXT_READY", help="心跳里上报的状态字")
    ap.add_argument("--log-file", default=None, help="额外把日志追加写入该文件")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印心跳和旁听到的包")
    args = ap.parse_args()

    handlers: List[logging.Handler] = [logging.StreamHandler(sys.stderr)]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding="utf-8"))
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname).1s %(message)s",
                        datefmt="%H:%M:%S", handlers=handlers)

    if args.fake_radio:
        radio = FakeRadio(args.fake_drop)
    else:
        from sx1278 import SX1278, RadioError
        bus, dev = (int(x) for x in args.spi.split("."))
        radio = SX1278(spi_bus=bus, spi_dev=dev,
                       reset_gpio=None if args.reset_gpio < 0 else args.reset_gpio,
                       gpiochip=args.gpiochip)
    try:
        radio.begin()
    except Exception as e:
        log.error("LoRa 初始化失败: %s", e)
        return 1

    node = Node2(radio, state=args.state)
    stop = threading.Event()
    input_done = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    if args.demo:
        threading.Thread(target=demo_feeder, args=(node, args.demo_interval, stop),
                         daemon=True).start()
    else:
        if sys.stdin.isatty():
            log.info("请输入要发送的文字, 回车发送一句, Ctrl-D 结束")
        threading.Thread(target=stdin_reader, args=(node, input_done), daemon=True).start()

    log.info("节点2 已启动 (节点号 %s)", proto.NODE_ID)
    try:
        while not stop.is_set():
            node.step()
            if input_done.is_set() and node.idle:
                break
            time.sleep(LOOP_SLEEP_S)
    except KeyboardInterrupt:
        pass
    finally:
        log.info("退出: 成功 %d 条, 失败 %d 条", node.sent_ok, node.sent_fail)
        radio.close()
    return 0 if node.sent_fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
