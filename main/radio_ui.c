// main/radio_ui.c —— 深色界面渲染实现(界面文案英文,中文作为兜底字体)。
//
// 排版规则(全部来自真机踩坑后重新量出来的数字,不是估的):
//
//   Montserrat 14 行高 16 | Montserrat 20 行高 22
//   中文字体 16 行高 31   | 中文字体 20 行高 38
//
//   * 中文字体的行高远大于字号(Noto Sans SC 的 ascent/descent 本身很宽),
//     所以【绝对不能把字号当行高用】—— 之前把 16 当 17/18 用,文字就顶出了行框。
//   * 所有标签的高度一律取 lv_font_get_line_height(font),并按行框居中摆放,
//     这样无论实际选中哪个字体都不会溢出。
//   * 界面文案是英文,用 Montserrat 排版紧凑;一旦文本里出现非 ASCII(中文 SSID、
//     中文曲目名),radio_font_for_text() 会自动换成带中文字形的字体,并按它更大的
//     行高重新计算位置 —— "支持中文以防万一"因此不会破坏排版。
//
// 纵向分区:头部 0..32 | 内容 34..284 | 底部提示 286..302。
#include "radio_ui.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "radio_fonts.h"
#include "radio_icy.h"
#include "radio_keyboard.h"
#include "radio_player.h"
#include "radio_styles.h"
#include "radio_theme.h"
#include "radio_wifi.h"

#include "lvgl.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "radio_ui";

#define UI_W 240
#define UI_H 320
#define UI_PAD 6
#define UI_ROW_W 228
#define UI_ROW_BOX_H 47
#define UI_ROW_STEP 49
// 设置行:单行文字,行框可以矮一些,好让 6 个设置项一屏放完(6*41 = 246 <= 252)。
#define UI_SET_BOX_H 39
#define UI_SET_STEP 41
#define UI_ROWS_TOP 34
#define UI_HINT_TOP 286

// 头部:标题用 20 px,右侧状态/电量用 14 px。三个标签都做成同样高度的盒子并把
// 文字在盒内居中,中心线统一在 y=16 —— 这是"左上角 Wi-Fi 和顶栏不齐"的修法。
#define UI_HEADER_H 32
#define UI_HEADER_CENTER 16

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_net;
static lv_obj_t *s_batt;

static lv_obj_t *s_list_cont;
static struct {
    lv_obj_t *box;
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *badge;
    bool selected;
} s_rows[RADIO_UI_MAX_ROWS];

static lv_obj_t *s_set_cont;
static struct {
    lv_obj_t *box;
    lv_obj_t *title;
    lv_obj_t *value;
    bool selected;
    bool adjusting;
} s_set[RADIO_UI_SET_ROWS];

static lv_obj_t *s_now_cont;
static lv_obj_t *s_now_name;
static lv_obj_t *s_now_meta;
static lv_obj_t *s_now_state;
static lv_obj_t *s_now_icy;
static lv_obj_t *s_level_fill;
static lv_obj_t *s_volume_fill;
static lv_obj_t *s_now_info;
static lv_obj_t *s_now_sleep;
static lv_obj_t *s_now_hint;

static lv_obj_t *s_info_cont;
static lv_obj_t *s_info_label;

static lv_obj_t *s_hint;

static radio_keyboard_t *s_keyboard;
static bool s_initialised;
static char s_about[224];

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static lv_obj_t *plain_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);          // 去掉默认内边距,坐标即所见
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

