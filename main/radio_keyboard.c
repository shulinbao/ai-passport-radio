// main/radio_keyboard.c —— 软键盘的 LVGL 绘制与输入处理。
//
// 真机上踩过的坑都记在这里,后续改动别再踩回去:
//
// 1) 对象/样式数量。板子没有 PSRAM,LVGL 的对象与样式全从 CONFIG_LV_MEM_SIZE
//    那块静态池里分配。早先每个按键用一个容器 + 一个标签(108 个对象),
//    弹出键盘时把池子吃空 → 白屏重启。现在每个按键只用【一个标签】。
//
// 2) 【样式必须共享】。曾经用 lv_obj_set_style_xxx() 逐对象设 8 个属性,
//    54 个按键就是 400+ 份本地样式属性,实测打开键盘后 LVGL 池用量从 19,340
//    涨到 37,244 字节,最大空闲块只剩 4,856,碎片 31%,随后一次分配失败直接
//    Load access fault(A0=0)+ 栈写坏 → 用户看到的白屏。现在按键只用两个共享
//    样式(常态 + 选中),选中态靠 add/remove 一层样式切换。
//
// 3) 每次按键不要重画所有键。只重画"失去选中"和"获得选中"的两个键。
//
// 4) 提交回调拿到的字符串必须【先复制】。回调会关闭键盘,而关闭会 memset 本
//    结构体 —— 直接把 s_kb.text 交给回调,回调里正要读密码时那块内存已经被清零。
#include "radio_keyboard.h"

#include "radio_fonts.h"
#include "radio_keyboard_layout.h"
#include "radio_styles.h"
#include "radio_theme.h"

#include "esp_log.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "radio_kb";

#define KB_MAX_KEYS 64

#define KB_PAD 6
#define KB_GAP 2
#define KB_KEY_W 36
#define KB_KEY_H 21
#define KB_ROW_H (KB_KEY_H + KB_GAP)
#define KB_ROWS 9
#define KB_HINT_H 16
#define KB_FIELD_H 22

struct radio_keyboard {
    lv_obj_t *panel;
    lv_obj_t *field;
    lv_obj_t *keys[KB_MAX_KEYS];
    uint64_t selected_mask;   // 哪些键当前叠着"选中"样式,避免重复 add/remove
    int index;
    bool shifted;
    bool active;
    char text[RADIO_KB_TEXT_MAX];
    radio_kb_done_cb_t on_done;
    radio_kb_cancel_cb_t on_cancel;
    void *user;
};

// 同一时刻只允许一个键盘。用静态实例避免在这块没有 PSRAM 的板子上再引入一次
// 堆分配与随之而来的碎片。
static struct radio_keyboard s_kb;
static bool s_kb_in_use;

// LVGL 内存池是有上限的静态区。把用量打出来,才能用证据判断"是不是池子不够",
// 而不是靠猜 —— 分配失败在默认配置下是静默的,只会表现成白屏。
static void log_pool(const char *stage) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: LVGL池 已用 %u/%u 字节, 最大空闲块 %u, 碎片 %u%%",
             stage, (unsigned)(mon.total_size - mon.free_size),
             (unsigned)mon.total_size, (unsigned)mon.free_biggest_size,
             (unsigned)mon.frag_pct);
}

static void field_refresh(void) {
    if (s_kb.field == NULL) return;
    // 空串时显示一个占位符,否则用户看不出输入框在哪。
    if (s_kb.text[0] == '\0') {
        lv_label_set_text(s_kb.field, "-");
        lv_obj_set_style_text_color(s_kb.field, lv_color_hex(RADIO_COLOR_MUTED), 0);
    } else {
        lv_label_set_text(s_kb.field, s_kb.text);
        lv_obj_set_style_text_color(s_kb.field, lv_color_hex(RADIO_COLOR_TEXT), 0);
    }
}

// 叠/去"选中"样式。用掩码记录状态,避免对同一个键反复 add 同一样式。
static void key_apply_selection(int i) {
    if (i < 0 || i >= KB_MAX_KEYS) return;
    lv_obj_t *key = s_kb.keys[i];
    if (key == NULL) return;

    const bool want = (i == s_kb.index);
    const bool has = ((s_kb.selected_mask >> (unsigned)i) & 1u) != 0;
    if (want == has) return;

    if (want) {
        lv_obj_add_style(key, radio_style_key_selected(), 0);
        s_kb.selected_mask |= (UINT64_C(1) << (unsigned)i);
    } else {
        lv_obj_remove_style(key, radio_style_key_selected(), 0);
        s_kb.selected_mask &= ~(UINT64_C(1) << (unsigned)i);
    }
}

