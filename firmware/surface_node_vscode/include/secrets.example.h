#pragma once

// 复制本文件为 include/secrets.h 并填入你自己的参数。
// secrets.h 已被 .gitignore 忽略, 不会提交到仓库。
// 没有这个文件时固件仍能编译运行, 只是跳过 WiFi / 云端上报。

// 现场用一个 2.4GHz 热点(手机热点或路由器)给网关联网
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Firebase Realtime Database 地址, 与 web_dashboard/app.js 里的 databaseURL 一致
static const char *FIREBASE_DB_URL = "https://mine-pulse-default-rtdb.firebaseio.com";

// NTP 服务器(用于给上报数据打真实时间戳), 默认用阿里云
static const char *NTP_SERVER_1 = "ntp.aliyun.com";
static const char *NTP_SERVER_2 = "ntp1.aliyun.com";