// 重复出现的盒子(列表行、设置行)用【共享样式】而不是逐对象设属性。
// 逐对象设属性会在每个对象上各存一份本地样式,几十个对象就能吃光 LVGL 内存池 ——
// 这正是打开软键盘后白屏的那个故障。见 radio_styles.h。
static lv_obj_t *styled_box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_add_style(box, radio_style_card(), 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

// 选中态只切换一层样式,并且用记录的状态避免重复 add。
static void set_box_selected(lv_obj_t *box, bool *state, bool selected) {
    if (box == NULL || state == NULL || *state == selected) return;
    if (selected) {
        lv_obj_add_style(box, radio_style_card_selected(), 0);
    } else {
        lv_obj_remove_style(box, radio_style_card_selected(), 0);
    }
    *state = selected;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static void set_visible(lv_obj_t *obj, bool visible) {
    if (obj == NULL) return;
    lv_obj_set_hidden(obj, !visible);
}

// 单行文本:按内容选字体,高度取该字体的真实行高,并在 [box_y, box_y+box_h)
// 里垂直居中。返回所用的行高,便于调用方接着往下排。
static int put_single_line(lv_obj_t *label, const lv_font_t *latin, const char *text,
                           int x, int box_y, int box_h, int w, uint32_t color,
                           bool right_align) {
    const lv_font_t *font = radio_font_for_text(text, latin);
    const int line_h = lv_font_get_line_height(font);
    int y = box_y + (box_h - line_h) / 2;
    if (y < box_y) y = box_y;              // 行高比行框还大时至少不错位到上方

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label,
                                right_align ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(label, w, line_h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_pos(label, x, y);
    return line_h;
}

// 多行文本:显式给固定高度的盒子,行数由调用方按最坏情况预留。
static void put_wrapped(lv_obj_t *label, const lv_font_t *latin, const char *text,
                        int x, int y, int w, int h, uint32_t color) {
    const lv_font_t *font = radio_font_for_text(text, latin);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_pos(label, x, y);
}

// ---------------------------------------------------------------------------
// 建屏
// ---------------------------------------------------------------------------
static void build_header(void) {
    lv_obj_t *bar = plain_box(s_scr, 0, 0, UI_W, UI_HEADER_H, RADIO_COLOR_BG);
    lv_obj_set_style_radius(bar, 0, 0);

    s_title = make_label(bar, RADIO_COLOR_TEXT);
    lv_obj_set_size(s_title, 126, 22);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_title, 8, UI_HEADER_CENTER - 22 / 2);

    s_net = make_label(bar, RADIO_COLOR_MUTED);
    lv_obj_set_style_text_font(s_net, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_net, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_net, 56, 16);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_net, 138, UI_HEADER_CENTER - 16 / 2);

    s_batt = make_label(bar, RADIO_COLOR_BATTERY);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_batt, 38, 16);
    lv_label_set_long_mode(s_batt, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_batt, 196, UI_HEADER_CENTER - 16 / 2);
}

static void build_list(void) {
    s_list_cont = plain_box(s_scr, 0, UI_ROWS_TOP, UI_W, UI_H - UI_ROWS_TOP - 34,
                            RADIO_COLOR_BG);
    lv_obj_set_style_radius(s_list_cont, 0, 0);

    for (int i = 0; i < RADIO_UI_MAX_ROWS; i++) {
        const int y = i * UI_ROW_STEP;
        s_rows[i].box = styled_box(s_list_cont, UI_PAD, y, UI_ROW_W, UI_ROW_BOX_H);
        s_rows[i].title = make_label(s_rows[i].box, RADIO_COLOR_TEXT);
        s_rows[i].sub = make_label(s_rows[i].box, RADIO_COLOR_MUTED);
        s_rows[i].badge = make_label(s_rows[i].box, RADIO_COLOR_MUTED);
        lv_obj_set_style_text_font(s_rows[i].badge, &lv_font_montserrat_14, 0);
    }
}

