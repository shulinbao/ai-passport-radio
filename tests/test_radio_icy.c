// tests/test_radio_icy.c —— ICY 元数据解复用器的纯逻辑主机测试。
//
// 这些用例覆盖真实网络电台的三种常见形态:标准 StreamTitle、空块(服务器本轮没有
// 新标题)、以及超长/畸形块。解复用一旦错位,后果是"音频里混进元数据"这种很难在
// 设备上定位的故障,所以这里把字节边界逐个钉死。
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "radio_icy.h"

// 逐字节喂给解复用器并统计各类型字节数。
static void feed(radio_icy_t *icy, const uint8_t *data, size_t len,
                 size_t *audio_bytes, size_t *meta_bytes) {
    for (size_t i = 0; i < len; i++) {
        if (radio_icy_consume(icy, data[i]) == RADIO_ICY_AUDIO) {
            (*audio_bytes)++;
        } else {
            (*meta_bytes)++;
        }
    }
}

// 构造一个元数据块:1 个长度字节 + 向上取整到 16 的块体。返回写入的字节数。
static size_t make_block(uint8_t *out, const char *text) {
    const size_t need = strlen(text) + 1;          // 含结尾 NUL
    size_t units = (need + 15) / 16;
    if (units == 0) units = 1;
    assert(units <= 255);
    out[0] = (uint8_t)units;
    const size_t body = units * 16;
    memset(out + 1, 0, body);
    memcpy(out + 1, text, strlen(text));
    return body + 1;
}

// 以完整字符串长度调用解析器。
static bool parse(const char *block, char *out, size_t out_cap) {
    return radio_icy_parse_block(block, strlen(block), out, out_cap);
}

static void test_metaint_zero_passes_everything_through(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 0);
    const uint8_t data[] = {0x00, 0xFF, 0x10, 'S', 't'};
    size_t audio = 0, meta = 0;
    feed(&icy, data, sizeof(data), &audio, &meta);
    assert(audio == sizeof(data));
    assert(meta == 0);
    assert(strcmp(radio_icy_title(&icy), "") == 0);
}

static void test_audio_and_metadata_are_split_at_the_declared_boundary(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 4);

    uint8_t block[64];
    const size_t block_len = make_block(block, "StreamTitle='Now Playing';");

    uint8_t stream[4 + 64];
    for (size_t i = 0; i < 4; i++) stream[i] = (uint8_t)(0xA0 + i);
    memcpy(stream + 4, block, block_len);

    size_t audio = 0, meta = 0;
    feed(&icy, stream, 4 + block_len, &audio, &meta);

    // 恰好 4 个音频字节;长度字节和整个块都算元数据。
    assert(audio == 4);
    assert(meta == block_len);
    assert(strcmp(radio_icy_title(&icy), "Now Playing") == 0);
}

static void test_two_consecutive_blocks_and_boundary_repeat(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 2);

    uint8_t b1[64], b2[64];
    const size_t l1 = make_block(b1, "StreamTitle='First';");
    const size_t l2 = make_block(b2, "StreamTitle='Second';");

    size_t audio = 0, meta = 0;
    const uint8_t a[2] = {1, 2};
    feed(&icy, a, 2, &audio, &meta);
    feed(&icy, b1, l1, &audio, &meta);
    assert(strcmp(radio_icy_title(&icy), "First") == 0);

    feed(&icy, a, 2, &audio, &meta);
    feed(&icy, b2, l2, &audio, &meta);
    assert(strcmp(radio_icy_title(&icy), "Second") == 0);

    // 音频计数在块后必须复位,否则第二段 2 字节会被误判成元数据。
    assert(audio == 4);
    assert(meta == l1 + l2);
}

static void test_empty_block_keeps_the_previous_title(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 2);

    uint8_t b1[64];
    const size_t l1 = make_block(b1, "StreamTitle='Keep me';");
    size_t audio = 0, meta = 0;
    const uint8_t a[2] = {7, 8};
    feed(&icy, a, 2, &audio, &meta);
    feed(&icy, b1, l1, &audio, &meta);
    assert(strcmp(radio_icy_title(&icy), "Keep me") == 0);

    const uint32_t seq_before = icy.title_seq;
    // 长度字节为 0:服务器表示本轮无新标题。
    const uint8_t empty[3] = {9, 10, 0x00};
    feed(&icy, empty, 3, &audio, &meta);
    assert(strcmp(radio_icy_title(&icy), "Keep me") == 0);
    assert(icy.title_seq == seq_before);
    // 前两个字节仍是音频,只有长度字节算元数据。
    assert(audio == 4);
    assert(meta == l1 + 1);
}

