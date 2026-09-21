// main/main.c —— 「港 · 新 · 马 网络电台」应用入口。
//
// 分层:
//   radio_ui.c        只负责画(纯渲染,通过回调取当前页要显示的行)
//   radio_app.c       三按键界面状态机(纯逻辑,有主机测试)
//   radio_catalog.c   内置精选电台目录
//   radio_player.c    HTTP 流 -> 解码 -> I2S
//   radio_wifi.c      扫描 / 连接 / 凭据持久化
//   main.c            把上面几层接起来:按键分发、动作执行、周期刷新
//
// 按键语义(与 radio_app.h 的约定一致):
//   上/下 短按  移动选中项;播放页调音量;设置页改数值
//   上/下 长按  电台列表页直接切换地区(港/新/马)
//   确定  短按  播放 / 进入子页 / 设置页进入调节模式 / 播放页暂停
//   确定  长按  返回上一层;电台列表页打开菜单
//   确定  双击  列表页收藏或取消收藏;播放页切下一个台
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "radio_app.h"
#include "radio_catalog.h"
#include "radio_favorites.h"
#include "radio_player.h"
#include "radio_store.h"
#include "radio_ui.h"
#include "radio_wifi.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "radio";

#define INPUT_QUEUE_DEPTH 8
// 周期刷新:播放页要看得见电平条和曲目变化,刷快一点;其他页面 1 秒足够,
// 少刷一次就少一次整屏 SPI 传输,对这块电池设备是实打实的省电。
#define TICK_PERIOD_MS 250
#define NOW_RENDER_EVERY 2      // 500ms
#define IDLE_RENDER_EVERY 4     // 1000ms

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static radio_app_t s_app;
static radio_store_t s_store;
static const radio_station_t *s_playing;   // 指向目录里的静态条目,生命周期与固件一致

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static esp_timer_handle_t s_tick_timer;
static uint32_t s_tick_count;
static uint32_t s_idle_seconds;
static bool s_backlight_on = true;
// 待输入的 Wi-Fi 目标:弹软键盘时记住是哪张网,提交回调里要用。
static char s_pending_ssid[RADIO_WIFI_SSID_MAX];

// 电源任务与周期刷新之间的握手。
static TaskHandle_t s_power_task;
static volatile bool s_sleep_now;       // 睡眠定时到点,由电源任务执行深睡
static uint8_t s_resume_left;           // 开机续播:还需要等多少秒连网(0 = 不等了)
static uint8_t s_playing_flag;          // 上次写进 NVS 的"正在放音"标志

// ---------------------------------------------------------------------------
// 关于页文本
// ---------------------------------------------------------------------------
static void about_text_build(void) {
    char text[224];
    snprintf(text, sizeof(text),
             "Hong Kong / Singapore / Malaysia radio\n"
             "Built %s %s\n"
             "%d stations - up to %d favorites\n"
             "Audio: MP3 and AAC\n"
             "Wi-Fi setup runs on the device\n"
             "hold OK = back",
             __DATE__, __TIME__, radio_catalog_total(), RADIO_FAVORITES_MAX);
    radio_ui_set_about_text(text);
}

// ---------------------------------------------------------------------------
// 目录 / 列表解析
// ---------------------------------------------------------------------------
static const radio_station_t *favorite_station(int row) {
    if (row < 0 || row >= radio_favorites_count(&s_store.favorites)) return NULL;
    return radio_catalog_by_id(radio_favorites_at(&s_store.favorites, row));
}

static const radio_station_t *station_at(radio_page_t page, int row) {
    switch (page) {
    case RADIO_PAGE_LIST:
        return radio_catalog_region_at(s_app.region, row);
    case RADIO_PAGE_FAVORITES:
        return favorite_station(row);
    case RADIO_PAGE_NOW:
        return s_playing;
    default:
        return NULL;
    }
}

// 列表页里的行号 -> 电台。播放页需要知道"是哪个列表把它打开的",
// 页面栈顶就是那个来源页。
static radio_page_t play_source_page(void) {
    if (s_app.page != RADIO_PAGE_NOW) return s_app.page;
    if (s_app.stack_depth > 0) return s_app.stack[s_app.stack_depth - 1];
    return RADIO_PAGE_LIST;
}