// 只更新一个键的字面(大小写切换时才需要)。
static void key_refresh_label(int i) {
    lv_obj_t *key = s_kb.keys[i];
    if (key == NULL) return;
    char label[8];
    radio_kb_layout_label(i, s_kb.shifted, label, sizeof(label));
    lv_label_set_text(key, label);
}

// 选中项变化:只动两个键。
static void select_index(int next) {
    if (next == s_kb.index) return;
    const int previous = s_kb.index;
    s_kb.index = next;
    key_apply_selection(previous);
    key_apply_selection(next);
}

// 大小写切换会改变全部字母键的字面,这时才需要整盘刷新。
static void keys_refresh_all(void) {
    const int count = radio_kb_layout_count();
    for (int i = 0; i < count && i < KB_MAX_KEYS; i++) {
        key_refresh_label(i);
        key_apply_selection(i);
    }
}

static void insert_char(char ch) {
    const size_t len = strlen(s_kb.text);
    if (len + 1 >= RADIO_KB_TEXT_MAX) return;   // 满了就不动,不截断已有内容
    s_kb.text[len] = ch;
    s_kb.text[len + 1] = '\0';
    field_refresh();
}

static void backspace(void) {
    const size_t len = strlen(s_kb.text);
    if (len == 0) return;
    // 只按字节退格:键盘只产生 ASCII,不存在把多字节字符切成半个的问题。
    s_kb.text[len - 1] = '\0';
    field_refresh();
}

static void activate(int index) {
    char label[8];
    radio_kb_layout_label(index, s_kb.shifted, label, sizeof(label));
    switch (radio_kb_layout_kind(index)) {
    case RADIO_KB_GLYPH:
        if (label[0] != '\0') insert_char(label[0]);
        break;
    case RADIO_KB_SPACE:
        insert_char(' ');
        break;
    case RADIO_KB_BACKSPACE:
        backspace();
        break;
    case RADIO_KB_CLEAR:
        s_kb.text[0] = '\0';
        field_refresh();
        break;
    case RADIO_KB_SHIFT:
        s_kb.shifted = !s_kb.shifted;
        keys_refresh_all();
        break;
    case RADIO_KB_SUBMIT: {
        // 先做栈上快照:回调会关闭键盘,关闭会清零 s_kb.text。
        char snapshot[RADIO_KB_TEXT_MAX];
        memcpy(snapshot, s_kb.text, sizeof(snapshot));
        radio_kb_done_cb_t cb = s_kb.on_done;
        void *user = s_kb.user;
        if (cb != NULL) cb(snapshot, user);
        break;
    }
    case RADIO_KB_CANCEL: {
        radio_kb_cancel_cb_t cb = s_kb.on_cancel;
        void *user = s_kb.user;
        if (cb != NULL) cb(user);
        break;
    }
    default:
        break;
    }
}

