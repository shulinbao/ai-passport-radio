// main/radio_app.h —— 应用界面状态机(纯逻辑,不依赖 ESP-IDF / LVGL)。
//
// 为什么把它单独拆出来:界面上"按一下上键到底发生什么"这件事,在只有三颗按键的
// 设备上很容易出岔子 —— 选中项越界、滚动窗口没跟手、调节模式下按键语义冲突、
// 返回上一层后光标位置丢了。这些都不需要真机就能验,所以状态机与绘制彻底分开:
// main/radio_app.c 只做决策,radio_ui.c 只负责把决策画出来。
//
// 按键语义(全局约定):
//   上/下 短按   列表:移动选中项;调节模式:改数值;播放页:调音量
//   上/下 长按   列表页:直接切换地区(港/新/马)
//   确定  短按   列表:播放选中台 / 进入子页;设置:进入或退出调节模式;播放页:暂停
//   确定  长按   返回上一层(根列表页则打开菜单)
//   确定  双击   列表:收藏/取消收藏该台;播放页:下一个台
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 地区数量:香港 / 新加坡 / 马来西亚。
#define RADIO_REGION_COUNT 3

typedef enum {
    RADIO_REGION_HK = 0,
    RADIO_REGION_SG = 1,
    RADIO_REGION_MY = 2,
} radio_region_t;

typedef enum {
    RADIO_PAGE_LIST = 0,      // 当前地区的电台列表(根页面)
    RADIO_PAGE_FAVORITES,     // 我的收藏
    RADIO_PAGE_NOW,           // 正在播放
    RADIO_PAGE_MENU,          // 主菜单
    RADIO_PAGE_REGION,        // 切换地区
    RADIO_PAGE_SETTINGS,      // 设置
    RADIO_PAGE_WIFI,          // Wi-Fi 网络
    RADIO_PAGE_ABOUT,         // 关于
    RADIO_PAGE_COUNT,
} radio_page_t;

typedef enum {
    RADIO_KEY_UP = 0,
    RADIO_KEY_DOWN,
    RADIO_KEY_OK,
    RADIO_KEY_OK_LONG,
    RADIO_KEY_OK_DOUBLE,
    RADIO_KEY_UP_LONG,
    RADIO_KEY_DOWN_LONG,
} radio_key_t;

typedef enum {
    RADIO_ACT_NONE = 0,
    RADIO_ACT_REDRAW,          // 选中项/滚动窗口/数值变化,重画当前页
    RADIO_ACT_OPEN_PAGE,       // value = radio_page_t
    RADIO_ACT_CLOSE_PAGE,      // 返回上一层
    RADIO_ACT_PLAY_INDEX,      // value = 当前可见列表中的行号,开始播放
    RADIO_ACT_TOGGLE_PLAY,     // 暂停/继续
    RADIO_ACT_NEXT_STATION,    // 切到下一个台
    RADIO_ACT_VOLUME,          // value = 新音量(0..100)
    RADIO_ACT_BRIGHTNESS,      // value = 新亮度(0..100)
    RADIO_ACT_FAVORITE_TOGGLE, // value = 当前可见列表中的行号
    RADIO_ACT_REGION_SET,      // value = radio_region_t
    RADIO_ACT_WIFI_ACTION,     // value = radio_wifi_action_t(见下)
    RADIO_ACT_SETTINGS_CHANGED,// 数值类设置已改动,需要落盘
    RADIO_ACT_SLEEP_SET,       // value = 睡眠定时档位(不落盘:倒计时不该跨重启保留)
} radio_act_kind_t;

// RADIO_ACT_WIFI_ACTION 的取值。
typedef enum {
    RADIO_WIFI_ACT_NONE = 0,
    RADIO_WIFI_ACT_START_SCAN,
    RADIO_WIFI_ACT_CONNECT,     // 连接当前选中的网络(密码由界面层提供)
    RADIO_WIFI_ACT_FORGET,      // 清除已保存的凭据
    RADIO_WIFI_ACT_RESTART,     // 用已保存凭据重连
} radio_wifi_action_t;

// 设置页的行。可调项在前,可进项在后。
typedef enum {
    RADIO_SET_ROW_VOLUME = 0,
    RADIO_SET_ROW_BRIGHTNESS,
    RADIO_SET_ROW_AUTO_OFF,
    RADIO_SET_ROW_SLEEP,
    RADIO_SET_ROW_WIFI,
    RADIO_SET_ROW_ABOUT,
    RADIO_SET_ROW_COUNT,
} radio_set_row_t;

// 主菜单的行。
typedef enum {
    RADIO_MENU_ROW_REGION = 0,
    RADIO_MENU_ROW_FAVORITES,
    RADIO_MENU_ROW_SETTINGS,
    RADIO_MENU_ROW_WIFI,
    RADIO_MENU_ROW_ABOUT,
    RADIO_MENU_ROW_COUNT,
} radio_menu_row_t;

