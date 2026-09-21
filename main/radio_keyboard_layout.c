// main/radio_keyboard_layout.c —— 软键盘布局实现。纯逻辑,无 LVGL 依赖。
#include "radio_keyboard_layout.h"

#include <string.h>

#define KB_COLUMNS 6

// 字符行,每行 KB_COLUMNS 个字符。顺序按"Wi-Fi 密码里最常出现的先放"排:
// 数字在最上面(很多人用纯数字密码),然后是小写字母,最后是需要长按跳行的符号行。
// 小写字母按手写频率重排成 6 列块,而不是照搬 QWERTY,因为这里没有左右键,
// 线性顺序下的"移动距离"比键盘手感更重要。
static const char *const kGlyphRows[] = {
    "123456",
    "7890-_",
    "qwerty",
    "uiopas",
    "dfghjk",
    "lzxcvb",
    "nm.,!@",
    "#$%&*?",
};
#define GLYPH_ROWS ((int)(sizeof(kGlyphRows) / sizeof(kGlyphRows[0])))
#define GLYPH_KEYS (GLYPH_ROWS * KB_COLUMNS)

// 功能键行,同样 KB_COLUMNS 个。
static const radio_kb_glyph_kind_t kFunctionRow[KB_COLUMNS] = {
    RADIO_KB_SPACE, RADIO_KB_BACKSPACE, RADIO_KB_CLEAR,
    RADIO_KB_SHIFT, RADIO_KB_SUBMIT, RADIO_KB_CANCEL,
};
static const char *const kFunctionLabels[KB_COLUMNS] = {
    "SPC", "DEL", "CLR", "Aa", "OK", "ESC",
};

int radio_kb_layout_count(void) {
    return GLYPH_KEYS + KB_COLUMNS;
}

int radio_kb_layout_columns(void) {
    return KB_COLUMNS;
}

radio_kb_glyph_kind_t radio_kb_layout_kind(int index) {
    if (index < 0 || index >= radio_kb_layout_count()) return RADIO_KB_CANCEL;
    if (index < GLYPH_KEYS) return RADIO_KB_GLYPH;
    return kFunctionRow[index - GLYPH_KEYS];
}

int radio_kb_layout_find(radio_kb_glyph_kind_t kind) {
    const int count = radio_kb_layout_count();
    for (int i = 0; i < count; i++) {
        if (radio_kb_layout_kind(i) == kind) return i;
    }
    return -1;
}

void radio_kb_layout_label(int index, bool shifted, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (index < 0 || index >= radio_kb_layout_count()) return;

    if (index < GLYPH_KEYS) {
        const char raw = kGlyphRows[index / KB_COLUMNS][index % KB_COLUMNS];
        char ch = raw;
        // 只对字母做大小写切换,数字和符号没有大小写。
        if (shifted && raw >= 'a' && raw <= 'z') ch = (char)(raw - 'a' + 'A');
        out[0] = ch;
        out[1] = '\0';
        return;
    }
    const char *label = kFunctionLabels[index - GLYPH_KEYS];
    strncpy(out, label, cap - 1);
    out[cap - 1] = '\0';
}

static int clamp_index(int index) {
    const int count = radio_kb_layout_count();
    if (index < 0) return 0;
    if (index >= count) return count - 1;
    return index;
}

int radio_kb_layout_move(int index, int delta) {
    return clamp_index(index + delta);
}

int radio_kb_layout_row_move(int index, int delta) {
    index = clamp_index(index);
    const int rows = radio_kb_layout_count() / KB_COLUMNS;
    const int column = index % KB_COLUMNS;
    int row = index / KB_COLUMNS + delta;
    // 跳行时保持列号;越过首尾行就停在首尾行(而不是跳到别的列,那会让用户
    // 以为自己按错了键)。
    if (row < 0) row = 0;
    if (row > rows - 1) row = rows - 1;
    return row * KB_COLUMNS + column;
}
