// tests/test_radio_app.c —— 三按键界面状态机的主机测试。
//
// 设备只有"上/下/确定",很多交互细节只能靠状态机保证:游标不能越界、滚动窗口
// 要跟着游标、调节模式下按键语义要换一套、子页返回后光标要停在原处。这些用例
// 把每条约定钉死,界面上就算画错了也骗不过这里。
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "radio_app.h"

static radio_app_t app;

static void reset(void) {
    // 音量 55 / 亮度 70 / 自动熄屏第 1 档(30 秒)/ 地区香港。
    radio_app_init(&app, 55, 70, 1, RADIO_REGION_HK);
    radio_app_set_visible_rows(&app, RADIO_PAGE_LIST, 3);
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 10);
}

static radio_action_t press(radio_key_t key) {
    return radio_app_key(&app, key);
}

static void press_n(radio_key_t key, int times) {
    for (int i = 0; i < times; i++) (void)press(key);
}

static void test_init_defaults(void) {
    reset();
    assert(app.page == RADIO_PAGE_LIST);
    assert(app.region == RADIO_REGION_HK);
    assert(app.volume == 55);
    assert(app.brightness == 70);
    assert(app.auto_off_step == 1);
    assert(app.adjust == 0);
    assert(app.stack_depth == 0);
    assert(radio_app_sel(&app) == 0);
    assert(radio_app_scroll(&app) == 0);
    assert(radio_app_count(&app, RADIO_PAGE_LIST) == 10);
    // 固定项数的页面在 init 里就应写好。
    assert(radio_app_count(&app, RADIO_PAGE_MENU) == RADIO_MENU_ROW_COUNT);
    assert(radio_app_count(&app, RADIO_PAGE_SETTINGS) == RADIO_SET_ROW_COUNT);
    assert(radio_app_count(&app, RADIO_PAGE_REGION) == RADIO_REGION_COUNT);
}

static void test_selection_clamps_at_both_ends(void) {
    reset();
    // 顶端再按上键必须停住。
    press_n(RADIO_KEY_UP, 3);
    assert(radio_app_sel(&app) == 0);

    press_n(RADIO_KEY_DOWN, 100);
    assert(radio_app_sel(&app) == 9);

    press_n(RADIO_KEY_UP, 100);
    assert(radio_app_sel(&app) == 0);
}

static void test_scroll_window_follows_the_cursor(void) {
    reset();
    // 可见 3 行、共 10 行。
    press_n(RADIO_KEY_DOWN, 3);
    assert(radio_app_sel(&app) == 3);
    assert(radio_app_scroll(&app) == 1);

    press_n(RADIO_KEY_DOWN, 2);
    assert(radio_app_sel(&app) == 5);
    assert(radio_app_scroll(&app) == 3);

    press_n(RADIO_KEY_UP, 3);
    assert(radio_app_sel(&app) == 2);
    assert(radio_app_scroll(&app) == 2);

    press_n(RADIO_KEY_UP, 5);
    assert(radio_app_sel(&app) == 0);
    assert(radio_app_scroll(&app) == 0);

    // 到底部时滚动窗口不能露出列表之外的空行。
    press_n(RADIO_KEY_DOWN, 50);
    assert(radio_app_sel(&app) == 9);
    assert(radio_app_scroll(&app) == 7);
}

static void test_shrinking_list_clamps_cursor(void) {
    reset();
    press_n(RADIO_KEY_DOWN, 9);
    assert(radio_app_sel(&app) == 9);

    // 例如切换地区后新地区只有 4 个台。
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 4);
    assert(radio_app_sel(&app) == 3);
    assert(radio_app_scroll(&app) <= 1);

    // 空列表也必须安全。
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 0);
    assert(radio_app_sel(&app) == 0);
    assert(radio_app_scroll(&app) == 0);
}

static void test_enter_plays_the_selected_row_and_opens_now_playing(void) {
    reset();
    press_n(RADIO_KEY_DOWN, 4);
    radio_action_t a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_PLAY_INDEX);
    assert(a.value == 4);
    assert(app.page == RADIO_PAGE_NOW);

    // 返回后光标必须还在原来那一行。
    a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_CLOSE_PAGE);
    assert(app.page == RADIO_PAGE_LIST);
    assert(radio_app_sel(&app) == 4);
}