static void build_settings(void) {
    s_set_cont = plain_box(s_scr, 0, UI_ROWS_TOP, UI_W, UI_H - UI_ROWS_TOP - 34,
                           RADIO_COLOR_BG);
    lv_obj_set_style_radius(s_set_cont, 0, 0);

    for (int i = 0; i < RADIO_UI_SET_ROWS; i++) {
        const int y = i * UI_SET_STEP;
        s_set[i].box = styled_box(s_set_cont, UI_PAD, y, UI_ROW_W, UI_SET_BOX_H);
        s_set[i].title = make_label(s_set[i].box, RADIO_COLOR_TEXT);
        s_set[i].value = make_label(s_set[i].box, RADIO_COLOR_MUTED);
        lv_obj_set_style_text_font(s_set[i].title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_font(s_set[i].value, &lv_font_montserrat_20, 0);
    }
}

// 播放页只建一次控件,之后只改文本与宽度 —— 每换一台重建整棵对象树会带来
// 堆碎片和功耗,也会让解码任务在刷屏时被抢占。
static void build_now(void) {
    s_now_cont = plain_box(s_scr, 0, UI_ROWS_TOP, UI_W, UI_H - UI_ROWS_TOP - 34,
                           RADIO_COLOR_BG);
    lv_obj_set_style_radius(s_now_cont, 0, 0);

    lv_obj_t *card = plain_box(s_now_cont, UI_PAD, 0, UI_ROW_W, 110, RADIO_COLOR_SURFACE);
    // 台名最多两行。西文 22×2 = 44;万一台名是中文,31×2 = 62,卡片高度按 62 预留。
    s_now_name = make_label(card, RADIO_COLOR_TEXT);
    lv_obj_set_size(s_now_name, 212, 62);
    lv_label_set_long_mode(s_now_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_now_name, 8, 0);
    s_now_meta = make_label(card, RADIO_COLOR_MUTED);
    s_now_state = make_label(card, RADIO_COLOR_ACCENT);

    // 电平条:让用户一眼看出"在响但没声音"和"根本没数据"的区别。
    lv_obj_t *level_bg = plain_box(s_now_cont, UI_PAD, 102, UI_ROW_W, 8, RADIO_COLOR_SURFACE_2);
    lv_obj_set_style_radius(level_bg, 4, 0);
    s_level_fill = plain_box(level_bg, 0, 0, 0, 8, RADIO_COLOR_ACCENT);
    lv_obj_set_style_radius(s_level_fill, 4, 0);

    // 曲目名是动态内容,可能很长也可能含中文:按 2 行中文(31×2=62)预留高度。
    lv_obj_t *icy_box = plain_box(s_now_cont, UI_PAD, 116, UI_ROW_W, 62, RADIO_COLOR_SURFACE);
    s_now_icy = make_label(icy_box, RADIO_COLOR_TEXT);
    lv_obj_set_size(s_now_icy, 212, 62);
    lv_label_set_long_mode(s_now_icy, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_now_icy, 8, 0);

    lv_obj_t *vol_bg = plain_box(s_now_cont, UI_PAD, 182, UI_ROW_W, 8, RADIO_COLOR_SURFACE_2);
    lv_obj_set_style_radius(vol_bg, 4, 0);
    s_volume_fill = plain_box(vol_bg, 0, 0, 0, 8, RADIO_COLOR_WARN);
    lv_obj_set_style_radius(s_volume_fill, 4, 0);

    // 流信息(编码格式 / 实测码率 / 采样率)与睡眠定时倒计时各占一行。
    s_now_info = make_label(s_now_cont, RADIO_COLOR_MUTED);
    lv_obj_set_style_text_font(s_now_info, &lv_font_montserrat_14, 0);
    lv_obj_set_size(s_now_info, 228, 16);
    lv_label_set_long_mode(s_now_info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_now_info, UI_PAD, 194);

    s_now_sleep = make_label(s_now_cont, RADIO_COLOR_WARN);
    lv_obj_set_style_text_font(s_now_sleep, &lv_font_montserrat_14, 0);
    lv_obj_set_size(s_now_sleep, 228, 16);
    lv_label_set_long_mode(s_now_sleep, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_now_sleep, UI_PAD, 212);

    s_now_hint = make_label(s_now_cont, RADIO_COLOR_MUTED);
    lv_obj_set_style_text_font(s_now_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_now_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_now_hint, 228, 16);
    lv_label_set_long_mode(s_now_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_now_hint, UI_PAD, 230);
}

static void build_info(void) {
    s_info_cont = plain_box(s_scr, 0, UI_ROWS_TOP, UI_W, UI_H - UI_ROWS_TOP - 34,
                            RADIO_COLOR_BG);
    lv_obj_set_style_radius(s_info_cont, 0, 0);
    s_info_label = make_label(s_info_cont, RADIO_COLOR_TEXT);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_size(s_info_label, 212, 240);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_info_label, 8, 4);
}

