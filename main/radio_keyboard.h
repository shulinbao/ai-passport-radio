// main/radio_keyboard.h —— 屏幕软键盘(设备端配网输入 Wi-Fi 密码用)。
//
// 为什么需要它:这台设备只有"上/下/确定"三颗按键,没有触屏。用户选了要连的
// Wi-Fi 之后,必须在设备上把密码敲进去。市面上的做法通常是"开热点让手机配网",
// 但本项目按用户选择走纯设备端配网,所以键盘是这个功能能不能用的关键。
//
// 导航设计(只有两个方向键,必须能把 54 个键都走到):
//   上/下 短按   在按键表里前后移动一格(按阅读顺序线性排列)
//   上/下 长按   跳过一整行(本列上下移动),用来快速跨越八行字符
//   确定  短按   输入当前键
//   确定  长按   取消(与本应用"长按确定 = 返回"的全局约定一致)
//   确定  双击   直接提交
//
// 只支持 ASCII 可见字符:Wi-Fi 密码绝大多数是 ASCII,而中文字库不该为了键盘
// 再放大一轮。这一点会写在交付说明的"未覆盖"里。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RADIO_KB_KEY_UP = 0,
    RADIO_KB_KEY_DOWN,
    RADIO_KB_KEY_UP_LONG,
    RADIO_KB_KEY_DOWN_LONG,
    RADIO_KB_KEY_OK,
    RADIO_KB_KEY_OK_LONG,
    RADIO_KB_KEY_OK_DOUBLE,
} radio_kb_key_t;

#define RADIO_KB_TEXT_MAX 65   // Wi-Fi 密码最长 64 字节 + NUL

typedef void (*radio_kb_done_cb_t)(const char *text, void *user);
typedef void (*radio_kb_cancel_cb_t)(void *user);

typedef struct radio_keyboard radio_keyboard_t;

// 在 parent 上创建键盘面板。initial 可为 NULL(空密码)。
// 返回 NULL 表示内存不足。同一时刻只允许一个键盘实例。
radio_keyboard_t *radio_keyboard_create(void *parent, const char *title,
                                        const char *subtitle,
                                        const char *initial,
                                        radio_kb_done_cb_t on_done,
                                        radio_kb_cancel_cb_t on_cancel,
                                        void *user);

// 销毁键盘(必须先于其父对象销毁)。
void radio_keyboard_destroy(radio_keyboard_t *kb);

// 处理一次按键,返回 true 表示该键已被键盘消费。
bool radio_keyboard_key(radio_keyboard_t *kb, radio_kb_key_t key);

const char *radio_keyboard_text(const radio_keyboard_t *kb);
bool radio_keyboard_shifted(const radio_keyboard_t *kb);

// 供测试使用:公开按键表,便于校验布局与线性导航。
int radio_keyboard_key_count(void);
// 取第 index 个键上显示的文本;越界返回 ""。
const char *radio_keyboard_key_label(int index, bool shifted);
