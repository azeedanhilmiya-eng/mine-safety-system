# Firmware Guide

This folder contains the ESP32 firmware for your mine safety system.

## Files
- `surface_node_vscode/` — **current** surface gateway, PlatformIO project (ESP32-S3 N16R8 + Ra-02).
  Receives `VOICE / DATA / STATUS` packets from Node 1 and Node 2, shows them on a SH1106 OLED
  with a Chinese font, and uploads `/status/<mineN>` + `/alerts/<mineN>` to
  Firebase Realtime Database. Receive-only (no ACK / no downlink commands).
  Build with `pio run -t upload` inside that folder; see its `include/secrets.example.h`.
- `voice_node_vscode/` — ESP-IDF version of the underground voice node (alternative to `esp32s3-kws/`).
- `underground_node1_mine1.ino` — early Arduino IDE firmware for underground Node 1
- `underground_node2_mine2.ino` — early Arduino IDE firmware for underground Node 2
- `surface_node.ino` — early Arduino IDE surface gateway (LoRa + GSM SMS + OLED + buzzer).
  Uses the old `A,<nodeId>,…` / `T,<nodeId>,…` protocol, so it does **not** talk to the current
  `esp32s3-kws` node; it also still contains an unresolved git merge conflict at the end of the file.

## Recommended Arduino IDE Settings
- Board: `ESP32S3 Dev Module`
- Flash Mode: `QIO` 80MHz
- Flash Size: `16MB (128Mb)`
- Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- PSRAM: `OPI PSRAM`
- Upload Speed: `921600`

## Notes
- Calibrate MQ sensor thresholds after sensor warm-up.
- Use unique node IDs: `M1` and `M2`.
- The surface node uploads data to Firebase and (in the old `.ino` version) also sends SMS alerts.
- Firebase database URL / WiFi credentials now live in `surface_node_vscode/include/secrets.h`
  (copy from `secrets.example.h`, git-ignored).