esp_err_t radio_ui_init(void) {
    if (s_initialised) return ESP_OK;

    radio_fonts_init();
    radio_styles_init();   // 必须在创建任何控件之前:控件会引用这些共享样式

    s_scr = lv_obj_create(NULL);
    if (s_scr == NULL) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(RADIO_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    build_header();
    build_list();
    build_settings();
    build_now();
    build_info();

    s_hint = make_label(s_scr, RADIO_COLOR_MUTED);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_size(s_hint, 224, 16);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_hint, UI_PAD + 2, UI_HINT_TOP);

    lv_screen_load(s_scr);
    s_initialised = true;

    // 把基础界面占用的 LVGL 池打出来。软键盘还要再占一块,两者相加拿来和
    // CONFIG_LV_MEM_SIZE 对比,才能判断池子够不够 —— 而不是等它白屏。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "基础界面占 LVGL 池 %u/%u 字节, 最大空闲块 %u",
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
             (unsigned)mon.free_biggest_size);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 逐页渲染
// ---------------------------------------------------------------------------
static const char *page_title(const radio_app_t *app) {
    switch (app->page) {
    case RADIO_PAGE_LIST: return radio_region_name(app->region);
    case RADIO_PAGE_FAVORITES: return "Favorites";
    case RADIO_PAGE_NOW: return "Now Playing";
    case RADIO_PAGE_MENU: return "Menu";
    case RADIO_PAGE_REGION: return "Region";
    case RADIO_PAGE_SETTINGS: return "Settings";
    case RADIO_PAGE_WIFI: return "Wi-Fi";
    case RADIO_PAGE_ABOUT: return "About";
    default: return "Radio";
    }
}

static const char *page_hint(const radio_app_t *app) {
    switch (app->page) {
    case RADIO_PAGE_LIST: return "OK play | hold OK menu";
    case RADIO_PAGE_FAVORITES: return "OK play | hold OK back";
    case RADIO_PAGE_NOW: return "Up/Down volume | hold back";
    case RADIO_PAGE_WIFI: return "OK connect | hold OK back";
    case RADIO_PAGE_ABOUT: return "hold OK = back";
    case RADIO_PAGE_SETTINGS:
        // 调节模式下上下键的含义变成了"改数值",提示必须跟着变,否则用户会以为
        // 自己还在选择列表项。
        return app->adjust ? "Up/Down change | OK done" : "OK adjust | hold OK back";
    default: return "OK select | hold OK back";
    }
}

