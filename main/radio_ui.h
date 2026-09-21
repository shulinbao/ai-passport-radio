// main/radio_ui.h —— 深色界面渲染层。
//
// 分层:main.c 负责"状态与数据",本文件只负责"把状态画出来"。
// 因此界面不直接读电台表、收藏、扫描结果,而是每帧通过 rows_fn 回调取当前页要显示的
// 行。这样界面不会和目录/网络模块互相依赖,渲染函数也能专注在排版与配色上。
//
// 所有函数都必须在 LVGL 任务里调用,或在外部持 bsp_lvgl_lock() 的情况下调用。
#pragma once

#include "esp_err.h"
#include "radio_app.h"

#include <stdbool.h>

// 列表类页面一屏能显示的最大行数。
// 一行放"台名 + 说明"两行西文:22 + 16 = 38 px,行框 47 px,行距 49 px,
// 5 行 = 245 px,落在内容区(250 px)内。宁可少一行也要让行高舒服 ——
// 这是真机上"两行字挤在一起"之后定下来的。
#define RADIO_UI_MAX_ROWS 5

// 设置页一屏显示的行数。设置行比列表行矮(只有"名称 + 数值"两列、单行文字),
// 所以 6 行能一次放完,不需要滚动 —— 设置项藏起来不给用户看见是最糟的设计。
#define RADIO_UI_SET_ROWS RADIO_SET_ROW_COUNT

typedef struct {
    const char *title;    // 主文本,必填
    const char *sub;      // 次文本,可为 NULL
    const char *badge;    // 右侧徽标(频率/信号/▶),可为 NULL
    bool accent;          // 用强调色绘制(正在播放的那一行)
} radio_ui_row_t;

// 取当前页面第 index 行要显示的内容。返回 false 表示该行是空的。
typedef bool (*radio_ui_rows_fn)(int index, radio_ui_row_t *out, void *user);

// 建屏与控件并加载。必须在 bsp_display_init()/bsp_lvgl_init() 成功之后调用。
esp_err_t radio_ui_init(void);

// 按状态重画当前页。rows_fn 可为 NULL(非列表页面不需要)。
void radio_ui_render(const radio_app_t *app, radio_ui_rows_fn rows_fn, void *user);

// 关于页要显示的版本/说明文本,由 main.c 提供(避免界面层硬编码构建信息)。
void radio_ui_set_about_text(const char *text);

// 软键盘开关。键盘存在时它会盖住整屏,按键全部交给它。
bool radio_ui_keyboard_open(const char *title, const char *subtitle,
                            const char *initial,
                            void (*on_done)(const char *text, void *user),
                            void (*on_cancel)(void *user), void *user);
void radio_ui_keyboard_close(void);
bool radio_ui_keyboard_active(void);
void radio_ui_keyboard_key(radio_key_t key);
