// main/radio_favorites.c —— 收藏集合实现。纯逻辑,无 ESP-IDF 依赖。
#include "radio_favorites.h"

#include <string.h>

#define FAV_MAGIC0 0x52u  // 'R'
#define FAV_MAGIC1 0x46u  // 'F'
#define FAV_VERSION 1u
#define FAV_HEADER 3u

void radio_favorites_clear(radio_favorites_t *fav) {
    if (fav == NULL) return;
    memset(fav, 0, sizeof(*fav));
}

static int find_index(const radio_favorites_t *fav, uint16_t id) {
    for (uint8_t i = 0; i < fav->count; i++) {
        if (fav->ids[i] == id) return (int)i;
    }
    return -1;
}

bool radio_favorites_contains(const radio_favorites_t *fav, uint16_t id) {
    if (fav == NULL || id == 0) return false;
    return find_index(fav, id) >= 0;
}

bool radio_favorites_add(radio_favorites_t *fav, uint16_t id) {
    if (fav == NULL || id == 0) return false;
    if (find_index(fav, id) >= 0) return false;
    if (fav->count >= RADIO_FAVORITES_MAX) return false;
    fav->ids[fav->count] = id;
    fav->count++;
    return true;
}

bool radio_favorites_remove(radio_favorites_t *fav, uint16_t id) {
    if (fav == NULL) return false;
    const int index = find_index(fav, id);
    if (index < 0) return false;
    // 后面的条目整体前移,保持插入顺序 —— 收藏列表按"加的先后"排更符合直觉。
    for (uint8_t i = (uint8_t)index; i + 1 < fav->count; i++) {
        fav->ids[i] = fav->ids[i + 1];
    }
    fav->count--;
    fav->ids[fav->count] = 0;
    return true;
}

bool radio_favorites_toggle(radio_favorites_t *fav, uint16_t id) {
    if (fav == NULL || id == 0) return false;
    if (radio_favorites_remove(fav, id)) return true;
    return radio_favorites_add(fav, id);
}

int radio_favorites_count(const radio_favorites_t *fav) {
    if (fav == NULL) return 0;
    return (int)fav->count;
}

uint16_t radio_favorites_at(const radio_favorites_t *fav, int index) {
    if (fav == NULL || index < 0 || index >= (int)fav->count) return 0;
    return fav->ids[index];
}

size_t radio_favorites_serialize(const radio_favorites_t *fav, uint8_t *out, size_t cap) {
    if (fav == NULL || out == NULL) return 0;
    const size_t need = FAV_HEADER + (size_t)fav->count * 2u;
    if (cap < need) return 0;
    out[0] = FAV_MAGIC0;
    out[1] = FAV_MAGIC1;
    out[2] = FAV_VERSION;
    for (uint8_t i = 0; i < fav->count; i++) {
        out[FAV_HEADER + i * 2u] = (uint8_t)(fav->ids[i] & 0xFFu);
        out[FAV_HEADER + i * 2u + 1u] = (uint8_t)(fav->ids[i] >> 8);
    }
    return need;
}

bool radio_favorites_deserialize(radio_favorites_t *fav, const uint8_t *in, size_t len) {
    if (fav == NULL) return false;
    radio_favorites_clear(fav);
    if (in == NULL || len < FAV_HEADER) return false;
    if (in[0] != FAV_MAGIC0 || in[1] != FAV_MAGIC1 || in[2] != FAV_VERSION) return false;

    const size_t body = len - FAV_HEADER;
    if (body % 2u != 0) return false;
    const size_t count = body / 2u;
    if (count > RADIO_FAVORITES_MAX) return false;
    // 长度必须和声明完全一致,多余字节说明是我们不认识的格式。
    if (FAV_HEADER + count * 2u != len) return false;

    radio_favorites_t parsed;
    radio_favorites_clear(&parsed);
    for (size_t i = 0; i < count; i++) {
        const uint16_t id = (uint16_t)(in[FAV_HEADER + i * 2u] |
                                       ((uint16_t)in[FAV_HEADER + i * 2u + 1u] << 8));
        // id 0 无效,重复 id 说明数据被写坏了。
        if (id == 0) return false;
        if (!radio_favorites_add(&parsed, id)) return false;
    }
    *fav = parsed;
    return true;
}