static void render_header(const radio_app_t *app) {
    // 标题可能是地区名,理论上只会是英文;仍按内容选字体以防万一。
    const lv_font_t *title_font = radio_font_for_text(page_title(app), &lv_font_montserrat_20);
    lv_obj_set_style_text_font(s_title, title_font, 0);
    const int title_h = lv_font_get_line_height(title_font);
    lv_obj_set_size(s_title, 126, title_h);
    lv_obj_set_pos(s_title, 8, UI_HEADER_CENTER - title_h / 2);
    lv_label_set_text(s_title, page_title(app));

    const radio_wifi_state_t wifi = radio_wifi_state();
    uint32_t color = RADIO_COLOR_MUTED;
    switch (wifi) {
    case RADIO_WIFI_STATE_CONNECTED: color = RADIO_COLOR_ACCENT; break;
    case RADIO_WIFI_STATE_FAILED: color = RADIO_COLOR_DANGER; break;
    case RADIO_WIFI_STATE_SCANNING:
    case RADIO_WIFI_STATE_CONNECTING: color = RADIO_COLOR_WARN; break;
    default: break;
    }
    lv_label_set_text(s_net, radio_wifi_state_text());
    lv_obj_set_style_text_color(s_net, lv_color_hex(color), 0);

    const int soc = bsp_battery_soc();
    if (soc < 0) {
        // 读不到电量就留空:画一个假的百分比比不画更糟。
        lv_label_set_text(s_batt, "");
    } else {
        // 缓冲区按最坏情况留够(10 位数字 + '%' + NUL),避免 -Wformat-truncation。
        char text[16];
        snprintf(text, sizeof(text), "%d%%", soc);
        lv_label_set_text(s_batt, text);
    }
}

static void render_rows_page(const radio_app_t *app, radio_ui_rows_fn rows_fn, void *user) {
    const int count = radio_app_count(app, app->page);
    const int selected = radio_app_sel(app);
    // ⚠ 必须把滚动偏移加回行号。早先这里直接拿槽位 i 当行号去取内容,于是
    // 49 个台的列表永远只显示前 5 个,而选中项一旦移出前 5 行就"消失"了
    // (高亮判据同样用的是槽位号)。真机上的表现是:按到第 6 个台时列表看起来
    // 卡住了,其实游标还在往下走。
    const int first = radio_app_scroll(app);

    for (int i = 0; i < RADIO_UI_MAX_ROWS; i++) {
        const int index = first + i;
        radio_ui_row_t row = {0};
        const bool filled = (index < count) && rows_fn != NULL && rows_fn(index, &row, user);
        set_visible(s_rows[i].box, filled);
        if (!filled) {
            // 隐藏后仍要清掉文本,否则复用行时旧文字会在下一帧闪一下。
            lv_label_set_text(s_rows[i].title, "");
            lv_label_set_text(s_rows[i].sub, "");
            lv_label_set_text(s_rows[i].badge, "");
            continue;
        }

        const char *title = row.title != NULL ? row.title : "";
        const char *sub = row.sub != NULL ? row.sub : "";
        const bool two_lines = (sub[0] != '\0') &&
                               !radio_text_needs_cjk(title) &&
                               !radio_text_needs_cjk(sub);

        const uint32_t title_color = row.accent ? RADIO_COLOR_ACCENT : RADIO_COLOR_TEXT;
        if (two_lines) {
            // 台名一行 + 说明一行。两行都用西文字体,合计 22 + 16 = 38 < 47。
            put_single_line(s_rows[i].title, &lv_font_montserrat_20, title,
                            8, 3, 22, 136, title_color, false);
            put_single_line(s_rows[i].sub, &lv_font_montserrat_14, sub,
                            8, 28, 16, 212, RADIO_COLOR_MUTED, false);
        } else {
            // 任一行含中文:换成中文字体后行高会变大(31),两行放不下 47 px,
            // 所以只画主文本并在行框里居中 —— 宁可少一行信息,也不让文字溢出。
            put_single_line(s_rows[i].title, &lv_font_montserrat_20, title,
                            8, 0, UI_ROW_BOX_H, 212, title_color, false);
            lv_label_set_text(s_rows[i].sub, "");
        }

        const char *badge = row.badge != NULL ? row.badge : "";
        put_single_line(s_rows[i].badge, &lv_font_montserrat_14, badge,
                        150, two_lines ? 3 : 0, two_lines ? 22 : UI_ROW_BOX_H, 70,
                        row.accent ? RADIO_COLOR_ACCENT : RADIO_COLOR_MUTED, true);

        const bool is_sel = (index == selected);
        set_box_selected(s_rows[i].box, &s_rows[i].selected, is_sel);
    }
}

