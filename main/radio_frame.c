// main/radio_frame.c —— 帧头解析与定位实现。纯逻辑,无 ESP-IDF 依赖。
#include "radio_frame.h"

// MPEG Audio 帧头字段表。索引即帧头里的 4 位 / 2 位字段值。
// 采样率表按版本分:MPEG1 / MPEG2 / MPEG2.5。
static const uint32_t kSampleRates[3][3] = {
    {44100, 48000, 32000},   // MPEG1
    {22050, 24000, 16000},   // MPEG2
    {11025, 12000, 8000},    // MPEG2.5
};

// Layer III 码率表(kbps)。0 = free,15 = 非法。
static const uint16_t kBitrateV1L3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
};
static const uint16_t kBitrateV2L3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,
};

// ADTS 采样率表(按 sampling_frequency_index)。
static const uint32_t kAdtsSampleRates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

// ADTS 声道配置 -> 声道数。0 表示"由 PCE 决定",按单声道处理。
static uint8_t adts_channels(uint8_t config) {
    if (config == 0) return 1;
    if (config > 7) return 0;
    return config == 7 ? 8 : config;
}

size_t radio_frame_mp3_header(const uint8_t *data, size_t len,
                              uint32_t *sample_rate_hz, uint8_t *channels) {
    if (data == NULL || len < 4) return 0;
    if (data[0] != 0xFF) return 0;
    // 11 位同步 + 后续字段:第二个字节高 3 位必须是 111。
    if ((data[1] & 0xE0) != 0xE0) return 0;

    const uint8_t version_bits = (uint8_t)((data[1] >> 3) & 0x03);
    const uint8_t layer_bits = (uint8_t)((data[1] >> 1) & 0x03);
    // 01 = reserved,必须丢弃。
    if (version_bits == 0x01) return 0;
    if (layer_bits == 0x00) return 0;
    // 本播放器只解 Layer III;Layer I/II 的帧长公式不同,不做支持也不误判。
    if (layer_bits != 0x01) return 0;

    const uint8_t bitrate_index = (uint8_t)((data[2] >> 4) & 0x0F);
    const uint8_t rate_index = (uint8_t)((data[2] >> 2) & 0x03);
    const uint8_t padding = (uint8_t)((data[2] >> 1) & 0x01);
    if (bitrate_index == 0x0F || rate_index == 0x03) return 0;

    // version_bits: 0 = MPEG2.5, 2 = MPEG2, 3 = MPEG1
    const bool is_v1 = (version_bits == 0x03);
    const uint16_t bitrate_kbps = is_v1 ? kBitrateV1L3[bitrate_index]
                                        : kBitrateV2L3[bitrate_index];
    // free-format 流没有可推算的帧长,不能用于同步确认。
    if (bitrate_kbps == 0) return 0;

    const int version_row = is_v1 ? 0 : (version_bits == 0x02 ? 1 : 2);
    const uint32_t rate = kSampleRates[version_row][rate_index];
    if (rate == 0) return 0;

    if (sample_rate_hz != NULL) *sample_rate_hz = rate;
    if (channels != NULL) {
        // 通道模式在 data[3] 高 2 位:11 = 单声道,其余按立体声处理。
        *channels = ((data[3] >> 6) & 0x03) == 0x03 ? 1 : 2;
    }

    // Layer III 每帧采样数:MPEG1 = 1152,MPEG2/2.5 = 576。
    const uint32_t samples_per_frame = is_v1 ? 1152u : 576u;
    const uint32_t frame_len =
        (samples_per_frame / 8u) * bitrate_kbps * 1000u / rate + padding;
    if (frame_len < 4 || frame_len > 8192) return 0;
    return frame_len;
}

size_t radio_frame_adts_header(const uint8_t *data, size_t len,
                               uint32_t *sample_rate_hz, uint8_t *channels) {
    if (data == NULL || len < 7) return 0;
    if (data[0] != 0xFF) return 0;
    // 同步 12 位 + layer 必须为 00:mask 0xF6 覆盖 bit3(layer 高位)与 bit2。
    if ((data[1] & 0xF6) != 0xF0) return 0;

    const uint8_t rate_index = (uint8_t)((data[2] >> 2) & 0x0F);
    if (rate_index >= 13) return 0;
    const uint8_t channel_config =
        (uint8_t)(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));

    const uint32_t frame_len = ((uint32_t)(data[3] & 0x03) << 11) |
                               ((uint32_t)data[4] << 3) |
                               ((uint32_t)data[5] >> 5);
    // ADTS 头部本身 7 字节(无 CRC),帧长必须大于头部且合理。
    if (frame_len < 7 || frame_len > 8192) return 0;

    if (sample_rate_hz != NULL) *sample_rate_hz = kAdtsSampleRates[rate_index];
    if (channels != NULL) *channels = adts_channels(channel_config);
    return frame_len;
}

bool radio_frame_find(const uint8_t *data, size_t len, radio_frame_kind_t kind,
                      size_t *offset) {
    if (data == NULL || offset == NULL) return false;
    if (kind == RADIO_FRAME_KIND_MP3) {
        for (size_t i = 0; i + 4 <= len; i++) {
            if (data[i] != 0xFF) continue;
            const size_t frame_len = radio_frame_mp3_header(data + i, len - i, NULL, NULL);
            if (frame_len == 0) continue;
            // 二次确认:下一帧必须也落在算出来的位置。数据里随机出现 0xFF 的概率
            // 不低,只有连续两帧都对上才认为是真的流起始点。
            const size_t next = i + frame_len;
            // 数据不够确认这一处时继续往后找,而不是直接放弃:任意一个真正的帧
            // 边界对解码都是等价的,后面找到的候选同样可用。
            if (next + 4 > len) continue;
            if (radio_frame_mp3_header(data + next, len - next, NULL, NULL) == 0) continue;
            *offset = i;
            return true;
        }
        return false;
    }

    for (size_t i = 0; i + 7 <= len; i++) {
        if (data[i] != 0xFF) continue;
        const size_t frame_len = radio_frame_adts_header(data + i, len - i, NULL, NULL);
        if (frame_len == 0) continue;
        const size_t next = i + frame_len;
        if (next + 7 > len) continue;
        if (radio_frame_adts_header(data + next, len - next, NULL, NULL) == 0) continue;
        *offset = i;
        return true;
    }
    return false;
}
