// main/radio_app.c —— 界面状态机实现。纯逻辑,无 ESP-IDF / LVGL 依赖。
#include "radio_app.h"

#include <stdio.h>
#include <string.h>

// 自动熄屏档位:关 / 15 秒 / 30 秒 / 1 分 / 2 分 / 5 分 / 10 分。
const uint16_t RADIO_AUTO_OFF_SECONDS[RADIO_AUTO_OFF_STEPS] = {0, 15, 30, 60, 120, 300, 600};

// 睡眠定时档位:关 / 15 / 30 / 60 / 90 分钟。
const uint16_t RADIO_SLEEP_MINUTES[RADIO_SLEEP_STEPS] = {0, 15, 30, 60, 90};

static const char *const kRegionNames[RADIO_REGION_COUNT] = {
    "Hong Kong", "Singapore", "Malaysia",
};

const char *radio_region_name(uint8_t region) {
    if (region >= RADIO_REGION_COUNT) return kRegionNames[0];
    return kRegionNames[region];
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

bool radio_set_row_is_adjustable(int row) {
    // 可调项排在前面,最后一个是可调项(睡眠定时),之后是"进入子页"的文本行。
    return row >= 0 && row <= RADIO_SET_ROW_SLEEP;
}

int radio_set_row_value(const radio_app_t *app, int row) {
    switch (row) {
    case RADIO_SET_ROW_VOLUME: return app->volume;
    case RADIO_SET_ROW_BRIGHTNESS: return app->brightness;
    case RADIO_SET_ROW_AUTO_OFF: return app->auto_off_step;
    case RADIO_SET_ROW_SLEEP: return app->sleep_step;
    default: return -1;
    }
}

void radio_set_row_format(const radio_app_t *app, int row, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    switch (row) {
    case RADIO_SET_ROW_VOLUME:
        snprintf(out, cap, "%u%%", (unsigned)app->volume);
        break;
    case RADIO_SET_ROW_BRIGHTNESS:
        snprintf(out, cap, "%u%%", (unsigned)app->brightness);
        break;
    case RADIO_SET_ROW_AUTO_OFF: {
        const uint16_t seconds = RADIO_AUTO_OFF_SECONDS[app->auto_off_step];
        if (seconds == 0) {
            snprintf(out, cap, "Off");
        } else if (seconds < 60) {
            snprintf(out, cap, "%us", (unsigned)seconds);
        } else {
            snprintf(out, cap, "%umin", (unsigned)(seconds / 60));
        }
        break;
    }
    case RADIO_SET_ROW_SLEEP: {
        const uint16_t minutes = RADIO_SLEEP_MINUTES[app->sleep_step];
        if (minutes == 0) {
            snprintf(out, cap, "Off");
        } else {
            snprintf(out, cap, "%umin", (unsigned)minutes);
        }
        break;
    }
    default:
        out[0] = '\0';
        break;
    }
}

void radio_app_sleep_set(radio_app_t *app, uint8_t step) {
    if (app == NULL) return;
    if (step >= RADIO_SLEEP_STEPS) step = 0;
    app->sleep_step = step;
    // 档位为 0(关)时剩余时间归零,表示"没有在倒计时"。
    app->sleep_left_s = (uint16_t)((uint32_t)RADIO_SLEEP_MINUTES[step] * 60u);
}

bool radio_app_sleep_tick(radio_app_t *app) {
    if (app == NULL) return false;
    if (app->sleep_step == 0 || app->sleep_left_s == 0) return false;
    app->sleep_left_s--;
    if (app->sleep_left_s == 0) {
        // 到点:档位复位,返回 true 让调用方停止播放并深睡。
        app->sleep_step = 0;
        return true;
    }
    return false;
}

uint16_t radio_app_sleep_left(const radio_app_t *app) {
    return app == NULL ? 0 : app->sleep_left_s;
}

void radio_app_sleep_format(const radio_app_t *app, char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (app == NULL || app->sleep_left_s == 0) return;
    // mm:ss。最长 90 分钟 = 5400 秒 = "90:00",24 字节足够。
    const unsigned total = app->sleep_left_s;
    snprintf(out, cap, "%u:%02u", total / 60u, total % 60u);
}

// 把某一页的游标夹进 [0, count-1],并让滚动窗口包住游标。
static void clamp_page(radio_app_t *app, radio_page_t page) {
    const int count = app->count[page];
    int visible = app->visible[page];
    if (visible < 1) visible = 1;

    if (count <= 0) {
        app->sel[page] = 0;
        app->scroll[page] = 0;
        return;
    }
    if (visible > count) visible = count;

    int sel = clamp_int(app->sel[page], 0, count - 1);
    int scroll = app->scroll[page];
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    if (scroll > count - visible) scroll = count - visible;
    if (scroll < 0) scroll = 0;

    app->sel[page] = (int16_t)sel;
    app->scroll[page] = (int16_t)scroll;
}

void radio_app_clamp(radio_app_t *app) {
    if (app == NULL) return;
    clamp_page(app, app->page);
}

void radio_app_set_row_count(radio_app_t *app, radio_page_t page, int count) {
    if (app == NULL || page >= RADIO_PAGE_COUNT) return;
    if (count < 0) count = 0;
    app->count[page] = (int16_t)count;
    clamp_page(app, page);
}

void radio_app_set_visible_rows(radio_app_t *app, radio_page_t page, int visible) {
    if (app == NULL || page >= RADIO_PAGE_COUNT) return;
    if (visible < 1) visible = 1;
    app->visible[page] = (int16_t)visible;
    clamp_page(app, page);
}

int radio_app_sel(const radio_app_t *app) {
    if (app == NULL) return 0;
    return app->sel[app->page];
}

int radio_app_scroll(const radio_app_t *app) {
    if (app == NULL) return 0;
    return app->scroll[app->page];
}

int radio_app_count(const radio_app_t *app, radio_page_t page) {
    if (app == NULL || page >= RADIO_PAGE_COUNT) return 0;
    return app->count[page];
}

void radio_app_init(radio_app_t *app, uint8_t volume, uint8_t brightness,
                    uint8_t auto_off_step, uint8_t region) {
    if (app == NULL) return;
    memset(app, 0, sizeof(*app));
    app->page = RADIO_PAGE_LIST;
    app->region = region < RADIO_REGION_COUNT ? region : 0;
    app->volume = (uint8_t)clamp_int(volume, 0, 100);
    app->brightness = (uint8_t)clamp_int(brightness, 0, 100);
    app->auto_off_step = (uint8_t)clamp_int(auto_off_step, 0, RADIO_AUTO_OFF_STEPS - 1);
    // 睡眠定时不跨重启保留:开机时总是"未启用",避免用户下次开机莫名被深睡。
    app->sleep_step = 0;
    app->sleep_left_s = 0;
    // 各页默认行数:菜单和设置是固定项数,列表由界面层随后写入真实值。
    app->count[RADIO_PAGE_MENU] = RADIO_MENU_ROW_COUNT;
    app->count[RADIO_PAGE_SETTINGS] = RADIO_SET_ROW_COUNT;
    app->count[RADIO_PAGE_REGION] = RADIO_REGION_COUNT;
    for (int i = 0; i < RADIO_PAGE_COUNT; i++) {
        app->visible[i] = 1;
        clamp_page(app, (radio_page_t)i);
    }
}

// 打开子页。特例:从主菜单进入时"替换"菜单而不是压栈,这样子页返回一步就回到列表,
// 不会出现"列表 → 菜单 → 设置 → 返回 → 菜单 → 返回 → 列表"这种多余的一跳。
static void page_open(radio_app_t *app, radio_page_t page) {
    if (app->page == RADIO_PAGE_MENU) {
        app->page = page;
    } else {
        if (app->stack_depth < (uint8_t)(sizeof(app->stack) / sizeof(app->stack[0]))) {
            app->stack[app->stack_depth++] = app->page;
        }
        app->page = page;
    }
    clamp_page(app, app->page);
    app->adjust = 0;
}

static void page_close(radio_app_t *app) {
    if (app->stack_depth > 0) {
        app->page = app->stack[--app->stack_depth];
    } else {
        // 根列表页没有可返回的上一层;长按确定在列表页的语义是"打开菜单"。
        app->page = RADIO_PAGE_LIST;
    }
    clamp_page(app, app->page);
    app->adjust = 0;
}

static radio_action_t act(radio_act_kind_t kind, int32_t value) {
    radio_action_t a = {.kind = kind, .value = value};
    return a;
}

static radio_action_t move_selection(radio_app_t *app, int delta) {
    app->sel[app->page] = (int16_t)(app->sel[app->page] + delta);
    clamp_page(app, app->page);
    app->sync_scroll = true;
    return act(RADIO_ACT_REDRAW, 0);
}

static radio_action_t adjust_setting(radio_app_t *app, int delta, bool large) {
    const int row = app->sel[RADIO_PAGE_SETTINGS];
    switch (row) {
    case RADIO_SET_ROW_VOLUME: {
        // 长按给更粗的步进,便于快速从 0 拉到 100。
        const int step = large ? 20 : 5;
        app->volume = (uint8_t)clamp_int(app->volume + delta * step, 0, 100);
        return act(RADIO_ACT_VOLUME, app->volume);
    }
    case RADIO_SET_ROW_BRIGHTNESS: {
        const int step = large ? 25 : 10;
        // 亮度下限留 10%,全黑会让用户以为设备关机。
        app->brightness = (uint8_t)clamp_int(app->brightness + delta * step, 10, 100);
        return act(RADIO_ACT_BRIGHTNESS, app->brightness);
    }
    case RADIO_SET_ROW_AUTO_OFF:
        app->auto_off_step = (uint8_t)clamp_int(
            app->auto_off_step + delta, 0, RADIO_AUTO_OFF_STEPS - 1);
        app->sync_scroll = true;
        return act(RADIO_ACT_SETTINGS_CHANGED, app->auto_off_step);
    case RADIO_SET_ROW_SLEEP: {
        // 长按按 2 档跳,方便从"关"快速到 30/60 分钟。
        const int step = (int)clamp_int(app->sleep_step + delta * (large ? 2 : 1),
                                        0, RADIO_SLEEP_STEPS - 1);
        radio_app_sleep_set(app, (uint8_t)step);
        app->sync_scroll = true;
        return act(RADIO_ACT_SLEEP_SET, step);
    }
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_list(radio_app_t *app, radio_key_t key) {
    // 列表页与收藏页共用同一套按键逻辑,区别只在"长按确定"的去向:
    // 根列表页打开主菜单,收藏页则是普通子页,长按确定返回上一层。
    const bool is_root = (app->page == RADIO_PAGE_LIST);
    const radio_page_t from = app->page;

    switch (key) {
    case RADIO_KEY_UP:
        return move_selection(app, -1);
    case RADIO_KEY_DOWN:
        return move_selection(app, 1);
    case RADIO_KEY_UP_LONG:
        // 列表页长按上/下直接换地区,省去"菜单 → 切换地区"两步。
        app->region = (uint8_t)((app->region + RADIO_REGION_COUNT - 1) % RADIO_REGION_COUNT);
        return act(RADIO_ACT_REGION_SET, app->region);
    case RADIO_KEY_DOWN_LONG:
        app->region = (uint8_t)((app->region + 1) % RADIO_REGION_COUNT);
        return act(RADIO_ACT_REGION_SET, app->region);
    case RADIO_KEY_OK:
        if (app->count[from] <= 0) return act(RADIO_ACT_NONE, 0);
        // 播放从列表发起:压入播放页,并把发起页的行号交给播放器。
        // 必须先取行号再切页,否则读到的是播放页自己的游标。
        {
            const int row = app->sel[from];
            page_open(app, RADIO_PAGE_NOW);
            return act(RADIO_ACT_PLAY_INDEX, row);
        }
    case RADIO_KEY_OK_LONG:
        if (!is_root) {
            page_close(app);
            return act(RADIO_ACT_CLOSE_PAGE, app->page);
        }
        // 根页面长按确定 = 打开主菜单(不压栈,菜单里返回一步即回列表)。
        app->page = RADIO_PAGE_MENU;
        clamp_page(app, app->page);
        return act(RADIO_ACT_OPEN_PAGE, RADIO_PAGE_MENU);
    case RADIO_KEY_OK_DOUBLE:
        if (app->count[from] <= 0) return act(RADIO_ACT_NONE, 0);
        return act(RADIO_ACT_FAVORITE_TOGGLE, app->sel[from]);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_now(radio_app_t *app, radio_key_t key) {
    switch (key) {
    case RADIO_KEY_UP:
    case RADIO_KEY_UP_LONG:
        app->volume = (uint8_t)clamp_int(app->volume + (key == RADIO_KEY_UP_LONG ? 20 : 5), 0, 100);
        return act(RADIO_ACT_VOLUME, app->volume);
    case RADIO_KEY_DOWN:
    case RADIO_KEY_DOWN_LONG:
        app->volume = (uint8_t)clamp_int(app->volume - (key == RADIO_KEY_DOWN_LONG ? 20 : 5), 0, 100);
        return act(RADIO_ACT_VOLUME, app->volume);
    case RADIO_KEY_OK:
        return act(RADIO_ACT_TOGGLE_PLAY, 0);
    case RADIO_KEY_OK_LONG:
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    case RADIO_KEY_OK_DOUBLE:
        return act(RADIO_ACT_NEXT_STATION, 0);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_menu(radio_app_t *app, radio_key_t key) {
    switch (key) {
    case RADIO_KEY_UP:
        return move_selection(app, -1);
    case RADIO_KEY_DOWN:
        return move_selection(app, 1);
    case RADIO_KEY_OK:
        switch (app->sel[RADIO_PAGE_MENU]) {
        case RADIO_MENU_ROW_REGION: page_open(app, RADIO_PAGE_REGION); break;
        case RADIO_MENU_ROW_FAVORITES: page_open(app, RADIO_PAGE_FAVORITES); break;
        case RADIO_MENU_ROW_SETTINGS: page_open(app, RADIO_PAGE_SETTINGS); break;
        case RADIO_MENU_ROW_WIFI: page_open(app, RADIO_PAGE_WIFI); break;
        case RADIO_MENU_ROW_ABOUT: page_open(app, RADIO_PAGE_ABOUT); break;
        default: return act(RADIO_ACT_NONE, 0);
        }
        return act(RADIO_ACT_OPEN_PAGE, app->page);
    case RADIO_KEY_OK_LONG:
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_region(radio_app_t *app, radio_key_t key) {
    switch (key) {
    case RADIO_KEY_UP:
        return move_selection(app, -1);
    case RADIO_KEY_DOWN:
        return move_selection(app, 1);
    case RADIO_KEY_OK: {
        app->region = (uint8_t)clamp_int(app->sel[RADIO_PAGE_REGION], 0, RADIO_REGION_COUNT - 1);
        // 选完地区直接回到电台列表:栈清空,不带用户绕回菜单。
        app->stack_depth = 0;
        app->page = RADIO_PAGE_LIST;
        app->adjust = 0;
        clamp_page(app, app->page);
        return act(RADIO_ACT_REGION_SET, app->region);
    }
    case RADIO_KEY_OK_LONG:
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_settings(radio_app_t *app, radio_key_t key) {
    const int row = app->sel[RADIO_PAGE_SETTINGS];

    if (app->adjust) {
        switch (key) {
        case RADIO_KEY_UP: return adjust_setting(app, +1, false);
        case RADIO_KEY_DOWN: return adjust_setting(app, -1, false);
        case RADIO_KEY_UP_LONG: return adjust_setting(app, +1, true);
        case RADIO_KEY_DOWN_LONG: return adjust_setting(app, -1, true);
        case RADIO_KEY_OK:
        case RADIO_KEY_OK_LONG:
            // 退出调节模式。确定键在这里的含义是"改完了",不是返回上一层,
            // 否则用户每改一次音量就要重新进一次设置页。
            app->adjust = 0;
            return act(RADIO_ACT_REDRAW, 0);
        default:
            return act(RADIO_ACT_NONE, 0);
        }
    }

    switch (key) {
    case RADIO_KEY_UP:
        return move_selection(app, -1);
    case RADIO_KEY_DOWN:
        return move_selection(app, 1);
    case RADIO_KEY_OK:
        if (radio_set_row_is_adjustable(row)) {
            app->adjust = 1;
            return act(RADIO_ACT_REDRAW, 0);
        }
        if (row == RADIO_SET_ROW_WIFI) {
            page_open(app, RADIO_PAGE_WIFI);
            return act(RADIO_ACT_OPEN_PAGE, RADIO_PAGE_WIFI);
        }
        if (row == RADIO_SET_ROW_ABOUT) {
            page_open(app, RADIO_PAGE_ABOUT);
            return act(RADIO_ACT_OPEN_PAGE, RADIO_PAGE_ABOUT);
        }
        return act(RADIO_ACT_NONE, 0);
    case RADIO_KEY_OK_LONG:
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_wifi(radio_app_t *app, radio_key_t key) {
    switch (key) {
    case RADIO_KEY_UP:
        return move_selection(app, -1);
    case RADIO_KEY_DOWN:
        return move_selection(app, 1);
    case RADIO_KEY_OK:
        // 选中网络后由界面层决定是直接连(已存密码)还是弹软键盘输密码。
        return act(RADIO_ACT_WIFI_ACTION, RADIO_WIFI_ACT_CONNECT);
    case RADIO_KEY_OK_LONG:
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    case RADIO_KEY_OK_DOUBLE:
        // 双击确定 = 用已保存凭据重新连接,不必重新输密码。
        return act(RADIO_ACT_WIFI_ACTION, RADIO_WIFI_ACT_RESTART);
    case RADIO_KEY_DOWN_LONG:
        return act(RADIO_ACT_WIFI_ACTION, RADIO_WIFI_ACT_START_SCAN);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

static radio_action_t handle_about(radio_app_t *app, radio_key_t key) {
    if (key == RADIO_KEY_OK_LONG) {
        page_close(app);
        return act(RADIO_ACT_CLOSE_PAGE, app->page);
    }
    return act(RADIO_ACT_NONE, 0);
}

radio_action_t radio_app_key(radio_app_t *app, radio_key_t key) {
    if (app == NULL) return act(RADIO_ACT_NONE, 0);
    app->sync_scroll = false;
    switch (app->page) {
    case RADIO_PAGE_LIST:
    case RADIO_PAGE_FAVORITES:
        return handle_list(app, key);
    case RADIO_PAGE_NOW:
        return handle_now(app, key);
    case RADIO_PAGE_MENU:
        return handle_menu(app, key);
    case RADIO_PAGE_REGION:
        return handle_region(app, key);
    case RADIO_PAGE_SETTINGS:
        return handle_settings(app, key);
    case RADIO_PAGE_WIFI:
        return handle_wifi(app, key);
    case RADIO_PAGE_ABOUT:
        return handle_about(app, key);
    default:
        return act(RADIO_ACT_NONE, 0);
    }
}

radio_action_t radio_app_last_extra(const radio_app_t *app) {
    (void)app;
    // 目前每个按键最多只需要一个副作用;界面层按返回的动作重画当前页即可。
    // 保留这个入口是为了后续加入"播放 + 立刻重画"这类组合时不必改公开接口。
    return act(RADIO_ACT_NONE, 0);
}