static void test_enter_on_empty_list_does_nothing(void) {
    reset();
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 0);
    radio_action_t a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_NONE);
    assert(app.page == RADIO_PAGE_LIST);
}

static void test_long_up_down_switches_region_with_wraparound(void) {
    reset();
    radio_action_t a = press(RADIO_KEY_DOWN_LONG);
    assert(a.kind == RADIO_ACT_REGION_SET);
    assert(a.value == RADIO_REGION_SG);
    assert(app.region == RADIO_REGION_SG);

    a = press(RADIO_KEY_DOWN_LONG);
    assert(a.value == RADIO_REGION_MY);

    a = press(RADIO_KEY_DOWN_LONG);
    assert(a.value == RADIO_REGION_HK);   // 回环

    a = press(RADIO_KEY_UP_LONG);
    assert(a.value == RADIO_REGION_MY);   // 反向回环
}

static void test_ok_long_opens_menu_and_menu_back_returns_to_list(void) {
    reset();
    radio_action_t a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_OPEN_PAGE);
    assert(a.value == RADIO_PAGE_MENU);
    assert(app.page == RADIO_PAGE_MENU);
    // 菜单是覆盖层,不占页面栈。
    assert(app.stack_depth == 0);

    a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_CLOSE_PAGE);
    assert(app.page == RADIO_PAGE_LIST);
}

static void test_menu_entries_open_their_pages(void) {
    reset();

    // 切换地区
    press(RADIO_KEY_OK_LONG);
    radio_action_t a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_OPEN_PAGE);
    assert(app.page == RADIO_PAGE_REGION);

    // 在地区页选第二个(新加坡),应直接回到列表并清空页面栈。
    press(RADIO_KEY_DOWN);
    a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_REGION_SET);
    assert(app.region == RADIO_REGION_SG);
    assert(app.page == RADIO_PAGE_LIST);
    assert(app.stack_depth == 0);

    // 我的收藏:菜单第一项是地区,所以要下移到第二项。
    press(RADIO_KEY_OK_LONG);
    press(RADIO_KEY_DOWN);
    a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_OPEN_PAGE);
    assert(app.page == RADIO_PAGE_FAVORITES);

    // 收藏页是子页,长按确定返回一层(回到列表)。
    a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_CLOSE_PAGE);
    assert(app.page == RADIO_PAGE_LIST);
}

static void test_menu_settings_then_back_returns_to_list(void) {
    reset();
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    radio_action_t a = press(RADIO_KEY_OK);
    assert(app.page == RADIO_PAGE_SETTINGS);

    // 菜单进入的子页返回一步就是列表,不该再回到菜单。
    a = press(RADIO_KEY_OK_LONG);
    assert(app.page == RADIO_PAGE_LIST);
    (void)a;
}

static void test_settings_adjust_mode_changes_volume(void) {
    reset();
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    press(RADIO_KEY_OK);                     // 进入设置页,光标在第 0 行(音量)
    assert(radio_app_sel(&app) == RADIO_SET_ROW_VOLUME);
    assert(app.adjust == 0);

    radio_action_t a = press(RADIO_KEY_OK);  // 进入调节模式
    assert(a.kind == RADIO_ACT_REDRAW);
    assert(app.adjust == 1);

    a = press(RADIO_KEY_UP);
    assert(a.kind == RADIO_ACT_VOLUME);
    assert(a.value == 60);
    assert(app.volume == 60);

    a = press(RADIO_KEY_DOWN);
    assert(a.value == 55);

    // 长按给更粗的步进。
    a = press(RADIO_KEY_UP_LONG);
    assert(a.value == 75);

    // 上限夹紧。
    press_n(RADIO_KEY_UP_LONG, 5);
    assert(app.volume == 100);

    // 在调节模式下,确定键是"改完了",不能返回上一层。
    a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_REDRAW);
    assert(app.adjust == 0);
    assert(app.page == RADIO_PAGE_SETTINGS);

    // 退出调节模式后,上/下重新变成移动光标。
    a = press(RADIO_KEY_DOWN);
    assert(a.kind == RADIO_ACT_REDRAW);
    assert(radio_app_sel(&app) == RADIO_SET_ROW_BRIGHTNESS);
}