static void refresh_counts(void) {
    radio_app_set_row_count(&s_app, RADIO_PAGE_LIST,
                            radio_catalog_region_count(s_app.region));
    radio_app_set_row_count(&s_app, RADIO_PAGE_FAVORITES,
                            radio_favorites_count(&s_store.favorites));
    radio_app_set_row_count(&s_app, RADIO_PAGE_MENU, RADIO_MENU_ROW_COUNT);
    radio_app_set_row_count(&s_app, RADIO_PAGE_REGION, RADIO_REGION_COUNT);
    radio_app_set_row_count(&s_app, RADIO_PAGE_SETTINGS, RADIO_SET_ROW_COUNT);
    radio_app_set_row_count(&s_app, RADIO_PAGE_WIFI, radio_wifi_scan_count());
    for (int p = 0; p < RADIO_PAGE_COUNT; p++) {
        radio_app_set_visible_rows(&s_app, (radio_page_t)p, RADIO_UI_MAX_ROWS);
    }
    // 设置页的行框更矮,6 项一次全放得下,不需要滚动隐藏任何一项。
    radio_app_set_visible_rows(&s_app, RADIO_PAGE_SETTINGS, RADIO_UI_SET_ROWS);
}

static void format_frequency(uint16_t freq_deci, char *out, size_t cap) {
    if (freq_deci < 870 || freq_deci > 1080) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%u.%u", (unsigned)(freq_deci / 10), (unsigned)(freq_deci % 10));
}

static void format_signal(int8_t rssi, char *out, size_t cap) {
    // 用 Hi/Mid/Lo 代替 dBm 数字:列表里那串 "-67 dBm" 既占地方又不直观。
    const char *level = "Lo";
    if (rssi > -55) {
        level = "Hi";
    } else if (rssi > -70) {
        level = "Mid";
    }
    snprintf(out, cap, "%s", level);
}

