// main/radio_wifi.h —— Wi-Fi 扫描 / 连接 / 凭据持久化。
//
// 本应用采用【纯设备端配网】:设备自己扫描网络,用户在屏幕上用软键盘输入密码,
// 不做 SoftAP 网页配网。因此这里只负责三件事:扫描、连接、把凭据存进 NVS。
//
// 线程模型:所有耗时动作(扫描、连接)都丢给内部的 wifi 工作任务执行,调用者
// (LVGL 任务 / 按键回调)只投递命令后立即返回。状态用 radio_wifi_state() 轮询,
// 界面按固定周期刷新即可,不需要回调进 LVGL。
//
// 安全:密码只在 NVS 与内存里出现,不写日志、不进错误信息。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_WIFI_SSID_MAX 33          // 32 字节 + NUL
#define RADIO_WIFI_PASS_MAX 65          // 64 字节 + NUL
#define RADIO_WIFI_SCAN_MAX 24          // 屏幕上一次也显示不了更多

typedef enum {
    RADIO_WIFI_STATE_IDLE = 0,          // 未连接,也没有在做事
    RADIO_WIFI_STATE_SCANNING,
    RADIO_WIFI_STATE_CONNECTING,
    RADIO_WIFI_STATE_CONNECTED,
    RADIO_WIFI_STATE_FAILED,            // 上次连接失败(密码错/超时/找不到 AP)
} radio_wifi_state_t;

typedef struct {
    char ssid[RADIO_WIFI_SSID_MAX];
    int8_t rssi;                        // dBm
    uint8_t secure;                     // 1 = 需要密码
} radio_wifi_ap_t;

// 初始化 NVS / netif / 事件循环 / Wi-Fi STA。可重复调用。
esp_err_t radio_wifi_init(void);

// 异步开始扫描。返回 ESP_OK 只表示命令已投递。
esp_err_t radio_wifi_scan_start(void);
int radio_wifi_scan_count(void);
// 越界返回 NULL。
const radio_wifi_ap_t *radio_wifi_scan_at(int index);
// 扫描结果里是否出现过这个 SSID(用于判断是否需要弹软键盘)。
bool radio_wifi_scan_contains(const char *ssid);

// 连接指定网络。save=true 时连同密码写入 NVS;失败不会写入。
esp_err_t radio_wifi_connect(const char *ssid, const char *password, bool save);
// 用已保存的凭据重连。
esp_err_t radio_wifi_autoconnect(void);
// 断开并清除已保存凭据。
esp_err_t radio_wifi_forget(void);
bool radio_wifi_has_credentials(void);
// 取已保存的 SSID / 密码。没有则返回 false。
bool radio_wifi_saved_ssid(char *out, size_t cap);
bool radio_wifi_saved_password(char *out, size_t cap);

radio_wifi_state_t radio_wifi_state(void);
// 状态对应的中文短语,直接用于界面。
const char *radio_wifi_state_text(void);
const char *radio_wifi_current_ssid(void);
int radio_wifi_rssi(void);
// 已获得的 IPv4 地址文本;未连接时返回 ""。
const char *radio_wifi_ip(void);

// 主循环/界面周期调用:推进状态机(例如连接超时判定)。
void radio_wifi_tick(void);