static void test_settings_brightness_has_a_floor(void) {
    reset();
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    press(RADIO_KEY_OK);
    press(RADIO_KEY_DOWN);                   // 移到亮度行
    press(RADIO_KEY_OK);                     // 进入调节

    press_n(RADIO_KEY_DOWN_LONG, 10);
    // 亮度不允许调到 0:全黑会被误认为死机。
    assert(app.brightness == 10);

    press_n(RADIO_KEY_UP_LONG, 10);
    assert(app.brightness == 100);
}

static void test_settings_auto_off_cycles_within_range(void) {
    reset();
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    press(RADIO_KEY_OK);
    press_n(RADIO_KEY_DOWN, RADIO_SET_ROW_AUTO_OFF);
    press(RADIO_KEY_OK);                     // 进入调节

    radio_action_t a = press(RADIO_KEY_UP);
    assert(a.kind == RADIO_ACT_SETTINGS_CHANGED);
    assert(app.auto_off_step == 2);

    press_n(RADIO_KEY_UP, 10);
    assert(app.auto_off_step == RADIO_AUTO_OFF_STEPS - 1);

    press_n(RADIO_KEY_DOWN, 10);
    assert(app.auto_off_step == 0);
}

static void test_settings_text_rows_open_pages(void) {
    reset();
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    press(RADIO_KEY_OK);

    // 文本行(Wi-Fi)在非调节模式下进入子页,并压栈,所以返回一步是设置页。
    press_n(RADIO_KEY_DOWN, RADIO_SET_ROW_WIFI);
    assert(radio_app_sel(&app) == RADIO_SET_ROW_WIFI);
    radio_action_t a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_OPEN_PAGE);
    assert(app.page == RADIO_PAGE_WIFI);

    a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_CLOSE_PAGE);
    assert(app.page == RADIO_PAGE_SETTINGS);

    // 关于页同理。
    press_n(RADIO_KEY_DOWN, RADIO_SET_ROW_ABOUT - RADIO_SET_ROW_WIFI);
    assert(radio_app_sel(&app) == RADIO_SET_ROW_ABOUT);
    a = press(RADIO_KEY_OK);
    assert(app.page == RADIO_PAGE_ABOUT);
    a = press(RADIO_KEY_OK);                 // 关于页的确定键没有副作用
    assert(a.kind == RADIO_ACT_NONE);
    a = press(RADIO_KEY_OK_LONG);
    assert(app.page == RADIO_PAGE_SETTINGS);
}

static void test_now_playing_keys(void) {
    reset();
    press(RADIO_KEY_OK);                     // 播放第 0 行,进入播放页
    assert(app.page == RADIO_PAGE_NOW);

    radio_action_t a = press(RADIO_KEY_UP);
    assert(a.kind == RADIO_ACT_VOLUME);
    assert(a.value == 60);

    a = press(RADIO_KEY_DOWN);
    assert(a.value == 55);

    a = press(RADIO_KEY_UP_LONG);
    assert(a.value == 75);

    a = press(RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_TOGGLE_PLAY);

    a = press(RADIO_KEY_OK_DOUBLE);
    assert(a.kind == RADIO_ACT_NEXT_STATION);

    a = press(RADIO_KEY_OK_LONG);
    assert(a.kind == RADIO_ACT_CLOSE_PAGE);
    assert(app.page == RADIO_PAGE_LIST);
}

static void test_volume_clamps_on_now_playing_page(void) {
    reset();
    press(RADIO_KEY_OK);
    press_n(RADIO_KEY_UP_LONG, 10);
    assert(app.volume == 100);
    press_n(RADIO_KEY_DOWN_LONG, 20);
    assert(app.volume == 0);
}