// ---------------------------------------------------------------------------
// 行内容提供者
// ---------------------------------------------------------------------------
static bool rows_provider(int index, radio_ui_row_t *out, void *user) {
    (void)user;
    memset(out, 0, sizeof(*out));

    static char badge[24];       // 静态缓冲:界面在同一帧内即用即取
    static char sub[96];

    switch (s_app.page) {
    case RADIO_PAGE_LIST:
    case RADIO_PAGE_FAVORITES:
    case RADIO_PAGE_NOW: {
        const radio_station_t *st = station_at(s_app.page, index);
        if (st == NULL) return false;
        const bool fav = radio_favorites_contains(&s_store.favorites, st->id);
        out->title = st->name;
        if (fav) {
            // 已收藏的台加一个记号,避免用户反复双击却不知道有没有生效。
            snprintf(sub, sizeof(sub), "* %s", st->tagline != NULL ? st->tagline : "");
            out->sub = sub;
            out->accent = true;
        } else {
            out->sub = st->tagline;
            out->accent = (s_playing == st);
        }
        format_frequency(st->freq_deci, badge, sizeof(badge));
        out->badge = badge[0] != '\0' ? badge : NULL;
        return true;
    }
    case RADIO_PAGE_MENU: {
        static const char *const kTitles[RADIO_MENU_ROW_COUNT] = {
            "Region", "Favorites", "Settings", "Wi-Fi", "About",
        };
        static const char *const kSubs[RADIO_MENU_ROW_COUNT] = {
            "Hong Kong, Singapore, Malaysia",
            "Your saved stations",
            "Volume, brightness, auto off",
            "Scan and connect",
            "Build and details",
        };
        if (index < 0 || index >= RADIO_MENU_ROW_COUNT) return false;
        out->title = kTitles[index];
        out->sub = kSubs[index];
        return true;
    }
    case RADIO_PAGE_REGION: {
        if (index < 0 || index >= RADIO_REGION_COUNT) return false;
        out->title = radio_region_name((uint8_t)index);
        snprintf(sub, sizeof(sub), "%d stations",
                 radio_catalog_region_count((uint8_t)index));
        out->sub = sub;
        out->accent = ((uint8_t)index == s_app.region);
        return true;
    }
    case RADIO_PAGE_WIFI: {
        if (index < 0 || index >= radio_wifi_scan_count()) return false;
        const radio_wifi_ap_t *ap = radio_wifi_scan_at(index);
        if (ap == NULL) return false;
        out->title = ap->ssid;
        char saved[RADIO_WIFI_SSID_MAX];
        if (radio_wifi_saved_ssid(saved, sizeof(saved)) && strcmp(saved, ap->ssid) == 0) {
            out->sub = ap->secure ? "saved | password" : "saved | open";
            out->accent = true;
        } else {
            out->sub = ap->secure ? "password" : "open";
        }
        format_signal(ap->rssi, badge, sizeof(badge));
        out->badge = badge;
        return true;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// 动作执行
// ---------------------------------------------------------------------------
static void render_locked(void) {
    radio_ui_render(&s_app, rows_provider, NULL);
}

static void request_render(void) {
    if (!bsp_lvgl_lock(200)) return;
    render_locked();
    bsp_lvgl_unlock();
}

static void play_row(radio_page_t page, int row) {
    const radio_station_t *st = station_at(page, row);
    if (st == NULL) return;
    s_playing = st;
    s_app.playing_row = (uint8_t)row;
    s_app.playing = true;
    radio_player_play(st);
    (void)radio_store_set_str(RADIO_STORE_KEY_LAST_URL, st->url);
    ESP_LOGI(TAG, "播放 %s", st->name);
}

static void step_station(int delta) {
    const radio_page_t page = play_source_page();
    const int count = radio_app_count(&s_app, page);
    if (count <= 0) return;
    const int current = s_app.sel[page];
    const int next = ((current + delta) % count + count) % count;
    s_app.sel[page] = (int16_t)next;
    play_row(page, next);
}

static void keyboard_done(const char *text, void *user) {
    (void)user;
    radio_ui_keyboard_close();
    // 键盘回调运行在 LVGL 任务里;连接本身只是投递命令,不会卡住界面。
    if (s_pending_ssid[0] != '\0') {
        const esp_err_t err = radio_wifi_connect(s_pending_ssid, text, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "投递连接请求失败: %s", esp_err_to_name(err));
        }
    }
    s_pending_ssid[0] = '\0';
    request_render();
}

static void keyboard_cancel(void *user) {
    (void)user;
    radio_ui_keyboard_close();
    s_pending_ssid[0] = '\0';
    request_render();
}

static void handle_wifi_connect(void) {
    const int row = s_app.sel[RADIO_PAGE_WIFI];
    if (row < 0 || row >= radio_wifi_scan_count()) return;
    const radio_wifi_ap_t *ap = radio_wifi_scan_at(row);
    if (ap == NULL) return;

    char saved_ssid[RADIO_WIFI_SSID_MAX];
    const bool is_saved = radio_wifi_saved_ssid(saved_ssid, sizeof(saved_ssid)) &&
                          strcmp(saved_ssid, ap->ssid) == 0;

    if (is_saved) {
        char password[RADIO_WIFI_PASS_MAX];
        (void)radio_wifi_saved_password(password, sizeof(password));
        (void)radio_wifi_connect(ap->ssid, password, false);
        return;
    }
    if (!ap->secure) {
        (void)radio_wifi_connect(ap->ssid, "", true);
        return;
    }
    // 需要密码:弹软键盘。这是本应用唯一的文本输入场景。
    strncpy(s_pending_ssid, ap->ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    if (!radio_ui_keyboard_open("Wi-Fi password", ap->ssid, NULL,
                                keyboard_done, keyboard_cancel, NULL)) {
        ESP_LOGW(TAG, "软键盘打开失败");
        s_pending_ssid[0] = '\0';
    }
}

static void apply_action(radio_action_t action) {
    switch (action.kind) {
    case RADIO_ACT_PLAY_INDEX:
        play_row(play_source_page(), (int)action.value);
        break;
    case RADIO_ACT_TOGGLE_PLAY:
        radio_player_set_paused(!radio_player_paused());
        break;
    case RADIO_ACT_NEXT_STATION:
        step_station(1);
        break;
    case RADIO_ACT_VOLUME:
        radio_player_set_volume((uint8_t)action.value);
        (void)radio_store_set_u8(RADIO_STORE_KEY_VOLUME, (uint8_t)action.value);
        break;
    case RADIO_ACT_BRIGHTNESS:
        bsp_display_backlight((uint8_t)action.value);
        (void)radio_store_set_u8(RADIO_STORE_KEY_BRIGHT, (uint8_t)action.value);
        break;
    case RADIO_ACT_SETTINGS_CHANGED:
        (void)radio_store_set_u8(RADIO_STORE_KEY_AUTOOFF, (uint8_t)action.value);
        // 改完自动熄屏档位就重新计时,否则"刚从关改成 30 秒"会立刻熄屏。
        s_idle_seconds = 0;
        break;
    case RADIO_ACT_SLEEP_SET:
        // 睡眠定时不落盘:它是"今晚这一次"的临时设定,跨重启保留只会让用户
        // 下次开机莫名被深睡。这里只记日志,倒计时由状态机自己推进。
        ESP_LOGI(TAG, "睡眠定时已设为 %u 分钟", (unsigned)RADIO_SLEEP_MINUTES[action.value]);
        break;
    case RADIO_ACT_FAVORITE_TOGGLE: {
        const radio_station_t *st = station_at(s_app.page, (int)action.value);
        if (st == NULL) break;
        radio_favorites_toggle(&s_store.favorites, st->id);
        (void)radio_store_save_favorites(&s_store.favorites);
        refresh_counts();
        ESP_LOGI(TAG, "%s:%s",
                 radio_favorites_contains(&s_store.favorites, st->id) ? "已收藏" : "已取消",
                 st->name);
        break;
    }
    case RADIO_ACT_REGION_SET:
        s_app.region = (uint8_t)action.value;
        (void)radio_store_set_u8(RADIO_STORE_KEY_REGION, (uint8_t)action.value);
        refresh_counts();
        break;
    case RADIO_ACT_WIFI_ACTION:
        switch ((radio_wifi_action_t)action.value) {
        case RADIO_WIFI_ACT_START_SCAN:
            (void)radio_wifi_scan_start();
            break;
        case RADIO_WIFI_ACT_CONNECT:
            handle_wifi_connect();
            break;
        case RADIO_WIFI_ACT_RESTART:
            (void)radio_wifi_autoconnect();
            break;
        case RADIO_WIFI_ACT_FORGET:
            (void)radio_wifi_forget();
            break;
        default:
            break;
        }
        refresh_counts();
        break;
    case RADIO_ACT_OPEN_PAGE:
        if ((radio_page_t)action.value == RADIO_PAGE_WIFI) {
            // 进 Wi-Fi 页就顺手扫一次,用户不必再按一次。
            (void)radio_wifi_scan_start();
            refresh_counts();
        }
        break;
    case RADIO_ACT_NONE:
    case RADIO_ACT_REDRAW:
    case RADIO_ACT_CLOSE_PAGE:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 按键分发
// ---------------------------------------------------------------------------
static void dispatch_key(radio_key_t key) {
    // 任何按键都算"人在用",重置自动熄屏计时。
    s_idle_seconds = 0;
    if (!s_backlight_on) {
        // 熄屏后的第一下只负责唤醒,不执行功能 —— 否则用户会在黑屏状态下
        // 误触到别的菜单。
        bsp_display_backlight(s_app.brightness);
        s_backlight_on = true;
        request_render();
        return;
    }

    // 整个按键处理过程都必须持 LVGL 锁:软键盘的创建、销毁和绘制都直接操作
    // LVGL 对象树。这把锁是递归锁,内部再锁一次是安全的。
    //
    // ⚠⚠ 这段代码踩过两次同一个坑,不要再动:
    //   1) 早先只给"重画"加了锁,键盘路径没加 —— 真机表现是选中 Wi-Fi 弹键盘时白屏。
    //   2) 补锁之后【漏删了上面那段"键盘已激活就先处理"的早返回】,于是打字期间
    //      每一次按键仍然走的是【没拿锁】的那条路(键盘打开时它必然先命中)。
    //      真机表现极具误导性:输入法"能用"(改标签、切选中只碰少量对象,大多时候
    //      侥幸不炸),但一按确定就白屏 —— 因为确定会 lv_obj_delete 掉整个键盘面板,
    //      与 LVGL 任务并行改对象树,之后渲染到已释放的对象(实拍崩在
    //      lv_draw_sw_mask.c 的画线/清零里,坐标是垃圾值,极难从崩点反推)。
    //   结论:键盘按键必须和普通按键走同一条"先加锁再处理"的路径。
    if (!bsp_lvgl_lock(300)) return;

    if (radio_ui_keyboard_active()) {
        radio_ui_keyboard_key(key);
        bsp_lvgl_unlock();
        return;
    }

    const radio_action_t action = radio_app_key(&s_app, key);
    apply_action(action);
    render_locked();
    bsp_lvgl_unlock();
}

static radio_key_t map_button(bsp_btn_t btn, bsp_btn_ev_t event, bool *mapped) {
    *mapped = true;
    switch (btn) {
    case BSP_BTN_UP:
        if (event == BSP_BTN_CLICK) return RADIO_KEY_UP;
        if (event == BSP_BTN_LONG) return RADIO_KEY_UP_LONG;
        break;
    case BSP_BTN_DOWN:
        if (event == BSP_BTN_CLICK) return RADIO_KEY_DOWN;
        if (event == BSP_BTN_LONG) return RADIO_KEY_DOWN_LONG;
        break;
    case BSP_BTN_OK:
        if (event == BSP_BTN_CLICK) return RADIO_KEY_OK;
        if (event == BSP_BTN_LONG) return RADIO_KEY_OK_LONG;
        if (event == BSP_BTN_DOUBLE) return RADIO_KEY_OK_DOUBLE;
        break;
    default:
        break;
    }
    // PRESS(按下瞬间)在本应用里没有用途:它会在长按确认时先触发一次,
    // 导致"长按返回"顺手把当前项也执行了。
    *mapped = false;
    return RADIO_KEY_OK;
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        bool mapped = false;
        const radio_key_t key = map_button(input.btn, input.event, &mapped);
        if (!mapped) continue;
        dispatch_key(key);
    }
}

// 按键回调运行在共享的 esp_timer 任务上:只入队后立即返回,绝不在这里碰 LVGL。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) return;
    const input_event_t input = {.btn = btn, .event = event};
    (void)xQueueSend(s_input_queue, &input, 0);
}

// ---------------------------------------------------------------------------
// 周期刷新
// ---------------------------------------------------------------------------
// tick_cb 要先用到它,定义在电源那一节(见下)。
static void sync_playing_flag(void);

static void tick_cb(void *arg) {
    (void)arg;
    s_tick_count++;
    // ⚠ 这里绝对不要写 `s_idle_seconds += TICK_PERIOD_MS / 1000;`:
    // 250 / 1000 在整数除法下等于 0,计时器永远不增长,自动熄屏就永远不会触发。
    // (真机上就是这么坏的:设置里能选档位,但屏幕永远不熄。)
    // 正确做法是按"每 4 个 tick(1000ms/250ms)算一秒"来累加。
    if ((s_tick_count % (1000 / TICK_PERIOD_MS)) == 0) {
        s_idle_seconds++;
        radio_wifi_tick();
        sync_playing_flag();
        // 睡眠定时倒计时放在状态机里(主机可测):到点只做握手,真正的深睡序列
        // 交给电源任务执行。
        if (radio_app_sleep_tick(&s_app)) {
            s_sleep_now = true;
            if (s_power_task != NULL) xTaskNotifyGive(s_power_task);
        }
    }

    // 自动熄屏:只在真的设置了档位时动手,并且不改音量、不中断播放。
    const uint16_t auto_off = RADIO_AUTO_OFF_SECONDS[s_app.auto_off_step];
    if (auto_off > 0 && s_backlight_on && s_idle_seconds >= auto_off) {
        bsp_display_backlight(0);
        s_backlight_on = false;
        ESP_LOGI(TAG, "空闲 %u 秒,关闭背光(播放不受影响)", (unsigned)auto_off);
        return;
    }
    if (!s_backlight_on) return;

    if (!bsp_lvgl_lock(100)) return;
    if (s_app.page == RADIO_PAGE_WIFI) {
        // 扫描结果是异步到的,行数要跟着更新。
        radio_app_set_row_count(&s_app, RADIO_PAGE_WIFI, radio_wifi_scan_count());
    }
    const bool now = (s_app.page == RADIO_PAGE_NOW);
    const uint32_t every = now ? NOW_RENDER_EVERY : IDLE_RENDER_EVERY;
    if ((s_tick_count % every) == 0) {
        render_locked();
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 电源:睡眠定时到点 -> 停止播放并进入深度睡眠
//
// 为什么单独一个任务:深睡序列会停 I2S、释放 I2C 引脚、关掉 LCD 并拉高阻,
// 之后还要 esp_deep_sleep_start()。这些都不能在 esp_timer 任务(上面压着别的
// 周期回调)或 LVGL 任务里做。顺序完全照 demo_low_power.c 的验证过的路径:
// CW2017 与 ES8311 共用 I2C,必须先做完电量计的 suspend 再做 codec 的。
// ---------------------------------------------------------------------------
static void enter_deep_sleep(void) {
    ESP_LOGI(TAG, "睡眠定时到点:停止播放并进入深度睡眠");

    radio_player_stop();
    // 早上重新开机时不要自己响:清掉"上次在放音"标志(恢复电台仍然保留)。
    (void)radio_store_set_u8(RADIO_STORE_KEY_WAS_PLAYING, 0);
    s_playing_flag = 0;
    s_backlight_on = false;
    // 给播放任务一点时间从 I2S 写路径里退出,避免深睡时还有半帧在 DMA 里。
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGW(TAG, "CW2017 suspend: %s", esp_err_to_name(bsp_battery_sleep()));
    ESP_LOGW(TAG, "ES8311 suspend: %s", esp_err_to_name(bsp_audio_sleep()));
    // 即使 codec 寄存器操作失败也继续:停时钟、释放引脚的收益远大于风险。
    ESP_LOGW(TAG, "I2S pin release: %s", esp_err_to_name(bsp_audio_prepare_deep_sleep()));
    ESP_LOGW(TAG, "shared I2C pin release: %s", esp_err_to_name(bsp_i2c_prepare_deep_sleep()));

    // 持锁等到当前 flush 结束,之后 LVGL 不会再往已关闭的 LCD 上刷屏。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "深睡前无法停止 LVGL 刷屏,重启恢复外设");
        esp_restart();
    }
    ESP_LOGW(TAG, "ST7789 suspend: %s", esp_err_to_name(bsp_display_prepare_deep_sleep()));

    // ⚠ 不注册任何唤醒源:睡眠定时的语义就是"关掉",靠机身电源键/复位键重新开机。
    // 若这里改成定时唤醒,设备会在半夜自己亮屏重启,那才是真的扰人。
    esp_deep_sleep_start();
    // 从深睡准备接口返回后总线已不可在本次运行中恢复,只能重启。
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启恢复外设");
    esp_restart();
}

// 开机续播:等 Wi-Fi 连上再放,否则第一轮 HTTP 必然失败并把播放器打到错误态。
static void resume_tick(void) {
    if (s_resume_left == 0) return;
    if (radio_wifi_state() == RADIO_WIFI_STATE_CONNECTED) {
        s_resume_left = 0;
        if (s_playing != NULL) {
            s_app.playing = true;
            radio_player_play(s_playing);
            ESP_LOGI(TAG, "开机续播:%s", s_playing->name);
        }
        return;
    }
    if (--s_resume_left == 0) {
        ESP_LOGW(TAG, "开机续播等待 Wi-Fi 超时,已停在列表页");
    }
}

static void power_task(void *arg) {
    (void)arg;
    for (;;) {
        // 超时 1 秒 = 顺手当作续播的节拍;睡眠定时到点时会立刻被通知唤醒。
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_sleep_now) {
            enter_deep_sleep();     // 不返回
        }
        resume_tick();
    }
}

// "上次在放音"只在真正播出声音时才记:连续失败的电台不该在下次开机时被自动重放。
// "上次在放音"只在真正播出声音时才记:连续失败的电台不该在下次开机时被自动重放。
static void sync_playing_flag(void) {
    const uint8_t now = (radio_player_state() == RADIO_PLAYER_PLAYING &&
                         !radio_player_paused()) ? 1 : 0;
    if (now == s_playing_flag) return;
    s_playing_flag = now;
    (void)radio_store_set_u8(RADIO_STORE_KEY_WAS_PLAYING, now);
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
static esp_err_t input_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) return ESP_ERR_NO_MEM;
    // 按键任务不只是"转发事件":弹出软键盘时它要在这里创建几十个 LVGL 对象并
    // 逐个设置样式。4KB 栈在真机上偏紧(创建对象 + 样式分配的调用链不浅),
    // 这里加到 6KB —— 栈是堆上分配的,这个代价很小。
    if (xTaskCreate(input_task, "radio_input", 6144, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void start_tick_timer(void) {
    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "radio_tick",
    };
    if (esp_timer_create(&args, &s_tick_timer) != ESP_OK) {
        ESP_LOGE(TAG, "定时器创建失败,界面将不会自动刷新");
        return;
    }
    (void)esp_timer_start_periodic(s_tick_timer, TICK_PERIOD_MS * 1000);
}

// 开机恢复上次播放的电台:把地区切到那个台所在的地区,并把光标停在该行。
// 只恢复"选中项",不在这里放音 —— 放音要等 Wi-Fi 连上,见 resume_tick()。
static void restore_last_station(void) {
    const radio_station_t *last =
        (s_store.last_url[0] != '\0') ? radio_catalog_by_url(s_store.last_url) : NULL;
    if (last == NULL) return;

    s_app.region = last->region;
    s_playing = last;              // 播放页因此也有内容可显示(状态行会写 Stopped)
    refresh_counts();

    // 按 id 反查该台在地区列表里的行号。不比较指针:目录是静态表,但 id 才是
    // 收藏、恢复这类持久化场景里稳定的标识。
    const int count = radio_catalog_region_count(last->region);
    for (int i = 0; i < count; i++) {
        const radio_station_t *cand = radio_catalog_region_at(last->region, i);
        if (cand != NULL && cand->id == last->id) {
            s_app.sel[RADIO_PAGE_LIST] = (int16_t)i;
            radio_app_clamp(&s_app);
            break;
        }
    }
    ESP_LOGI(TAG, "恢复上次电台: %s (%s)", last->name, radio_region_name(last->region));

    if (s_store.was_playing) {
        // 20 秒足够 DHCP 完成;超时就停在列表页,用户按一下确定即可播放。
        s_resume_left = 20;
        ESP_LOGI(TAG, "上次退出时在放音,连上 Wi-Fi 后自动续播");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "港·新·马 网络电台 启动");

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    radio_store_load(&s_store);
    about_text_build();
    s_playing_flag = s_store.was_playing;   // 首轮同步不要重复写同一个值

    radio_app_init(&s_app, s_store.volume, s_store.brightness,
                   s_store.auto_off_step, s_store.region);
    s_playing = NULL;

    bsp_display_backlight(s_store.brightness);
    (void)bsp_battery_init();

    const esp_err_t wifi_err = radio_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(wifi_err));
    }
    const esp_err_t player_err = radio_player_init();
    if (player_err != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败: %s", esp_err_to_name(player_err));
    }
    radio_player_set_volume(s_store.volume);

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "无法获取 LVGL 锁");
        return;
    }
    if (radio_ui_init() != ESP_OK) {
        bsp_lvgl_unlock();
        ESP_LOGE(TAG, "界面初始化失败");
        return;
    }
    refresh_counts();

    // 没有保存过 Wi-Fi 就直接落在配网页:这台设备第一次开机必须能配网,
    // 否则用户会停在空列表前不知道要按什么。
    if (wifi_err == ESP_OK && !radio_wifi_has_credentials()) {
        s_app.page = RADIO_PAGE_WIFI;
        (void)radio_wifi_scan_start();
        refresh_counts();
        ESP_LOGI(TAG, "尚未配置 Wi-Fi,已进入配网页");
    } else {
        // 有凭据时才恢复上次电台:否则光标会停在"配网页背后的列表"上,用户看到的是
        // 一个连不上网却高亮着某个台的界面。
        restore_last_station();
    }
    render_locked();
    bsp_lvgl_unlock();

    const esp_err_t input_err = input_init();
    const esp_err_t button_err = (input_err == ESP_OK)
                                     ? bsp_button_init(on_key, NULL)
                                     : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK || button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    if (wifi_err == ESP_OK && radio_wifi_has_credentials()) {
        (void)radio_wifi_autoconnect();
    }

    start_tick_timer();
    if (xTaskCreate(power_task, "radio_power", 5120, NULL, 4, &s_power_task) != pdPASS) {
        // 没这个任务只是睡眠定时不可用,其余功能照常,所以只告警不返回。
        ESP_LOGW(TAG, "电源任务创建失败:睡眠定时将不会生效");
    }
    s_input_ready = true;

    ESP_LOGI(TAG, "就绪:电台 %d 个,地区 %s,空闲堆 %u 字节",
             radio_catalog_total(), radio_region_name(s_app.region),
             (unsigned)esp_get_free_heap_size());
}
