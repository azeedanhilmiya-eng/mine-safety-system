"""Extend the exported OneNET Studio model without losing existing fields."""

import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
source = HERE / "onenet_model_bool_sample.json"
target = HERE / "onenet_model_3nodes.json"
model = json.loads(source.read_text(encoding="utf-8-sig"))


def field(identifier, name, kind, specs):
    return {
        "identifier": identifier,
        "name": name,
        "functionType": "u",
        "accessMode": "r",
        "desc": "",
        "dataType": {"type": kind, "specs": specs},
        "functionMode": "property",
        "required": False,
    }


def boolean(identifier, name):
    return field(identifier, name, "bool", {"true": "是", "false": "否"})


def integer(identifier, name, minimum, maximum, unit=""):
    return field(
        identifier,
        name,
        "int32",
        {"max": str(maximum), "min": str(minimum), "step": "1", "unit": unit},
    )


def string(identifier, name, length=255):
    return field(identifier, name, "string", {"length": str(length)})


new_fields = [
    string("m1_event", "M1语音事件", 64),
    integer("m1_event_seq", "M1事件序号", 0, 2147483647),
    boolean("m1_lora_online", "M1射频在线"),
    integer("m1_rssi", "M1信号强度", -160, 0),
    boolean("m1_alarm", "M1报警"),
    string("m1_rx_event", "M1网关接收事件"),
    boolean("m2_lora_online", "M2射频在线"),
    integer("m2_rssi", "M2信号强度", -160, 0),
    boolean("m2_alarm", "M2报警"),
    string("m2_text", "M2转写文本"),
    boolean("gw_wifi", "网关WiFi连接"),
    integer("gw_uptime", "网关运行秒数", 0, 2147483647),
]

seen = {entry["identifier"] for entry in model["properties"]}
for entry in new_fields:
    if entry["identifier"] not in seen:
        model["properties"].append(entry)
        seen.add(entry["identifier"])

target.write_text(json.dumps(model, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Prepared {len(model['properties'])} properties: {target}")