static void test_favorite_toggle_reports_the_row(void) {
    reset();
    press_n(RADIO_KEY_DOWN, 6);
    radio_action_t a = press(RADIO_KEY_OK_DOUBLE);
    assert(a.kind == RADIO_ACT_FAVORITE_TOGGLE);
    assert(a.value == 6);

    // 空列表上双击不应产生动作。
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 0);
    a = press(RADIO_KEY_OK_DOUBLE);
    assert(a.kind == RADIO_ACT_NONE);
}

static void test_pages_keep_independent_cursors(void) {
    reset();
    // 列表页停在第 3 行
    press_n(RADIO_KEY_DOWN, 3);
    assert(radio_app_sel(&app) == 3);

    // 进菜单并下移两行
    press(RADIO_KEY_OK_LONG);
    press_n(RADIO_KEY_DOWN, 2);
    assert(radio_app_sel(&app) == 2);

    // 回到列表,光标应还是第 3 行
    press(RADIO_KEY_OK_LONG);
    assert(app.page == RADIO_PAGE_LIST);
    assert(radio_app_sel(&app) == 3);

    // 再进菜单,菜单光标也应保留
    press(RADIO_KEY_OK_LONG);
    assert(radio_app_sel(&app) == 2);
}

static void test_setting_value_formatting(void) {
    char buf[24];
    reset();

    radio_set_row_format(&app, RADIO_SET_ROW_VOLUME, buf, sizeof(buf));
    assert(strcmp(buf, "55%") == 0);

    radio_set_row_format(&app, RADIO_SET_ROW_BRIGHTNESS, buf, sizeof(buf));
    assert(strcmp(buf, "70%") == 0);

    // 档位表是 {0, 15, 30, 60, 120, 300, 600} 秒。
    app.auto_off_step = 0;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "Off") == 0);

    app.auto_off_step = 1;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "15s") == 0);

    app.auto_off_step = 2;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "30s") == 0);

    app.auto_off_step = 3;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "1min") == 0);

    app.auto_off_step = 4;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "2min") == 0);

    app.auto_off_step = 5;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "5min") == 0);

    app.auto_off_step = RADIO_AUTO_OFF_STEPS - 1;
    radio_set_row_format(&app, RADIO_SET_ROW_AUTO_OFF, buf, sizeof(buf));
    assert(strcmp(buf, "10min") == 0);

    // 档位表本身要单调递增,且除 0(关)以外都 > 0 —— 否则"选了档位却不熄屏"。
    assert(RADIO_AUTO_OFF_SECONDS[0] == 0);
    for (int i = 1; i < RADIO_AUTO_OFF_STEPS; i++) {
        assert(RADIO_AUTO_OFF_SECONDS[i] > RADIO_AUTO_OFF_SECONDS[i - 1]);
    }

    // 文本行没有数值;容量为 0 时不能写坏内存。
    assert(radio_set_row_value(&app, RADIO_SET_ROW_WIFI) == -1);
    radio_set_row_format(&app, RADIO_SET_ROW_WIFI, buf, sizeof(buf));
    assert(buf[0] == '\0');
    radio_set_row_format(&app, RADIO_SET_ROW_VOLUME, NULL, 0);
}

// 睡眠定时:档位表、倒计时、到点回调与"不跨重启保留"。
static void test_sleep_step_table_is_monotonic(void) {
    assert(RADIO_SLEEP_MINUTES[0] == 0);          // 第 0 档必须是"关"
    for (int i = 1; i < RADIO_SLEEP_STEPS; i++) {
        assert(RADIO_SLEEP_MINUTES[i] > RADIO_SLEEP_MINUTES[i - 1]);
    }
    // 设置页里可调项必须包含睡眠定时,而 Wi-Fi/关于是"进子页"的文本行。
    assert(radio_set_row_is_adjustable(RADIO_SET_ROW_VOLUME));
    assert(radio_set_row_is_adjustable(RADIO_SET_ROW_SLEEP));
    assert(!radio_set_row_is_adjustable(RADIO_SET_ROW_WIFI));
    assert(!radio_set_row_is_adjustable(RADIO_SET_ROW_ABOUT));
    assert(!radio_set_row_is_adjustable(-1));
    assert(!radio_set_row_is_adjustable(RADIO_SET_ROW_COUNT));
}

