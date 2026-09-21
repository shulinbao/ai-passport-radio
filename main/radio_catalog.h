// main/radio_catalog.h —— 内置精选电台目录(香港 / 新加坡 / 马来西亚)。
//
// 设计取舍:目录【编进固件】而不是运行时从公共目录拉取。原因有三:
//   1. 用户要的是"我常听的那几个台",公共目录按国家拉回来的结果不可控;
//   2. 流地址会失效,内置目录配合逐条实测才能在交付时给出"哪些台是通的";
//   3. 开机即用,不依赖第三方服务的可用性。
// 代价是新增电台要重新出固件 —— 对这个产品形态可以接受。
//
// 每条记录的 url 都是本机实测过的:HTTP 200,且连续 >= 5 个合法帧头
// (MP3 帧同步 0xFFEx / AAC-ADTS 同步 0xFFFx),排除"能返回 200 但其实是
// HTML/播放列表/m3u8"的地址。未通过实测的台不写进来。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_app.h"   // radio_region_t

typedef enum {
    RADIO_CODEC_UNKNOWN = 0,
    RADIO_CODEC_MP3,     // MPEG 音频,esp_audio_codec 的 MP3 解码器
    RADIO_CODEC_AAC,     // ADTS AAC,esp_audio_codec 的 AAC 解码器
} radio_codec_t;

typedef struct {
    uint16_t id;          // 稳定 id(从 1 开始),收藏记录里存的就是它
    uint8_t region;       // radio_region_t
    uint8_t codec;        // radio_codec_t
    uint16_t freq_deci;   // FM 频率 x10(945 = 94.5MHz);0 表示无 FM 频率(纯网络台/AM)
    const char *name;     // 台名,保留当地写法(英文台名不强行翻译)
    const char *tagline;  // 一句话中文说明
    const char *url;      // 流地址
} radio_station_t;

// 某个地区的电台数量与按序号取用。
int radio_catalog_region_count(uint8_t region);
const radio_station_t *radio_catalog_region_at(uint8_t region, int index);

// 全表访问(收藏页按 id 反查时用)。
int radio_catalog_total(void);
const radio_station_t *radio_catalog_all_at(int index);

// 按 id 反查;找不到返回 NULL。
const radio_station_t *radio_catalog_by_id(uint16_t id);

// 按 URL 精确匹配;找不到返回 NULL。用于把"上次播放的台"还原成目录条目。
const radio_station_t *radio_catalog_by_url(const char *url);

// 把 id 列表(收藏)映射成电台指针数组,跳过目录里已不存在的 id(固件升级后
// 某个台被移除的情况)。返回实际写入的条目数。
int radio_catalog_resolve_ids(const uint16_t *ids, int count,
                              const radio_station_t **out, int out_cap);

// 频率是否可用于界面显示(0 表示没有可信频率,界面应留空而不是编一个)。
static inline bool radio_catalog_has_frequency(const radio_station_t *station) {
    return station != NULL && station->freq_deci >= 870 && station->freq_deci <= 1080;
}
