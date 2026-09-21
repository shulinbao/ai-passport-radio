// main/radio_frame.h —— 在裸字节流里定位第一个可信的音频帧头(纯逻辑)。
//
// 为什么需要单独一步:Icecast 服务器在客户端接入时是从"正在播"的位置开始发的,
// 所以流的头几个字节往往落在某一帧的中间(实测 RTHK 的 6 个频道分别在偏移
// 12/86/51/23/93/64 字节处才开始第一帧)。这些半截字节直接丢给解码器,轻则开头
// 一声爆音,重则解码器判定格式错误后复位,表现为"连上了但没声音"。
//
// 这里不仅找同步字,还用"下一帧是否也落在算出来的偏移上"做二次确认。只匹配
// 0xFF 同步字的做法在压缩音频里误判率不低 —— 数据里本来就会出现 0xFF。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RADIO_FRAME_KIND_MP3 = 0,
    RADIO_FRAME_KIND_AAC_ADTS,
} radio_frame_kind_t;

// 在 data[0..len) 中找第一个通过"连续两帧"校验的帧头,把偏移写入 offset。
// 找不到返回 false(调用方应继续读更多数据再试)。
bool radio_frame_find(const uint8_t *data, size_t len, radio_frame_kind_t kind,
                      size_t *offset);

// 解析单个 MP3 帧头。合法时返回该帧字节长度(含 padding),否则返回 0。
// sample_rate_hz / channels 可为 NULL。
size_t radio_frame_mp3_header(const uint8_t *data, size_t len,
                              uint32_t *sample_rate_hz, uint8_t *channels);

// 解析单个 ADTS 帧头,返回帧字节长度;非法返回 0。
size_t radio_frame_adts_header(const uint8_t *data, size_t len,
                               uint32_t *sample_rate_hz, uint8_t *channels);
