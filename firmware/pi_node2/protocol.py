"""
节点2 与地面网关之间的 LoRa 应用层报文 (CSV, UTF-8), 与《双节点实施方案》第七章一致:

    TXT,<节点>,<序号>,<片号>/<总片数>,<正文>     日常文本分片, 例 TXT,M2,77,1/2,三号巷道风机有异响
    STATUS,<节点>,<状态>,<序号>                  心跳, 例 STATUS,M2,TXT_READY,78
    ACK,<节点>,<序号>,<片号>                     网关逐片回执(下行), 例 ACK,M2,77,1

正文在最后一个字段, 允许包含逗号; 网关只按前 4 个逗号切字段。
分片一定落在 UTF-8 字符边界上, 网关不会拼出半个汉字。
"""

from __future__ import annotations

import re
from typing import List, Optional, Tuple

NODE_ID = "M2"

PART_MAX_BYTES = 80    # 每片正文上限(方案: ≤80 字节), 加包头后整包约 100 字节
MAX_PARTS = 8          # 网关每条消息最多缓存 8 片, 与 surface_node_vscode 的 TXT_MAX_PARTS 一致
SEQ_MAX = 65535

# 优先在这些字符之后断片, 读起来更顺
_BREAK_AFTER = set("，。！？；、,.!?; ")
_CTRL = re.compile(r"[\x00-\x1f\x7f]")
_SPACES = re.compile(r"\s+")


def normalize(text: str) -> str:
    """去控制字符、合并空白。换行也会变成空格, 网关按单行显示。"""
    text = _CTRL.sub(" ", text)
    return _SPACES.sub(" ", text).strip()


def _best_break(chunk: str) -> int:
    """在 chunk 后半段找最后一个标点, 返回断点(不含)位置; 找不到返回 0。"""
    for i in range(len(chunk) - 1, len(chunk) // 2 - 1, -1):
        if chunk[i] in _BREAK_AFTER:
            return i + 1
    return 0


def split_utf8(text: str, max_bytes: int = PART_MAX_BYTES) -> List[str]:
    """按 UTF-8 字节数切片, 只在字符边界上切, 尽量在标点后切。"""
    if max_bytes < 8:
        raise ValueError("max_bytes 太小")
    chunks: List[str] = []
    cur, cur_len = "", 0
    for ch in text:
        n = len(ch.encode("utf-8"))
        if cur_len + n > max_bytes:
            cut = _best_break(cur)
            # 断点之后剩下的部分还要和当前字符拼成下一片, 放不下就不在标点处断
            if cut and len(cur[cut:].encode("utf-8")) + n <= max_bytes:
                chunks.append(cur[:cut])
                cur = cur[cut:]
            else:
                chunks.append(cur)
                cur = ""
            cur_len = len(cur.encode("utf-8"))
        cur += ch
        cur_len += n
    if cur:
        chunks.append(cur)
    return chunks


def split_messages(text: str) -> List[List[str]]:
    """一句话 -> 若干条消息, 每条消息 ≤ MAX_PARTS 片。超长文本会拆成多条独立消息。"""
    text = normalize(text)
    if not text:
        return []
    parts = split_utf8(text)
    return [parts[i:i + MAX_PARTS] for i in range(0, len(parts), MAX_PARTS)]


def build_txt(seq: int, part: int, total: int, body: str, node: str = NODE_ID) -> bytes:
    return f"TXT,{node},{seq},{part}/{total},{body}".encode("utf-8")


def build_status(state: str, seq: int, node: str = NODE_ID) -> bytes:
    return f"STATUS,{node},{state},{seq}".encode("utf-8")


def parse_ack(payload: bytes, node: str = NODE_ID) -> Optional[Tuple[int, int]]:
    """ACK,<节点>,<序号>,<片号> -> (序号, 片号); 不是发给本节点的 TXT 回执返回 None。
    节点1 的回执是 ACK,M1 (没有序号), 这里会被忽略。"""
    try:
        fields = payload.decode("utf-8").strip().split(",")
    except UnicodeDecodeError:
        return None
    if len(fields) != 4 or fields[0] != "ACK" or fields[1] != node:
        return None
    try:
        return int(fields[2]), int(fields[3])
    except ValueError:
        return None


def packet_kind(payload: bytes) -> Tuple[str, str]:
    """返回 (类型, 节点号), 例 (b'VOICE,M1,HELP,3') -> ('VOICE', 'M1')。解析失败返回 ('', '')。"""
    head = payload[:24].split(b",")
    if len(head) < 2:
        return "", ""
    try:
        return head[0].decode("ascii"), head[1].decode("ascii")
    except UnicodeDecodeError:
        return "", ""


def next_seq(seq: int) -> int:
    return 1 if seq >= SEQ_MAX else seq + 1
