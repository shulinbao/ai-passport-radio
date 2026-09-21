// main/radio_icy.c —— ICY 元数据解复用器实现。纯逻辑,可用主机测试覆盖。
#include "radio_icy.h"

#include <string.h>

// 大小写无关的前缀比较。src 只有前 len 个字节有效,不假设其以 NUL 结尾。
static bool prefix_ci(const char *src, size_t len, const char *want) {
    for (size_t i = 0; want[i] != '\0'; i++) {
        if (i >= len) return false;
        char a = src[i];
        char b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool radio_icy_parse_block(const char *block, size_t len, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0) return false;
    out[0] = '\0';
    if (block == NULL || len == 0) return false;

    // 元数据块是 NUL 补齐的定长缓冲,真实内容到第一个 NUL 为止。
    size_t text_len = 0;
    while (text_len < len && block[text_len] != '\0') text_len++;
    if (text_len == 0) return false;

    static const char kKey[] = "StreamTitle=";
    const size_t key_len = sizeof(kKey) - 1;

    for (size_t pos = 0; pos + key_len <= text_len; pos++) {
        if (!prefix_ci(block + pos, text_len - pos, kKey)) continue;

        size_t i = pos + key_len;
        // 值通常被单引号包围,也有服务器用双引号或干脆不加引号。
        char quote = '\0';
        if (i < text_len && (block[i] == '\'' || block[i] == '"')) {
            quote = block[i];
            i++;
        }
        size_t written = 0;
        while (i < text_len && written + 1 < out_cap) {
            const char c = block[i];
            if (quote != '\0' && c == quote) break;
            if (quote == '\0' && c == ';') break;
            out[written++] = c;
            i++;
        }
        out[written] = '\0';
        return written > 0;
    }
    return false;
}

void radio_icy_init(radio_icy_t *icy, size_t metaint) {
    if (icy == NULL) return;
    memset(icy, 0, sizeof(*icy));
    icy->metaint = metaint;
    icy->block[0] = '\0';
}

radio_icy_kind_t radio_icy_consume(radio_icy_t *icy, uint8_t byte) {
    if (icy == NULL || icy->metaint == 0) return RADIO_ICY_AUDIO;

    if (!icy->in_block) {
        if (icy->audio_seen < icy->metaint) {
            icy->audio_seen++;
            return RADIO_ICY_AUDIO;
        }
        // 音频计数用满,这个字节是元数据块的长度字段(单位 16 字节)。
        // 它本身不属于音频,必须吞掉。
        icy->audio_seen = 0;
        icy->block_len = byte;
        icy->block_pos = 0;
        icy->block_fill = 0;
        icy->truncated = false;
        icy->block[0] = '\0';
        if (byte == 0) {
            // 空块 = 这一轮没有新标题,保留上一次的标题。
            return RADIO_ICY_META;
        }
        icy->in_block = true;
        return RADIO_ICY_META;
    }

    // 块内:始终给末尾留 1 字节放 NUL,便于当字符串解析。
    if (icy->block_fill + 1 < RADIO_ICY_BLOCK_CAP) {
        icy->block[icy->block_fill++] = (char)byte;
        icy->block[icy->block_fill] = '\0';
    } else {
        icy->truncated = true;
    }
    icy->block_pos++;

    if ((size_t)icy->block_pos >= (size_t)icy->block_len * 16u) {
        icy->in_block = false;
        // 超长块说明我们对块边界的假设和服务器不一致,里面的内容不可信:
        // 宁可暂时不更新标题,也不要显示半截乱码。
        if (!icy->truncated) {
            char parsed[RADIO_ICY_TITLE_CAP];
            if (radio_icy_parse_block(icy->block, icy->block_fill, parsed, sizeof(parsed))) {
                if (strcmp(parsed, icy->title) != 0) {
                    memcpy(icy->title, parsed, strlen(parsed) + 1);
                    icy->title_seq++;
                }
            }
        }
        icy->block_fill = 0;
        icy->block[0] = '\0';
    }
    return RADIO_ICY_META;
}

const char *radio_icy_title(const radio_icy_t *icy) {
    if (icy == NULL) return "";
    return icy->title;
}