static void render_settings(const radio_app_t *app) {
    // 顺序必须与 radio_set_row_t 一致:可调项在前,可进项在后。
    static const char *const kLabels[RADIO_SET_ROW_COUNT] = {
        "Volume", "Brightness", "Auto off", "Sleep", "Wi-Fi", "About",
    };

    const int count = radio_app_count(app, RADIO_PAGE_SETTINGS);
    const int selected = radio_app_sel(app);
    const int first = radio_app_scroll(app);   // 同列表页:槽位号 != 行号

    for (int i = 0; i < RADIO_UI_SET_ROWS; i++) {
        const int index = first + i;
        const bool filled = (index < count) && (index < RADIO_SET_ROW_COUNT);
        set_visible(s_set[i].box, filled);
        if (!filled) {
            lv_label_set_text(s_set[i].title, "");
            lv_label_set_text(s_set[i].value, "");
            continue;
        }

        const bool is_sel = (index == selected);
        // 调节模式:被调的那一行用警告色描边,提示"这时上下键是在改数值"。
        const bool adjusting = is_sel && app->adjust;
        set_box_selected(s_set[i].box, &s_set[i].selected, is_sel);
        // ⚠ 黄框必须是可叠加/可移除的样式。早先用 lv_obj_set_style_border_* 设了
        // 一次就再没复位,退出调节模式后黄框会一直留在那一行(真机反馈)。
        if (s_set[i].adjusting != adjusting) {
            if (adjusting) {
                lv_obj_add_style(s_set[i].box, radio_style_card_adjusting(), 0);
            } else {
                lv_obj_remove_style(s_set[i].box, radio_style_card_adjusting(), 0);
            }
            s_set[i].adjusting = adjusting;
        }

        put_single_line(s_set[i].title, &lv_font_montserrat_20, kLabels[index],
                        8, 0, UI_SET_BOX_H, 110, RADIO_COLOR_TEXT, false);

        char value[40] = "";
        switch (index) {
        case RADIO_SET_ROW_VOLUME:
        case RADIO_SET_ROW_BRIGHTNESS:
        case RADIO_SET_ROW_AUTO_OFF:
            radio_set_row_format(app, index, value, sizeof(value));
            break;
        case RADIO_SET_ROW_SLEEP: {
            // 正在倒计时就把剩余时间接在档位后面,这样用户不必切到播放页去确认。
            radio_set_row_format(app, index, value, sizeof(value));
            char left[16] = "";
            radio_app_sleep_format(app, left, sizeof(left));
            if (left[0] != '\0') {
                const size_t used = strlen(value);
                if (used + 2 < sizeof(value)) {
                    snprintf(value + used, sizeof(value) - used, " %s", left);
                }
            }
            break;
        }
        case RADIO_SET_ROW_WIFI: {
            // SSID 是动态文本,可能含中文 —— put_single_line 会按内容选字体。
            char ssid[RADIO_WIFI_SSID_MAX];
            if (radio_wifi_saved_ssid(ssid, sizeof(ssid))) {
                strncpy(value, ssid, sizeof(value) - 1);
            } else {
                strncpy(value, "not set", sizeof(value) - 1);
            }
            break;
        }
        default:
            value[0] = '\0';
            break;
        }
        put_single_line(s_set[i].value, &lv_font_montserrat_20, value,
                        122, 0, UI_SET_BOX_H, 98,
                        adjusting ? RADIO_COLOR_WARN : RADIO_COLOR_MUTED, true);
    }
}

