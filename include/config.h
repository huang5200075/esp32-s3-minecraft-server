#ifndef H_CONFIG
#define H_CONFIG

// ============================================================================
//  esp32-mcserver — 配置
//
//  这是占位配置，请填入你自己的 WiFi 信息后再编译！
//  参考同目录下的 config.h.example 了解每一项的含义。
// ============================================================================

// --- WiFi（仅支持 2.4GHz，ESP32 无法连接 5GHz 网络）-----------------------
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// --- 服务器标识 -------------------------------------------------------------
#define SERVER_MOTD "ESP32-S3 Minecraft Server"

// --- 玩法设置 ---------------------------------------------------------------
// 0 生存 / 1 创造 / 2 冒险 / 3 观察
#define INITIAL_GAMEMODE 0

// 渲染/模拟距离（区块）。2 是安全默认值，调高会显著增加 CPU 与内存消耗。
#define VIEW_DISTANCE 2

// 最大同时在线玩家数
#define MAX_PLAYERS 16

// 创造模式破坏方块是否也给物品（取消注释即开启）
// #define CREATIVE_DROPS_ITEMS

// 世界编辑预算：记录多少条方块改动。
// ⚠️ 修改此值会让旧存档无法读取，需擦除 Flash 后重新开始。
#define MAX_BLOCK_CHANGES 20000

#endif