static void test_title_sequence_only_advances_on_change(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 1);

    uint8_t b1[64], b2[64];
    const size_t l1 = make_block(b1, "StreamTitle='Same';");
    const size_t l2 = make_block(b2, "StreamTitle='Same';");
    const uint8_t a = 0x55;
    size_t audio = 0, meta = 0;

    feed(&icy, &a, 1, &audio, &meta);
    feed(&icy, b1, l1, &audio, &meta);
    const uint32_t after_first = icy.title_seq;
    assert(after_first == 1);

    feed(&icy, &a, 1, &audio, &meta);
    feed(&icy, b2, l2, &audio, &meta);
    // 内容没变就不该通知界面重绘。
    assert(icy.title_seq == after_first);
}

static void test_parse_block_accepts_common_quote_styles(void) {
    char out[RADIO_ICY_TITLE_CAP];

    assert(parse("StreamTitle='single';", out, sizeof(out)));
    assert(strcmp(out, "single") == 0);

    assert(parse("StreamTitle=\"double\";", out, sizeof(out)));
    assert(strcmp(out, "double") == 0);

    // 有些服务器不加引号,靠分号结束。
    assert(parse("StreamTitle=bare;StreamUrl='x';", out, sizeof(out)));
    assert(strcmp(out, "bare") == 0);

    // 键名大小写不统一。
    assert(parse("streamtitle='lower case key';", out, sizeof(out)));
    assert(strcmp(out, "lower case key") == 0);

    // 键名前还有别的字段。
    assert(parse("StreamUrl='u';StreamTitle='after';", out, sizeof(out)));
    assert(strcmp(out, "after") == 0);

    // 无结尾分号也要能取到值。
    assert(parse("StreamTitle='no terminator'", out, sizeof(out)));
    assert(strcmp(out, "no terminator") == 0);

    // 值里带空格和引号以外字符。
    assert(parse("StreamTitle='A: B - C (D)';", out, sizeof(out)));
    assert(strcmp(out, "A: B - C (D)") == 0);
}

static void test_parse_block_rejects_malformed_input(void) {
    char out[RADIO_ICY_TITLE_CAP];

    assert(!radio_icy_parse_block(NULL, 10, out, sizeof(out)));
    assert(!radio_icy_parse_block("StreamTitle='x';", 0, out, sizeof(out)));
    // 全 NUL 的填充块(服务器有时会发)。
    assert(!radio_icy_parse_block("\0\0\0\0", 4, out, sizeof(out)));
    // 空标题视为"没有信息",不要用空串覆盖已有台名。
    assert(!parse("StreamTitle='';", out, sizeof(out)));
    // 没有 StreamTitle 字段。
    assert(!parse("StreamUrl='http://x';", out, sizeof(out)));
    // 带键名但值是空的 NUL 块。
    assert(!radio_icy_parse_block("StreamTitle=", 12, out, sizeof(out)));
    // out 为空指针必须安全返回。
    assert(!radio_icy_parse_block("StreamTitle='x';", 16, NULL, 0));
    // out 有指针但容量为 0。
    assert(!radio_icy_parse_block("StreamTitle='x';", 16, out, 0));
}

static void test_parse_block_honours_partial_lengths(void) {
    char out[RADIO_ICY_TITLE_CAP];
    // 只给到引号之前:值不完整,仍应解出已到达的部分。
    assert(radio_icy_parse_block("StreamTitle='abc", 16, out, sizeof(out)));
    assert(strcmp(out, "abc") == 0);
    // 键名被从中间截断,不应误判。
    assert(!radio_icy_parse_block("StreamTi", 8, out, sizeof(out)));
    assert(!radio_icy_parse_block("StreamTitle", 11, out, sizeof(out)));
}

static void test_parsed_title_is_truncated_to_the_output_buffer(void) {
    char small[8];
    assert(parse("StreamTitle='abcdefghijklmnop';", small, sizeof(small)));
    assert(strcmp(small, "abcdefg") == 0);  // 7 字节 + NUL
}