static void render_now(const radio_app_t *app, radio_ui_rows_fn rows_fn, void *user) {
    radio_ui_row_t row = {0};
    const char *name = "-";
    const char *badge = "";
    if (rows_fn != NULL && rows_fn(app->playing_row, &row, user)) {
        if (row.title != NULL) name = row.title;
        if (row.badge != NULL) badge = row.badge;
    }
    put_wrapped(s_now_name, &lv_font_montserrat_20, name, 8, 2, 212, 64,
                RADIO_COLOR_TEXT);

    // 台名两行之后的位置固定:2 行西文 44,两行中文 62,统一按 64 预留。
    char meta[72];
    if (badge[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s | FM %s", radio_region_name(app->region), badge);
    } else {
        snprintf(meta, sizeof(meta), "%s", radio_region_name(app->region));
    }
    if (radio_wifi_state() == RADIO_WIFI_STATE_CONNECTED) {
        const int rssi = radio_wifi_rssi();
        if (rssi < 0) {
            size_t len = strlen(meta);
            snprintf(meta + len, sizeof(meta) - len, " | %d dBm", rssi);
        }
    }
    put_single_line(s_now_meta, &lv_font_montserrat_14, meta, 8, 68, 16, 212,
                    RADIO_COLOR_MUTED, false);

    uint32_t state_color = RADIO_COLOR_MUTED;
    switch (radio_player_state()) {
    case RADIO_PLAYER_PLAYING: state_color = RADIO_COLOR_ACCENT; break;
    case RADIO_PLAYER_BUFFERING:
    case RADIO_PLAYER_CONNECTING:
    case RADIO_PLAYER_RECONNECTING: state_color = RADIO_COLOR_WARN; break;
    case RADIO_PLAYER_ERROR: state_color = RADIO_COLOR_DANGER; break;
    default: break;
    }
    // 出错时把具体原因接在状态后面,否则用户只看到 "Failed" 却不知道是网络、
    // 服务器还是格式问题。单行截断,不会折行。
    char status[64];
    if (radio_player_state() == RADIO_PLAYER_ERROR) {
        snprintf(status, sizeof(status), "%s: %s", radio_player_state_text(),
                 radio_player_last_error());
    } else {
        snprintf(status, sizeof(status), "%s", radio_player_state_text());
    }
    put_single_line(s_now_state, &lv_font_montserrat_14, status, 8, 86, 16, 212,
                    state_color, false);

    char title[RADIO_ICY_TITLE_CAP];
    radio_player_title(title, sizeof(title));
    if (title[0] == '\0') {
        put_wrapped(s_now_icy, &lv_font_montserrat_14, "No track info", 8, 0, 212, 62,
                    RADIO_COLOR_MUTED);
    } else {
        // 曲目名是动态内容,可能是中文;多行高度按两行中文(62)预留,不会溢出。
        put_wrapped(s_now_icy, &lv_font_montserrat_14, title, 8, 0, 212, 62,
                    RADIO_COLOR_TEXT);
    }

    const int level = radio_player_level();
    lv_obj_set_width(s_level_fill, (UI_ROW_W - 2) * level / 100);
    set_visible(s_level_fill, level > 0);

    lv_obj_set_width(s_volume_fill, (UI_ROW_W - 2) * app->volume / 100);

    // 断网提示放在最显眼的一行:用户最容易遇到的故障就是"忘了连网/路由器断了",
    // 而播放器这时只会在状态行里显示一句 Failed,不够醒目。颜色用危险色。
    if (radio_wifi_state() != RADIO_WIFI_STATE_CONNECTED) {
        put_single_line(s_now_info, &lv_font_montserrat_14,
                        "No Wi-Fi: cannot stream", UI_PAD, 194, 16, 228,
                        RADIO_COLOR_DANGER, false);
    } else {
        char stream[48] = "";
        radio_player_stream_info(stream, sizeof(stream));
        put_single_line(s_now_info, &lv_font_montserrat_14, stream, UI_PAD, 194, 16, 228,
                        RADIO_COLOR_MUTED, false);
    }

    // 剩余时间的缓冲按 16 位秒数的最坏情况留够("65535" -> "1092:15"),否则
    // -Werror=format-truncation 会在拼 "Sleep timer %s" 时直接报错。
    char left[16] = "";
    radio_app_sleep_format(app, left, sizeof(left));
    if (left[0] != '\0') {
        char text[48];
        snprintf(text, sizeof(text), "Sleep timer %s", left);
        put_single_line(s_now_sleep, &lv_font_montserrat_14, text, UI_PAD, 212, 16, 228,
                        RADIO_COLOR_WARN, false);
    } else {
        lv_label_set_text(s_now_sleep, "");
    }

    lv_label_set_text(s_now_hint,
                      app->playing ? "2x OK = next station" : "Pick a station to play");
}

void radio_ui_set_about_text(const char *text) {
    if (text == NULL) return;
    strncpy(s_about, text, sizeof(s_about) - 1);
    s_about[sizeof(s_about) - 1] = '\0';
}

void radio_ui_render(const radio_app_t *app, radio_ui_rows_fn rows_fn, void *user) {
    if (!s_initialised || app == NULL) return;

    render_header(app);

    const bool list_page = (app->page == RADIO_PAGE_LIST ||
                            app->page == RADIO_PAGE_FAVORITES ||
                            app->page == RADIO_PAGE_MENU ||
                            app->page == RADIO_PAGE_REGION ||
                            app->page == RADIO_PAGE_WIFI);

    set_visible(s_list_cont, list_page);
    set_visible(s_set_cont, app->page == RADIO_PAGE_SETTINGS);
    set_visible(s_now_cont, app->page == RADIO_PAGE_NOW);
    set_visible(s_info_cont, app->page == RADIO_PAGE_ABOUT);

    if (list_page) {
        render_rows_page(app, rows_fn, user);
    } else if (app->page == RADIO_PAGE_SETTINGS) {
        render_settings(app);
    } else if (app->page == RADIO_PAGE_NOW) {
        render_now(app, rows_fn, user);
    } else if (app->page == RADIO_PAGE_ABOUT) {
        lv_label_set_text(s_info_label, s_about);
    }

    lv_label_set_text(s_hint, page_hint(app));
}

// ---------------------------------------------------------------------------
// 软键盘
// ---------------------------------------------------------------------------
bool radio_ui_keyboard_open(const char *title, const char *subtitle,
                            const char *initial,
                            void (*on_done)(const char *text, void *user),
                            void (*on_cancel)(void *user), void *user) {
    if (!s_initialised || s_keyboard != NULL) return false;
    // 调用方(main.c)已经持有 LVGL 锁;这把锁是递归锁,这里不再重复加锁。
    s_keyboard = radio_keyboard_create(lv_screen_active(), title, subtitle, initial,
                                       on_done, on_cancel, user);
    return s_keyboard != NULL;
}

void radio_ui_keyboard_close(void) {
    if (s_keyboard == NULL) return;
    radio_keyboard_destroy(s_keyboard);
    s_keyboard = NULL;
}

bool radio_ui_keyboard_active(void) {
    return s_keyboard != NULL;
}

void radio_ui_keyboard_key(radio_key_t key) {
    if (s_keyboard == NULL) return;
    radio_kb_key_t mapped = RADIO_KB_KEY_OK;
    switch (key) {
    case RADIO_KEY_UP: mapped = RADIO_KB_KEY_UP; break;
    case RADIO_KEY_DOWN: mapped = RADIO_KB_KEY_DOWN; break;
    case RADIO_KEY_UP_LONG: mapped = RADIO_KB_KEY_UP_LONG; break;
    case RADIO_KEY_DOWN_LONG: mapped = RADIO_KB_KEY_DOWN_LONG; break;
    case RADIO_KEY_OK: mapped = RADIO_KB_KEY_OK; break;
    case RADIO_KEY_OK_LONG: mapped = RADIO_KB_KEY_OK_LONG; break;
    case RADIO_KEY_OK_DOUBLE: mapped = RADIO_KB_KEY_OK_DOUBLE; break;
    default: return;
    }
    (void)radio_keyboard_key(s_keyboard, mapped);
}