radio_keyboard_t *radio_keyboard_create(void *parent, const char *title,
                                        const char *subtitle,
                                        const char *initial,
                                        radio_kb_done_cb_t on_done,
                                        radio_kb_cancel_cb_t on_cancel,
                                        void *user) {
    if (parent == NULL || s_kb_in_use) return NULL;

    memset(&s_kb, 0, sizeof(s_kb));
    s_kb.on_done = on_done;
    s_kb.on_cancel = on_cancel;
    s_kb.user = user;
    s_kb.active = true;
    if (initial != NULL) {
        strncpy(s_kb.text, initial, sizeof(s_kb.text) - 1);
    }
    log_pool("打开前");

    s_kb.panel = lv_obj_create((lv_obj_t *)parent);
    lv_obj_remove_style_all(s_kb.panel);
    lv_obj_set_size(s_kb.panel, 240, 320);
    lv_obj_set_pos(s_kb.panel, 0, 0);
    lv_obj_set_style_bg_color(s_kb.panel, lv_color_hex(RADIO_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_kb.panel, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_kb.panel, false);

    lv_obj_t *title_label = lv_label_create(s_kb.panel);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(RADIO_COLOR_ACCENT), 0);
    lv_obj_set_size(title_label, 228, 22);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_obj_set_pos(title_label, KB_PAD, 2);

    // SSID 是动态文本,可能含中文。按内容选字体,并按该字体【真实行高】给高度 ——
    // 用字号(16/20)当高度是错的:中文字体 16px 的行高是 31,写小了就会溢出。
    int y = 26;
    if (subtitle != NULL && subtitle[0] != '\0') {
        const lv_font_t *sub_font = radio_font_for_text(subtitle, &lv_font_montserrat_14);
        const int sub_h = lv_font_get_line_height(sub_font);
        lv_obj_t *sub = lv_label_create(s_kb.panel);
        lv_obj_set_style_text_font(sub, sub_font, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(RADIO_COLOR_MUTED), 0);
        lv_obj_set_size(sub, 228, sub_h);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_label_set_text(sub, subtitle);
        lv_obj_set_pos(sub, KB_PAD, y);
        y += sub_h + 2;
    }

    // 密码明文显示:三颗按键敲几十个字符本来就慢,再用圆点遮住会让用户无法
    // 核对输错在哪一位。键盘只产生 ASCII,所以这里用西文字体。
    s_kb.field = lv_label_create(s_kb.panel);
    lv_obj_set_style_text_font(s_kb.field, &lv_font_montserrat_20, 0);
    lv_obj_set_size(s_kb.field, 228, KB_FIELD_H);
    lv_label_set_long_mode(s_kb.field, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_kb.field, KB_PAD, y);
    y += KB_FIELD_H + 4;

    const int count = radio_kb_layout_count();
    const int columns = radio_kb_layout_columns();
    const int grid_top = y;
    for (int i = 0; i < count && i < KB_MAX_KEYS; i++) {
        lv_obj_t *key = lv_label_create(s_kb.panel);
        // 字体是每个键唯一的"本地"属性;底色/圆角/对齐/内边距全部来自共享样式。
        lv_obj_set_style_text_font(key, &lv_font_montserrat_14, 0);
        lv_obj_set_size(key, KB_KEY_W, KB_KEY_H);
        lv_obj_set_pos(key, KB_PAD + (i % columns) * (KB_KEY_W + KB_GAP),
                       grid_top + (i / columns) * KB_ROW_H);
        lv_obj_add_style(key, radio_style_key(), 0);
        s_kb.keys[i] = key;
    }

    // 提示行:单行、限高,放在网格下方且不越界。
    lv_obj_t *hint = lv_label_create(s_kb.panel);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(RADIO_COLOR_MUTED), 0);
    lv_obj_set_size(hint, 228, KB_HINT_H);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(hint, "Hold=row  OK=type  2xOK=done");
    lv_obj_set_pos(hint, KB_PAD, grid_top + KB_ROWS * KB_ROW_H + 2);

    s_kb_in_use = true;
    field_refresh();
    keys_refresh_all();
    log_pool("打开后");
    return &s_kb;
}

void radio_keyboard_destroy(radio_keyboard_t *kb) {
    if (kb == NULL || kb != &s_kb) return;
    if (s_kb.panel != NULL) {
        lv_obj_delete(s_kb.panel);
    }
    memset(&s_kb, 0, sizeof(s_kb));
    s_kb_in_use = false;
}

bool radio_keyboard_key(radio_keyboard_t *kb, radio_kb_key_t key) {
    if (kb == NULL || kb != &s_kb || !s_kb.active) return false;

    switch (key) {
    case RADIO_KB_KEY_UP:
        select_index(radio_kb_layout_move(s_kb.index, -1));
        break;
    case RADIO_KB_KEY_DOWN:
        select_index(radio_kb_layout_move(s_kb.index, 1));
        break;
    case RADIO_KB_KEY_UP_LONG:
        select_index(radio_kb_layout_row_move(s_kb.index, -1));
        break;
    case RADIO_KB_KEY_DOWN_LONG:
        select_index(radio_kb_layout_row_move(s_kb.index, 1));
        break;
    case RADIO_KB_KEY_OK:
        activate(s_kb.index);
        break;
    case RADIO_KB_KEY_OK_LONG: {
        // 长按确定 = 取消。按功能类型查找,不依赖"最后一格是什么"的隐含约定。
        const int esc = radio_kb_layout_find(RADIO_KB_CANCEL);
        if (esc >= 0) activate(esc);
        break;
    }
    case RADIO_KB_KEY_OK_DOUBLE: {
        // 双击确定 = 提交。这里曾经写成"最后一格",而最后一格其实是 ESC,
        // 于是双击确定变成了取消键盘。
        const int submit = radio_kb_layout_find(RADIO_KB_SUBMIT);
        if (submit >= 0) activate(submit);
        break;
    }
    default:
        return false;
    }
    // 提交/取消的回调会把键盘销毁(s_kb 被清零),之后不能再触碰任何对象。
    return true;
}

const char *radio_keyboard_text(const radio_keyboard_t *kb) {
    if (kb == NULL || kb != &s_kb) return "";
    return s_kb.text;
}

bool radio_keyboard_shifted(const radio_keyboard_t *kb) {
    if (kb == NULL || kb != &s_kb) return false;
    return s_kb.shifted;
}

int radio_keyboard_key_count(void) {
    return radio_kb_layout_count();
}

const char *radio_keyboard_key_label(int index, bool shifted) {
    static char label[8];
    radio_kb_layout_label(index, shifted, label, sizeof(label));
    return label;
}
