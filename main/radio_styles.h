// main/radio_styles.h —— 共享的 LVGL 样式对象。
//
// 为什么必须有这个模块(真机上付过代价):
//
// LVGL 里每一次 lv_obj_set_style_xxx(obj, ...) 都会在该对象上【新建/扩展一份本地
// 样式属性】。软键盘有 54 个按键,每个键设置 8 个样式属性,于是光是键盘就要吃掉
// 十几 KB 的 LVGL 内存池;实测打开键盘后池子用量从 19,340 涨到 37,244 字节,
// 最大空闲块从 25,580 掉到 4,856,碎片 31%。随后任何一次稍大的分配都会失败返回
// NULL,表现为 Load access fault(寄存器 A0=0)+ 栈被写坏 —— 也就是用户看到的白屏。
//
// 正确做法是共享样式:把"卡片长什么样""按键长什么样"各做成一个 lv_style_t,
// 所有同类对象只持有它的指针。选中态用"再叠一层样式"实现(后加的样式优先级更高),
// 于是每次换选中项只需要对一个对象 add/remove 一次样式,而不是重设一堆属性。
//
// 这些样式必须在创建任何控件之前初始化,由 radio_ui_init() 调用。
#pragma once

#include "lvgl.h"

// 幂等;重复调用无副作用。
void radio_styles_init(void);

// 卡片/列表行的常态外观(背景色 + 圆角 + 不透明)。
lv_style_t *radio_style_card(void);
// 叠在常态之上表示"当前选中":更亮的底色 + 强调色描边。
lv_style_t *radio_style_card_selected(void);
// 叠在最上层表示"正在调节这个数值":警告色描边。
// 必须是可叠加/可移除的样式 —— 早先用 lv_obj_set_style_border_* 直接设一次就再没
// 复位,于是退出调节模式后黄框一直留在那一行上(真机反馈)。
lv_style_t *radio_style_card_adjusting(void);

// 软键盘按键的常态外观(键帽底色 + 圆角 + 文字居中 + 内边距)。
lv_style_t *radio_style_key(void);
// 叠在常态之上表示"当前选中":强调色底 + 反色文字。
lv_style_t *radio_style_key_selected(void);
