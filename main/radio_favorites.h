// main/radio_favorites.h —— 收藏电台集合(纯逻辑,+ 一个稳定的持久化编码)。
//
// 只存电台的数值 id,不存 URL:URL 最长 256 字节,存 20 条就是 5KB 内部 RAM,
// 而这块板子没有 PSRAM,每一 KB 都要省。id 由电台表静态分配且不随版本变化,
// 因此换固件后旧的收藏记录仍然有效。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 收藏上限。20 条足够覆盖"常听的台",同时保证 NVS blob 很小(42 字节)。
#define RADIO_FAVORITES_MAX 20

typedef struct {
    uint16_t ids[RADIO_FAVORITES_MAX];
    uint8_t count;
} radio_favorites_t;

void radio_favorites_clear(radio_favorites_t *fav);

// 追加到末尾。已存在或已满返回 false(已存在不算错误,调用方按幂等处理)。
bool radio_favorites_add(radio_favorites_t *fav, uint16_t id);
bool radio_favorites_remove(radio_favorites_t *fav, uint16_t id);
bool radio_favorites_toggle(radio_favorites_t *fav, uint16_t id);
bool radio_favorites_contains(const radio_favorites_t *fav, uint16_t id);

int radio_favorites_count(const radio_favorites_t *fav);
// 越界返回 0(0 保留为"无效 id",电台 id 从 1 开始)。
uint16_t radio_favorites_at(const radio_favorites_t *fav, int index);

// 序列化格式(小端,共 3 + 2*count 字节):
//   [0]      魔数高字节 'R'
//   [1]      魔数低字节 'F'
//   [2]      版本号
//   [3..]    每条 2 字节 id
// 返回写入的字节数;cap 不足时返回 0 且不写入。
size_t radio_favorites_serialize(const radio_favorites_t *fav, uint8_t *out, size_t cap);

// 反序列化。任何格式不符(魔数、版本、长度、重复 id、个数超限)都整份丢弃并返回
// false:宁可回到空收藏,也不要让半截数据把界面搞乱。
bool radio_favorites_deserialize(radio_favorites_t *fav, const uint8_t *in, size_t len);
