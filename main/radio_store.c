// main/radio_store.c —— 设置持久化实现。
#include "radio_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_app.h"

#include <string.h>

static const char *TAG = "radio_store";

#define NVS_NAMESPACE "radio"

#define KEY_VOLUME RADIO_STORE_KEY_VOLUME
#define KEY_BRIGHT RADIO_STORE_KEY_BRIGHT
#define KEY_AUTOOFF RADIO_STORE_KEY_AUTOOFF
#define KEY_REGION RADIO_STORE_KEY_REGION
#define KEY_LAST_URL RADIO_STORE_KEY_LAST_URL
#define KEY_WAS_PLAYING RADIO_STORE_KEY_WAS_PLAYING
#define KEY_FAVORITES "fav"

// 与 radio_ui / radio_app 里的默认值保持一致。
#define DEFAULT_VOLUME 55
#define DEFAULT_BRIGHTNESS 70
#define DEFAULT_AUTOOFF 0
#define DEFAULT_REGION RADIO_REGION_HK

static bool open_rw(nvs_handle_t *handle) {
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle) != ESP_OK) return false;
    return true;
}

static uint8_t read_u8(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

static void read_str(const char *key, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = cap;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) out[0] = '\0';
    nvs_close(handle);
}

void radio_store_load(radio_store_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->volume = read_u8(KEY_VOLUME, DEFAULT_VOLUME);
    out->brightness = read_u8(KEY_BRIGHT, DEFAULT_BRIGHTNESS);
    out->auto_off_step = read_u8(KEY_AUTOOFF, DEFAULT_AUTOOFF);
    out->region = read_u8(KEY_REGION, DEFAULT_REGION);
    out->was_playing = read_u8(KEY_WAS_PLAYING, 0) != 0 ? 1 : 0;
    read_str(KEY_LAST_URL, out->last_url, sizeof(out->last_url));

    // 越界值一律拉回合法区间:旧固件写过的值或损坏的数据不应该让界面进入
    // 不可用状态(例如亮度 0 让人以为设备没开机)。
    if (out->volume > 100) out->volume = DEFAULT_VOLUME;
    if (out->brightness < 10 || out->brightness > 100) out->brightness = DEFAULT_BRIGHTNESS;
    if (out->auto_off_step >= RADIO_AUTO_OFF_STEPS) out->auto_off_step = DEFAULT_AUTOOFF;
    if (out->region >= RADIO_REGION_COUNT) out->region = DEFAULT_REGION;

    radio_favorites_clear(&out->favorites);
    uint8_t blob[RADIO_FAVORITES_MAX * 2 + 3];
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t length = sizeof(blob);
        if (nvs_get_blob(handle, KEY_FAVORITES, blob, &length) == ESP_OK) {
            if (!radio_favorites_deserialize(&out->favorites, blob, length)) {
                ESP_LOGW(TAG, "收藏记录格式不符,已按空收藏处理");
            }
        }
        nvs_close(handle);
    }
}

esp_err_t radio_store_set_u8(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (!open_rw(&handle)) return ESP_FAIL;
    esp_err_t err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t radio_store_set_str(const char *key, const char *value) {
    nvs_handle_t handle;
    if (!open_rw(&handle)) return ESP_FAIL;
    esp_err_t err = nvs_set_str(handle, key, value != NULL ? value : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t radio_store_save_favorites(const radio_favorites_t *fav) {
    if (fav == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t blob[RADIO_FAVORITES_MAX * 2 + 3];
    const size_t length = radio_favorites_serialize(fav, blob, sizeof(blob));
    if (length == 0) return ESP_FAIL;

    nvs_handle_t handle;
    if (!open_rw(&handle)) return ESP_FAIL;
    esp_err_t err = nvs_set_blob(handle, KEY_FAVORITES, blob, length);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
