// main/radio_fonts.c —— 应用字体初始化与覆盖检查。
#include "radio_fonts.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "radio_fonts";

// 由 tools/gen_fonts.py 调用 lv_font_conv 生成的只读字体数据,直接编译进 main
// 组件(见 main/CMakeLists.txt 的 target_sources),这里只做符号声明。
//
// 只保留 16 px 这一套:界面文案全部是英文,标题用 Montserrat 20 排版,中文只在
// 动态文本(SSID / 曲目名)里以兜底形式出现。早先还生成过一套 20 px 中文字体,
// 界面英文化之后已经没有任何代码会选中它,属于白占 Flash,故删除。
LV_FONT_DECLARE(radio_font_cjk_16);

// 可写副本:只为挂 fallback。原始符号是 const 的,不去强转它。
static lv_font_t s_cjk16;
static bool s_ready;

void radio_fonts_init(void) {
    if (s_ready) return;
    s_cjk16 = radio_font_cjk_16;
    s_cjk16.fallback = &lv_font_montserrat_14;
    s_ready = true;

    // 开机自检几个必然要显示的字,把"字体没编进去/子集漏字"这类问题在启动阶段
    // 就暴露成一条明确的日志,而不是等用户看到方框才发现。
    //
    // 这里必须写【数值】而不是 '\u7535' 这样的字符常量:在 C 里把大于 127 的
    // 字符放进 char 常量,编译器会按执行字符集(UTF-8)编码成多个字节,再按
    // 多字符常量打包成一个整数,于是 '\u7535' 会变成 0xE794B5 而不是 0x7535,
    // 自检就会莫名其妙地报"缺少字形"。真实固件上确实这样错过一次。
    // 同时放一个繁体字(設),用来确认整块基本区(而不仅是简体常用字)都进来了。
    static const uint32_t kProbe[] = {
        0x7535u,   // 电
        0x53F0u,   // 台
        0x8FDEu,   // 连
        0x63A5u,   // 接
        0x8B1Bu,   // 設(繁体)
        0x6A5Fu,   // 樓(繁体)
    };
    for (size_t i = 0; i < sizeof(kProbe) / sizeof(kProbe[0]); i++) {
        if (!radio_font_has_glyph(&s_cjk16, kProbe[i])) {
            ESP_LOGE(TAG, "中文字库缺少字形 U+%04X,界面会显示占位框",
                     (unsigned)kProbe[i]);
        }
    }
}

const lv_font_t *radio_font_body(void) {
    return s_ready ? &s_cjk16 : &radio_font_cjk_16;
}

bool radio_text_needs_cjk(const char *text) {
    if (text == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p >= 0x80) return true;
    }
    return false;
}

const lv_font_t *radio_font_for_text(const char *text, const lv_font_t *latin) {
    if (radio_text_needs_cjk(text)) return radio_font_body();
    return latin != NULL ? latin : &lv_font_montserrat_14;
}

bool radio_font_has_glyph(const lv_font_t *font, uint32_t codepoint) {
    if (font == NULL) return false;
    lv_font_glyph_dsc_t glyph = {0};
    if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0)) return false;
    // 占位框不是"有字形":把缺失当成功会让覆盖检查永远通过。
    return !glyph.is_placeholder;
}

// 从 UTF-8 解出下一个码点,并推进指针。非法字节按"跳过 1 字节"处理。
static uint32_t next_codepoint(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    if (*p == '\0') return 0;

    uint32_t cp = 0;
    int extra = 0;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        (*cursor)++;
        return 0xFFFD;   // 非法起始字节
    }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            // 截断的多字节序列:只前进已消费的字节数,让后续字符仍能被解析。
            *cursor += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cursor += extra + 1;
    return cp;
}

bool radio_font_covers_utf8(const lv_font_t *font, const char *utf8) {
    if (font == NULL || utf8 == NULL) return false;
    const char *cursor = utf8;
    for (;;) {
        const uint32_t cp = next_codepoint(&cursor);
        if (cp == 0) return true;
        // 控制字符不参与判断;换行在标签里是合法且必然存在的。
        if (cp == '\n' || cp == '\r' || cp == '\t') continue;
        if (!radio_font_has_glyph(font, cp)) return false;
    }
}
