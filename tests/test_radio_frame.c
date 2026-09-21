// tests/test_radio_frame.c —— 帧头解析与"流起始点"定位的主机测试。
//
// 这些用例的取值来自真实流:RTHK 六个频道是 MPEG-2 Layer III / 32 kbps / 22050 Hz,
// 而且流的开头落在帧中间;Malaysia 的 Astro 台是 AAC-ADTS。用例里把这两种帧头
// 按位拼出来,验证解析结果与帧长、以及"带垃圾前缀时能否找到真正的起点"。
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "radio_frame.h"

// 从帧头反推帧长所需的字段都在前 4 字节里,够用即可。
static void test_mp3_mpeg2_layer3_32k_22050(void) {
    // 0xFF 0xF3:同步 + MPEG2(10) + Layer III(01) + 无 CRC(1)
    // 0x40 0x40:码率索引 4(V2/L3 = 32kbps) + 采样率索引 0(MPEG2 = 22050Hz)
    const uint8_t header[4] = {0xFF, 0xF3, 0x40, 0x40};
    uint32_t rate = 0;
    uint8_t channels = 0;
    const size_t frame_len = radio_frame_mp3_header(header, sizeof(header), &rate, &channels);
    // 每帧采样数 576:576/8 * 32000 / 22050 = 104(向下取整,无 padding)
    assert(frame_len == 104);
    assert(rate == 22050);
    assert(channels == 2);
}

static void test_mp3_mpeg1_layer3_128k_44100(void) {
    // 0xFB:MPEG1(11) + Layer III + 无 CRC;0x90:码率索引 9(128kbps) + 采样率 0(44100)
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0x00};
    uint32_t rate = 0;
    uint8_t channels = 0;
    const size_t frame_len = radio_frame_mp3_header(header, sizeof(header), &rate, &channels);
    // 144 * 128000 / 44100 = 417
    assert(frame_len == 417);
    assert(rate == 44100);
    // 通道模式 00 = 立体声
    assert(channels == 2);
}

static void test_mp3_padding_is_counted(void) {
    // 同样的头但 padding 位为 1,帧长应加 1。
    const uint8_t plain[4] = {0xFF, 0xFB, 0x90, 0x00};
    const uint8_t padded[4] = {0xFF, 0xFB, 0x92, 0x00};
    assert(radio_frame_mp3_header(padded, 4, NULL, NULL) ==
           radio_frame_mp3_header(plain, 4, NULL, NULL) + 1);
}

static void test_mp3_mono_flag(void) {
    // 通道模式 11 = 单声道
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0xC0};
    uint8_t channels = 0;
    assert(radio_frame_mp3_header(header, 4, NULL, &channels) == 417);
    assert(channels == 1);
}

static void test_mp3_rejects_invalid_headers(void) {
    // 同步字不对
    const uint8_t bad_sync[4] = {0xFE, 0xFB, 0x90, 0x00};
    assert(radio_frame_mp3_header(bad_sync, 4, NULL, NULL) == 0);

    // layer = 00 是保留值
    const uint8_t bad_layer[4] = {0xFF, 0xF9, 0x90, 0x00};
    assert(radio_frame_mp3_header(bad_layer, 4, NULL, NULL) == 0);

    // 采样率索引 3 是保留值
    const uint8_t bad_rate[4] = {0xFF, 0xFB, 0x9C, 0x00};
    assert(radio_frame_mp3_header(bad_rate, 4, NULL, NULL) == 0);

    // 码率索引 15 非法
    const uint8_t bad_bitrate[4] = {0xFF, 0xFB, 0xF0, 0x00};
    assert(radio_frame_mp3_header(bad_bitrate, 4, NULL, NULL) == 0);

    // free-format(码率索引 0)无法推算帧长,不能用于同步确认
    const uint8_t free_format[4] = {0xFF, 0xFB, 0x00, 0x00};
    assert(radio_frame_mp3_header(free_format, 4, NULL, NULL) == 0);

    // 数据不足
    const uint8_t two_bytes[2] = {0xFF, 0xFB};
    assert(radio_frame_mp3_header(two_bytes, 2, NULL, NULL) == 0);
    assert(radio_frame_mp3_header(NULL, 4, NULL, NULL) == 0);
}

static void test_adts_header_22050_stereo(void) {
    // 实测 Mix FM/Lite FM 的帧头前缀:ff f9 5c 80 ...
    // 采样率索引 7 -> 22050Hz;声道配置 2 -> 立体声
    uint8_t header[7] = {0xFF, 0xF9, 0x5C, 0x80, 0x40, 0x00, 0x00};
    uint32_t rate = 0;
    uint8_t channels = 0;
    const size_t frame_len = radio_frame_adts_header(header, sizeof(header), &rate, &channels);
    // 帧长字段 =(b3 & 3) << 11 | b4 << 3 | b5 >> 5 = 0x40 << 3 = 512
    assert(frame_len == 512);
    assert(rate == 22050);
    assert(channels == 2);
}

static void test_adts_rejects_invalid(void) {
    // layer 位不为 00
    uint8_t bad_layer[7] = {0xFF, 0xF5, 0x5C, 0x80, 0x40, 0x00, 0x00};
    assert(radio_frame_adts_header(bad_layer, 7, NULL, NULL) == 0);

    // 采样率索引 13 是保留值
    uint8_t bad_rate[7] = {0xFF, 0xF9, 0x5C, 0x80, 0x40, 0x00, 0x00};
    bad_rate[2] = 0x5C | 0x34;   // rate_index = 13
    assert(radio_frame_adts_header(bad_rate, 7, NULL, NULL) == 0);

    // 帧长字段小于头部本身
    uint8_t short_frame[7] = {0xFF, 0xF9, 0x5C, 0x80, 0x00, 0x00, 0x00};
    assert(radio_frame_adts_header(short_frame, 7, NULL, NULL) == 0);

    // 数据不足
    assert(radio_frame_adts_header(short_frame, 4, NULL, NULL) == 0);
}

