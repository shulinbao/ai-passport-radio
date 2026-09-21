// main/radio_keyboard_layout.h —— 软键盘布局与导航(纯逻辑,可主机测试)。
//
// 把"54 个键怎么排、按一下上/下走到哪"从 LVGL 绘制里拆出来,是为了能在主机上
// 直接把边界情况跑一遍:从第一行按上、从最后一行按下、长按跳行时列号怎么处理。
// 这些算错了在设备上表现为"某个字符永远选不到",很难靠肉眼发现。
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RADIO_KB_GLYPH = 0,   // 普通字符键
    RADIO_KB_SPACE,       // 空格
    RADIO_KB_BACKSPACE,   // 退格
    RADIO_KB_CLEAR,       // 清空
    RADIO_KB_SHIFT,       // 大小写切换
    RADIO_KB_SUBMIT,      // 提交(确定双击)
    RADIO_KB_CANCEL,      // 取消(确定长按)
} radio_kb_glyph_kind_t;

// 按键总数与列数(行数为 count/columns)。
int radio_kb_layout_count(void);
int radio_kb_layout_columns(void);

// 越界返回 RADIO_KB_CANCEL(视为无效即可)。
radio_kb_glyph_kind_t radio_kb_layout_kind(int index);

// 按键表里第一个指定类型的键的下标;没有则返回 -1。
// 有了它,"双击确定 = 提交"这类语义就不必依赖"最后一格是什么"的隐含约定 ——
// 之前正是把这个约定写错,双击确定变成了取消。
int radio_kb_layout_find(radio_kb_glyph_kind_t kind);

// 键面文字。字符键按 shifted 返回大写或小写;功能键返回固定标签。
// 写入失败或越界时 out 为空串。
void radio_kb_layout_label(int index, bool shifted, char *out, size_t cap);

// 短按:在按键表里线性前后移动(按阅读顺序),夹紧在两端。
int radio_kb_layout_move(int index, int delta);

// 长按:上下跳一整行,保持列号;越界时夹紧到最近的有效键。
int radio_kb_layout_row_move(int index, int delta);