static void test_settings_sleep_row_starts_the_countdown(void) {
    reset();
    press(RADIO_KEY_OK_LONG);                       // 打开主菜单
    press_n(RADIO_KEY_DOWN, RADIO_MENU_ROW_SETTINGS);
    press(RADIO_KEY_OK);
    press_n(RADIO_KEY_DOWN, RADIO_SET_ROW_SLEEP);
    assert(radio_app_sel(&app) == RADIO_SET_ROW_SLEEP);
    assert(radio_set_row_value(&app, RADIO_SET_ROW_SLEEP) == 0);

    press(RADIO_KEY_OK);                            // 进入调节模式
    assert(app.adjust == 1);

    radio_action_t a = press(RADIO_KEY_UP);
    assert(a.kind == RADIO_ACT_SLEEP_SET);
    assert(a.value == 1);                           // 第 1 档 = 15 分钟
    assert(app.sleep_step == 1);
    assert(radio_app_sleep_left(&app) == 15 * 60);

    // 长按按两档跳:便于从"关"快速走到 30/60 分钟。
    a = press(RADIO_KEY_UP_LONG);
    assert(a.kind == RADIO_ACT_SLEEP_SET);
    assert(a.value == 3);                           // 1 + 2 档 = 60 分钟

    // 走到最上面一档后再上调必须停在最上面一档(不能越界)。
    press_n(RADIO_KEY_UP_LONG, 5);
    assert(app.sleep_step == RADIO_SLEEP_STEPS - 1);
    press_n(RADIO_KEY_DOWN_LONG, 10);
    assert(app.sleep_step == 0);
    assert(radio_app_sleep_left(&app) == 0);        // 关掉之后不再倒计时
}

static void test_sleep_countdown_expires_exactly_once(void) {
    reset();
    radio_app_sleep_set(&app, 1);                   // 15 分钟
    assert(radio_app_sleep_left(&app) == 15 * 60);

    for (int i = 0; i < 15 * 60 - 1; i++) {
        assert(!radio_app_sleep_tick(&app));
    }
    assert(radio_app_sleep_left(&app) == 1);

    // 最后一秒返回 true(调用方据此停播并深睡),并把档位复位。
    assert(radio_app_sleep_tick(&app));
    assert(app.sleep_step == 0);
    assert(radio_app_sleep_left(&app) == 0);
    // 已经到点之后不能再重复触发,否则每次 tick 都会让设备再深睡一次。
    assert(!radio_app_sleep_tick(&app));
}

static void test_sleep_state_is_not_restored_by_init(void) {
    reset();
    radio_app_sleep_set(&app, 3);
    assert(app.sleep_step == 3);

    // 倒计时是"今晚这一次"的临时设定:重开应用必须回到"未启用"。
    radio_app_init(&app, 55, 70, 1, RADIO_REGION_HK);
    assert(app.sleep_step == 0);
    assert(radio_app_sleep_left(&app) == 0);
}

static void test_sleep_formatting_and_bounds(void) {
    char buf[24];
    reset();

    radio_app_sleep_format(&app, buf, sizeof(buf));
    assert(buf[0] == '\0');                         // 未启用时不显示

    radio_app_sleep_set(&app, 2);                   // 30 分钟
    radio_app_sleep_format(&app, buf, sizeof(buf));
    assert(strcmp(buf, "30:00") == 0);

    radio_app_sleep_set(&app, RADIO_SLEEP_STEPS - 1);   // 90 分钟
    radio_app_sleep_format(&app, buf, sizeof(buf));
    assert(strcmp(buf, "90:00") == 0);

    // 设置页那一行的文本:档位时长,而不是剩余时间。
    radio_app_sleep_set(&app, 1);
    radio_set_row_format(&app, RADIO_SET_ROW_SLEEP, buf, sizeof(buf));
    assert(strcmp(buf, "15min") == 0);
    assert(radio_set_row_value(&app, RADIO_SET_ROW_SLEEP) == 1);
    radio_app_sleep_set(&app, 0);
    radio_set_row_format(&app, RADIO_SET_ROW_SLEEP, buf, sizeof(buf));
    assert(strcmp(buf, "Off") == 0);

    app.sleep_left_s = 61;
    radio_app_sleep_format(&app, buf, sizeof(buf));
    assert(strcmp(buf, "1:01") == 0);

    // 越界档位按"关"处理;NULL 与容量 0 不能写坏内存。
    radio_app_sleep_set(&app, 99);
    assert(app.sleep_step == 0);
    radio_app_sleep_set(NULL, 1);
    assert(!radio_app_sleep_tick(NULL));
    assert(radio_app_sleep_left(NULL) == 0);
    radio_app_sleep_format(&app, NULL, 0);
    radio_app_sleep_format(NULL, buf, sizeof(buf));
    assert(buf[0] == '\0');
}

