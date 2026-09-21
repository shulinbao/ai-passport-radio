// main/radio_store.h —— 应用设置的 NVS 持久化。
//
// 与 radio_wifi 共用 "radio" 命名空间,但键名前缀不同(wifi_* / fav / set_*),
// 这样"清除 Wi-Fi 凭据"不会连带清掉音量、亮度、收藏等其他设置。
//
// 这里的写操作都在按键处理路径上发生,所以只做单键写入并立即 commit —— NVS 的
// 单键写入是毫秒级且不会阻塞太久,不需要再排一个任务。真正耗时的网络/音频动作
// 才必须走工作任务。
#pragma once

#include "esp_err.h"
#include "radio_favorites.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t volume;         // 0..100
    uint8_t brightness;     // 10..100
    uint8_t auto_off_step;  // 0..RADIO_AUTO_OFF_STEPS-1
    uint8_t region;         // radio_region_t
    uint8_t was_playing;    // 上次退出时是否正在放音(决定开机要不要自动续播)
    char last_url[256];     // 上次播放的流地址,用于开机恢复
    radio_favorites_t favorites;
} radio_store_t;

// NVS 键名。集中在这里,避免调用方各写一份字符串字面量后拼错却编译通过。
#define RADIO_STORE_KEY_VOLUME "set_vol"
#define RADIO_STORE_KEY_BRIGHT "set_bright"
#define RADIO_STORE_KEY_AUTOOFF "set_autooff"
#define RADIO_STORE_KEY_REGION "set_region"
#define RADIO_STORE_KEY_LAST_URL "last_url"
#define RADIO_STORE_KEY_WAS_PLAYING "was_play"

// 读取全部设置。缺失的键用默认值补齐;任何一项读失败都不会让整体失败。
// NVS 未初始化时会先初始化(幂等)。
void radio_store_load(radio_store_t *out);

esp_err_t radio_store_set_u8(const char *key, uint8_t value);
esp_err_t radio_store_set_str(const char *key, const char *value);
// 收藏集合整体写入(定长小 blob,一次性写比逐条写更省 NVS 空间)。
esp_err_t radio_store_save_favorites(const radio_favorites_t *fav);
