from __future__ import annotations

import base64
import hashlib
import hmac
import json
import logging
import threading
import time
from pathlib import Path
from urllib.parse import quote

import paho.mqtt.client as mqtt


LOGGER = logging.getLogger(__name__)
ALARM_WORDS = ("救命", "求救", "被困", "火灾", "着火", "冒烟", "瓦斯", "危险")


def _token(product_id: str, device_name: str, device_secret: str) -> str:
    resource = f"products/{product_id}/devices/{device_name}"
    expiry = "2147483647"
    message = f"{expiry}\nsha1\n{resource}\n2018-10-31".encode("utf-8")
    signature = base64.b64encode(
        hmac.new(base64.b64decode(device_secret), message, hashlib.sha1).digest()
    ).decode("ascii")
    return (
        f"version=2018-10-31&res={quote(resource, safe='')}&et={expiry}"
        f"&method=sha1&sign={quote(signature, safe='')}"
    )


class OneNetPublisher:
    def __init__(self, secrets_path: Path) -> None:
        secrets = json.loads(secrets_path.read_text(encoding="utf-8"))
        self.product_id = secrets["product_id"]
        self.device_name = secrets["device_name"]
        self.topic = f"$sys/{self.product_id}/{self.device_name}/thing/property/post"
        self._lock = threading.Lock()
        self._pending: dict | None = None
        self._connected = False
        self._client = mqtt.Client(client_id=self.device_name, protocol=mqtt.MQTTv311)
        self._client.username_pw_set(
            self.product_id,
            _token(self.product_id, self.device_name, secrets["device_secret"]),
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(min_delay=2, max_delay=30)
        self._client.connect_async("mqtts.heclouds.com", 1883, keepalive=60)
        self._client.loop_start()

    def _on_connect(self, client, userdata, flags, rc) -> None:
        if rc != 0:
            LOGGER.warning("OneNET M2 连接失败：%s", rc)
            return
        with self._lock:
            self._connected = True
            pending = self._pending
            self._pending = None
        LOGGER.info("OneNET M2 已连接")
        client.subscribe(self.topic + "/reply")
        if pending is not None:
            self._publish(pending)
        else:
            self._publish({"m2_alarm": {"value": False}})

    def _on_disconnect(self, client, userdata, rc) -> None:
        with self._lock:
            self._connected = False
        LOGGER.warning("OneNET M2 连接断开：%s", rc)

    def _on_message(self, client, userdata, message) -> None:
        try:
            reply = json.loads(message.payload.decode("utf-8"))
            if reply.get("code", 0) not in (0, 200):
                LOGGER.warning("OneNET M2 属性上报拒绝：%s", reply)
        except (UnicodeDecodeError, json.JSONDecodeError):
            LOGGER.warning("OneNET M2 返回无法解析的上报回执")

    def _publish(self, params: dict) -> None:
        payload = json.dumps(
            {"id": str(int(time.time() * 1000)), "version": "1.0", "params": params},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        result = self._client.publish(self.topic, payload, qos=1)
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            LOGGER.warning("OneNET M2 发送失败：%s", result.rc)

    def publish_text(self, text: str) -> None:
        encoded = text.encode("utf-8")[:255]
        while encoded:
            try:
                cloud_text = encoded.decode("utf-8")
                break
            except UnicodeDecodeError:
                encoded = encoded[:-1]
        else:
            cloud_text = ""
        params = {
            "m2_text": {"value": cloud_text},
            "m2_alarm": {"value": any(word in text for word in ALARM_WORDS)},
        }
        with self._lock:
            if not self._connected:
                self._pending = params
                LOGGER.info("OneNET M2 暂未连接，保留最新转写待上报")
                return
        self._publish(params)

    def close(self) -> None:
        self._client.disconnect()
        self._client.loop_stop()