// 全屏列表(49 个台、可见 5 行)也要能把光标移到最后一个台,并且窗口不越界。
static void test_full_screen_scroll_window(void) {
    reset();
    radio_app_set_visible_rows(&app, RADIO_PAGE_LIST, 5);
    radio_app_set_row_count(&app, RADIO_PAGE_LIST, 49);

    press_n(RADIO_KEY_DOWN, 6);
    assert(radio_app_sel(&app) == 6);
    assert(radio_app_scroll(&app) == 2);            // 选中项必须落在窗口 2..6 内

    press_n(RADIO_KEY_DOWN, 100);
    assert(radio_app_sel(&app) == 48);
    assert(radio_app_scroll(&app) == 44);           // 最后 5 行,不露出空行

    press_n(RADIO_KEY_UP, 100);
    assert(radio_app_sel(&app) == 0);
    assert(radio_app_scroll(&app) == 0);
}

static void test_null_app_is_safe(void) {
    radio_action_t a = radio_app_key(NULL, RADIO_KEY_OK);
    assert(a.kind == RADIO_ACT_NONE);
    assert(radio_app_sel(NULL) == 0);
    assert(radio_app_scroll(NULL) == 0);
    assert(radio_app_count(NULL, RADIO_PAGE_LIST) == 0);
    radio_app_clamp(NULL);
    radio_app_set_row_count(NULL, RADIO_PAGE_LIST, 5);
    radio_app_set_visible_rows(NULL, RADIO_PAGE_LIST, 5);
    radio_app_init(NULL, 50, 50, 0, 0);
}

static void test_region_names_are_bounded(void) {
    assert(strcmp(radio_region_name(RADIO_REGION_HK), "Hong Kong") == 0);
    assert(strcmp(radio_region_name(RADIO_REGION_SG), "Singapore") == 0);
    assert(strcmp(radio_region_name(RADIO_REGION_MY), "Malaysia") == 0);
    // 越界要退化到第一个,而不是读数组外。
    assert(strcmp(radio_region_name(99), "Hong Kong") == 0);
}

int main(void) {
    test_init_defaults();
    test_selection_clamps_at_both_ends();
    test_scroll_window_follows_the_cursor();
    test_shrinking_list_clamps_cursor();
    test_enter_plays_the_selected_row_and_opens_now_playing();
    test_enter_on_empty_list_does_nothing();
    test_long_up_down_switches_region_with_wraparound();
    test_ok_long_opens_menu_and_menu_back_returns_to_list();
    test_menu_entries_open_their_pages();
    test_menu_settings_then_back_returns_to_list();
    test_settings_adjust_mode_changes_volume();
    test_settings_brightness_has_a_floor();
    test_settings_auto_off_cycles_within_range();
    test_settings_text_rows_open_pages();
    test_now_playing_keys();
    test_volume_clamps_on_now_playing_page();
    test_favorite_toggle_reports_the_row();
    test_pages_keep_independent_cursors();
    test_setting_value_formatting();
    test_sleep_step_table_is_monotonic();
    test_settings_sleep_row_starts_the_countdown();
    test_sleep_countdown_expires_exactly_once();
    test_sleep_state_is_not_restored_by_init();
    test_sleep_formatting_and_bounds();
    test_full_screen_scroll_window();
    test_null_app_is_safe();
    test_region_names_are_bounded();
    return 0;
}