// 按帧长铺 n 帧同样的 MP3 头,便于验证"连续两帧"确认逻辑。
static size_t build_mp3_stream(uint8_t *out, size_t cap, size_t frames, size_t prefix) {
    const uint8_t header[4] = {0xFF, 0xF3, 0x40, 0x40};
    const size_t frame_len = radio_frame_mp3_header(header, 4, NULL, NULL);
    assert(frame_len > 0);
    const size_t need = prefix + frame_len * frames;
    assert(need <= cap);
    for (size_t i = 0; i < prefix; i++) out[i] = (uint8_t)(0x30 + i);
    for (size_t f = 0; f < frames; f++) {
        uint8_t *p = out + prefix + f * frame_len;
        memset(p, 0x00, frame_len);
        memcpy(p, header, sizeof(header));
    }
    return need;
}

static void test_find_mp3_after_garbage_prefix(void) {
    uint8_t stream[1024];
    const size_t len = build_mp3_stream(stream, sizeof(stream), 4, 12);

    size_t offset = 999;
    assert(radio_frame_find(stream, len, RADIO_FRAME_KIND_MP3, &offset));
    // 12 字节的垃圾前缀必须被跳过,真实起点在第 12 字节。
    assert(offset == 12);
}

static void test_find_mp3_at_zero(void) {
    uint8_t stream[512];
    const size_t len = build_mp3_stream(stream, sizeof(stream), 3, 0);
    size_t offset = 999;
    assert(radio_frame_find(stream, len, RADIO_FRAME_KIND_MP3, &offset));
    assert(offset == 0);
}

static void test_find_mp3_rejects_lone_sync_byte(void) {
    // 只有同步字、后续字段非法:不能当成流起点。
    uint8_t data[64];
    memset(data, 0x00, sizeof(data));
    data[0] = 0xFF;
    data[1] = 0xFF;
    data[2] = 0xFF;
    data[3] = 0xFF;
    size_t offset = 999;
    assert(!radio_frame_find(data, sizeof(data), RADIO_FRAME_KIND_MP3, &offset));
}

static void test_find_mp3_rejects_single_frame(void) {
    // 一个合法帧后面没有第二帧:不足以确认,应返回 false 让调用方继续读数据。
    uint8_t stream[256];
    const size_t len = build_mp3_stream(stream, sizeof(stream), 1, 8);
    size_t offset = 999;
    assert(!radio_frame_find(stream, len, RADIO_FRAME_KIND_MP3, &offset));
}

static void test_find_adts_after_garbage_prefix(void) {
    // 铺 3 帧 ADTS,帧长 512
    uint8_t stream[2048];
    memset(stream, 0x11, sizeof(stream));
    const size_t prefix = 23;
    for (int f = 0; f < 3; f++) {
        uint8_t *p = stream + prefix + (size_t)f * 512;
        const uint8_t header[7] = {0xFF, 0xF9, 0x5C, 0x80, 0x40, 0x00, 0x00};
        memcpy(p, header, sizeof(header));
    }
    size_t offset = 999;
    assert(radio_frame_find(stream, prefix + 3 * 512, RADIO_FRAME_KIND_AAC_ADTS, &offset));
    assert(offset == prefix);
}

static void test_find_rejects_garbage_then_finds_later_frame(void) {
    // 前面放一个"看起来像但第二帧对不上"的假头,真的起点在后面:必须跳过假的。
    uint8_t stream[2048];
    memset(stream, 0x55, sizeof(stream));
    const uint8_t fake[4] = {0xFF, 0xF3, 0x40, 0x40};
    memcpy(stream, fake, 4);            // 假头:其后 104 字节处不是合法帧头
    const size_t real_len = build_mp3_stream(stream + 200, sizeof(stream) - 200, 3, 0);
    (void)real_len;

    size_t offset = 999;
    assert(radio_frame_find(stream, sizeof(stream), RADIO_FRAME_KIND_MP3, &offset));
    assert(offset == 200);
}

static void test_find_null_safety(void) {
    size_t offset = 0;
    assert(!radio_frame_find(NULL, 100, RADIO_FRAME_KIND_MP3, &offset));
    uint8_t data[64] = {0};
    assert(!radio_frame_find(data, sizeof(data), RADIO_FRAME_KIND_MP3, NULL));
    assert(!radio_frame_find(data, 0, RADIO_FRAME_KIND_MP3, &offset));
}

int main(void) {
    test_mp3_mpeg2_layer3_32k_22050();
    test_mp3_mpeg1_layer3_128k_44100();
    test_mp3_padding_is_counted();
    test_mp3_mono_flag();
    test_mp3_rejects_invalid_headers();
    test_adts_header_22050_stereo();
    test_adts_rejects_invalid();
    test_find_mp3_after_garbage_prefix();
    test_find_mp3_at_zero();
    test_find_mp3_rejects_lone_sync_byte();
    test_find_mp3_rejects_single_frame();
    test_find_adts_after_garbage_prefix();
    test_find_rejects_garbage_then_finds_later_frame();
    test_find_null_safety();
    return 0;
}