static void test_utf8_title_survives_byte_wise_feed(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 1);

    // "电台" 的 UTF-8 字节,避免测试源码编码影响用例。
    static const char kExpected[] = "\xE7\x94\xB5\xE5\x8F\xB0 FM 99.7";
    uint8_t block[96];
    const size_t block_len = make_block(block, "StreamTitle='" "\xE7\x94\xB5\xE5\x8F\xB0 FM 99.7" "';");

    size_t audio = 0, meta = 0;
    const uint8_t a = 0x33;
    feed(&icy, &a, 1, &audio, &meta);
    feed(&icy, block, block_len, &audio, &meta);

    // 多字节字符必须逐字节原样保留,不能被截断成半个字符。
    assert(strcmp(radio_icy_title(&icy), kExpected) == 0);
    assert(strlen(radio_icy_title(&icy)) == strlen(kExpected));
}

static void test_oversized_block_is_discarded_without_desync(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 2);

    // 声明 255*16 = 4080 字节,远超 RADIO_ICY_BLOCK_CAP(1024)。
    const size_t declared = 255 * 16;
    uint8_t *big = (uint8_t *)malloc(declared + 1);
    assert(big != NULL);
    big[0] = 255;
    memset(big + 1, 'A', declared);
    // 即使块头就是合法键,超长块的整体内容也不可信,不应据此更新标题。
    memcpy(big + 1, "StreamTitle='too long';", 22);

    size_t audio = 0, meta = 0;
    const uint8_t a[2] = {1, 1};
    feed(&icy, a, 2, &audio, &meta);
    feed(&icy, big, declared + 1, &audio, &meta);

    assert(icy.truncated);
    assert(strcmp(radio_icy_title(&icy), "") == 0);
    assert(audio == 2);
    assert(meta == declared + 1);

    // 关键:块结束后计数必须复位,后续字节仍被当作音频,元数据不会永久错位。
    const uint8_t more[2] = {2, 2};
    const size_t audio_before = audio;
    feed(&icy, more, 2, &audio, &meta);
    assert(audio == audio_before + 2);

    // 恢复后仍能正常解析新标题。此刻音频计数正好用满,下一个字节就是长度字节。
    uint8_t good[64];
    const size_t good_len = make_block(good, "StreamTitle='Recovered';");
    feed(&icy, good, good_len, &audio, &meta);
    assert(strcmp(radio_icy_title(&icy), "Recovered") == 0);

    free(big);
}

static void test_metaint_boundary_exact_multiple(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 1);
    // metaint=1 时,音频/元数据交替,是最容易差一位的边界。
    const uint8_t pattern[] = {0xAA, 0x01, 'S', 0xBB, 0x00};
    size_t audio = 0, meta = 0;
    feed(&icy, pattern, sizeof(pattern), &audio, &meta);
    // 0xAA 音频,0x01=长度字节(16 字节块),后面 16 字节是块,然后再一个音频字节。
    assert(audio == 1);
}

static void test_default_instance_reports_empty_title(void) {
    radio_icy_t icy;
    radio_icy_init(&icy, 16);
    assert(strcmp(radio_icy_title(&icy), "") == 0);
    assert(strcmp(radio_icy_title(NULL), "") == 0);
    // 消费字节前也要是安全的。
    assert(radio_icy_consume(NULL, 0) == RADIO_ICY_AUDIO);
    // NULL 初始化不应崩溃。
    radio_icy_init(NULL, 32);
}

int main(void) {
    test_metaint_zero_passes_everything_through();
    test_audio_and_metadata_are_split_at_the_declared_boundary();
    test_two_consecutive_blocks_and_boundary_repeat();
    test_empty_block_keeps_the_previous_title();
    test_title_sequence_only_advances_on_change();
    test_parse_block_accepts_common_quote_styles();
    test_parse_block_rejects_malformed_input();
    test_parse_block_honours_partial_lengths();
    test_parsed_title_is_truncated_to_the_output_buffer();
    test_utf8_title_survives_byte_wise_feed();
    test_oversized_block_is_discarded_without_desync();
    test_metaint_boundary_exact_multiple();
    test_default_instance_reports_empty_title();
    return 0;
}
