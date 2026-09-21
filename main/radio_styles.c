// main/radio_styles.c —— 共享样式的定义。见头文件里关于"为什么要共享"的说明。
#include "radio_styles.h"

#include "radio_theme.h"

static lv_style_t s_card;
static lv_style_t s_card_selected;
static lv_style_t s_card_adjusting;
static lv_style_t s_key;
static lv_style_t s_key_selected;
static bool s_ready;

void radio_styles_init(void) {
    if (s_ready) return;

    // ---- 卡片 / 列表行 ----------------------------------------------------
    lv_style_init(&s_card);
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_bg_color(&s_card, lv_color_hex(RADIO_COLOR_SURFACE));
    lv_style_set_radius(&s_card, 6);
    lv_style_set_border_width(&s_card, 0);
    lv_style_set_pad_all(&s_card, 0);      // 坐标即所见,回调里靠绝对定位摆放子控件

    // 选中态:后 add 的样式在冲突属性上优先,因此不必先移除常态样式。
    lv_style_init(&s_card_selected);
    lv_style_set_bg_color(&s_card_selected, lv_color_hex(RADIO_COLOR_SURFACE_2));
    lv_style_set_border_width(&s_card_selected, 1);
    lv_style_set_border_color(&s_card_selected, lv_color_hex(RADIO_COLOR_ACCENT));

    // 调节态:后 add 的样式优先,所以它会盖过选中态的强调色描边。
    lv_style_init(&s_card_adjusting);
    lv_style_set_border_width(&s_card_adjusting, 1);
    lv_style_set_border_color(&s_card_adjusting, lv_color_hex(RADIO_COLOR_WARN));

    // ---- 软键盘按键 ------------------------------------------------------
    lv_style_init(&s_key);
    lv_style_set_bg_opa(&s_key, LV_OPA_COVER);
    lv_style_set_bg_color(&s_key, lv_color_hex(RADIO_COLOR_SURFACE_2));
    lv_style_set_radius(&s_key, 4);
    lv_style_set_text_color(&s_key, lv_color_hex(RADIO_COLOR_TEXT));
    lv_style_set_text_align(&s_key, LV_TEXT_ALIGN_CENTER);
    // Montserrat 14 行高 16,键高 21 → 上留 2 px 视觉居中。
    lv_style_set_pad_top(&s_key, 2);
    lv_style_set_pad_hor(&s_key, 0);
    lv_style_set_pad_bottom(&s_key, 0);

    lv_style_init(&s_key_selected);
    lv_style_set_bg_color(&s_key_selected, lv_color_hex(RADIO_COLOR_ACCENT));
    lv_style_set_text_color(&s_key_selected, lv_color_hex(RADIO_COLOR_BG));

    s_ready = true;
}

lv_style_t *radio_style_card(void) { return &s_card; }
lv_style_t *radio_style_card_selected(void) { return &s_card_selected; }
lv_style_t *radio_style_card_adjusting(void) { return &s_card_adjusting; }
lv_style_t *radio_style_key(void) { return &s_key; }
lv_style_t *radio_style_key_selected(void) { return &s_key_selected; }
