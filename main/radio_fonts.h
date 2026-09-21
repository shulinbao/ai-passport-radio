// main/radio_fonts.h —— 应用字体的声明与初始化。
//
// 背景:LVGL 基线只启用 Montserrat 14/20,它们【不含任何中文字形】。UTF-8 源码、
// 编译通过、串口日志正常,都不能证明屏幕上能显示中文 —— 必须真的把带中文字形的
// 字体编进固件并且挂到具体控件上。
//
// 本应用使用两个自生成的子集(见 assets/fonts/ 与 tools/gen_fonts.py):
//   radio_font_cjk_16 —— 正文/列表/键盘标题,覆盖 ASCII + GB2312 全量汉字 + 常用
//                        标点 + 本应用界面与电台名里出现的繁体字;
//   radio_font_cjk_20 —— 标题,只覆盖界面实际用到的字符(体积小)。
//
// 挂载方式:把生成的只读字体描述符复制一份到可写描述符,再设 fallback 指向
// Montserrat。这样中文字形走我们的子集,图标(LV_SYMBOL_*)和缺失的西文走
// Montserrat,而不是把 const 描述符强转掉(那是未定义行为)。
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// 必须在 LVGL 初始化之后、创建任何标签之前调用一次。
void radio_fonts_init(void);

// 返回可直接赋给 lv_obj_set_style_text_font() 的字体指针。
// 注意命名:生成的字体符号本身就叫 radio_font_cjk_16,所以访问函数必须换名字,
// 否则会和字体数据符号撞名(编译期 "redeclared as different kind of symbol")。
const lv_font_t *radio_font_body(void);

// 按内容选字体。
//
// 为什么要这么做:界面文案已经全部英文化,而 Noto Sans SC 派生出来的中文字体
// 【行高特别大】(16 px 字号的行高是 31 px,20 px 字号是 38 px,因为 CJK 字体
// 的 ascent/descent 本身就很宽),用它排满屏英文会非常浪费纵向空间,也会让
// 排版极难对齐。所以:
//   * 纯 ASCII 文本 → 用调用方给的西文字体(Montserrat,行高 16/22);
//   * 含非 ASCII 字符(中文 SSID、中文曲目名等)→ 退回带中文字形的字体,
//     保证"万一出现中文"不会变成一排方框。
// 标签高度请用 lv_font_get_line_height(font) 取,不要写死,否则会溢出。
const lv_font_t *radio_font_for_text(const char *text, const lv_font_t *latin);

// text 是否含有非 ASCII 字节(即需要 CJK 字体)。
bool radio_text_needs_cjk(const char *text);

// 判断某个字体(含 fallback 链)是否真的覆盖该码点。
// is_placeholder 为真表示"缺字形占位框",不算覆盖。
bool radio_font_has_glyph(const lv_font_t *font, uint32_t codepoint);

// 判断一段 UTF-8 文本是否全部落在该字体的覆盖范围内。
// 用于运行时自检:界面上的动态文本(SSID、ICY 标题)不在覆盖范围内时,
// 宁可主动降级显示,也不要让屏幕上出现一排方框。
bool radio_font_covers_utf8(const lv_font_t *font, const char *utf8);
