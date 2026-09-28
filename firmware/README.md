# Firmware Guide

This folder contains the ESP32 firmware for your mine safety system.

## Files
- `esp32s3-kws/` — Node 1 PlatformIO project (ESP32-S3 N16R8 + INMP441 + Ra-02).
  Runs offline Chinese command recognition with ESP-SR MultiNet6 and sends `VOICE` / `STATUS`
  packets. MQ-4/MQ-7 sampling is present in code but disabled in the current debug configuration.
  Build and upload from this folder; see its README and `include/onenet_secrets.example.h`.
- `surface_node_vscode/` — surface gateway PlatformIO project (ESP32-S3 N16R8 + Ra-02).
  Receives `VOICE / DATA / STATUS` packets, displays events on a Chinese SH1106 OLED,
  sounds the buzzer, sends three delayed ACK packets for voice events, and uploads status/events
  to Firebase Realtime Database. Build with `pio run -t upload`; see `include/secrets.example.h`.
- `voice_node_vscode/` — ESP-IDF alternative implementation of the underground voice node.
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