// 自动熄屏档位(秒);0 表示不熄屏。
// 档位给得细一点:sleep 前听电台时"多久熄屏"是很个人的偏好,
// 只有 关/30秒/1分/3分 四档时,15 秒和 2、5、10 分钟这些常见诉求都落不进去。
#define RADIO_AUTO_OFF_STEPS 7
extern const uint16_t RADIO_AUTO_OFF_SECONDS[RADIO_AUTO_OFF_STEPS];

// 睡眠定时档位(分钟);0 表示不启用。到点停止播放并进入深度睡眠。
#define RADIO_SLEEP_STEPS 5
extern const uint16_t RADIO_SLEEP_MINUTES[RADIO_SLEEP_STEPS];

typedef struct {
    radio_page_t page;
    // 页面栈:RADIO_PAGE_LIST 是栈底,子页压栈,长按确定出栈。
    radio_page_t stack[8];
    uint8_t stack_depth;

    uint8_t region;

    // 当前页的列表游标。切换页面时按页保存/恢复,返回后光标停在原处。
    int16_t sel[RADIO_PAGE_COUNT];
    int16_t scroll[RADIO_PAGE_COUNT];
    int16_t count[RADIO_PAGE_COUNT];   // 由界面层在内容变化时写入
    int16_t visible[RADIO_PAGE_COUNT]; // 可见行数(列表高度),由界面层写入

    // 设置页状态
    uint8_t adjust;                    // 1 = 处于数值调节模式
    uint8_t volume;                    // 0..100
    uint8_t brightness;                // 0..100
    uint8_t auto_off_step;             // 0..RADIO_AUTO_OFF_STEPS-1
    uint8_t sleep_step;                // 0..RADIO_SLEEP_STEPS-1;0 = 不启用
    uint16_t sleep_left_s;             // 睡眠定时剩余秒数;0 = 未在倒计时

    // 播放状态(只读镜像,由播放器写入)
    bool playing;
    uint8_t playing_row;               // 当前播放项在可见列表中的行号
    bool sync_scroll;                  // 请求界面把选中项滚入视野
} radio_app_t;

typedef struct {
    radio_act_kind_t kind;
    int32_t value;
} radio_action_t;

// 返回值约定:同一按键最多产生一个动作;需要重画时返回 RADIO_ACT_REDRAW。
// 需要"重画 + 播放"这类组合时,界面层按 REDRAW 处理完再处理第二个动作 —— 为此
// 提供 radio_app_last_extra(),存放可选的附加动作。
void radio_app_init(radio_app_t *app, uint8_t volume, uint8_t brightness,
                    uint8_t auto_off_step, uint8_t region);

// 界面层在列表内容变化后调用,更新行数与可见行数,并夹紧游标。
void radio_app_set_row_count(radio_app_t *app, radio_page_t page, int count);
void radio_app_set_visible_rows(radio_app_t *app, radio_page_t page, int visible);

// 处理一次按键。返回主动作;附加动作见 radio_app_last_extra()。
radio_action_t radio_app_key(radio_app_t *app, radio_key_t key);
radio_action_t radio_app_last_extra(const radio_app_t *app);

// 当前页的游标/滚动窗口/行数(供界面层与测试读取)。
int radio_app_sel(const radio_app_t *app);
int radio_app_scroll(const radio_app_t *app);
int radio_app_count(const radio_app_t *app, radio_page_t page);

// 把游标夹到 [0, count-1],并让滚动窗口跟着游标走。内容变化后由界面层调用。
void radio_app_clamp(radio_app_t *app);

// 设置页:当前行的数值文本与是否可调。
bool radio_set_row_is_adjustable(int row);
int radio_set_row_value(const radio_app_t *app, int row);
void radio_set_row_format(const radio_app_t *app, int row, char *out, size_t cap);

// ---------------------------------------------------------------------------
// 睡眠定时:到点停止播放并进入深度睡眠。
// 倒计时放在状态机里(而不是 main.c 的定时器里),这样"多少分钟、什么时候到点、
// 到点后状态怎么变"都能在主机上直接测,不必真机等 15 分钟。
// ---------------------------------------------------------------------------
// 选择档位(0 = 关闭)。选择非 0 档位时立即开始倒计时。
void radio_app_sleep_set(radio_app_t *app, uint8_t step);
// 每秒调用一次。返回 true 表示"到点了",调用方应停止播放并进入深睡。
bool radio_app_sleep_tick(radio_app_t *app);
// 剩余秒数;0 表示没有在倒计时。
uint16_t radio_app_sleep_left(const radio_app_t *app);
// 睡眠定时剩余时间的显示文本(如 "12:34");未启用时为空串。
void radio_app_sleep_format(const radio_app_t *app, char *out, size_t cap);

// 当前选中的地区;供界面层显示。
const char *radio_region_name(uint8_t region);
