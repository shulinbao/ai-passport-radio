// main/radio_theme.h —— 深色主题的唯一定义处。
//
// 界面全部走这里取色,不在各个绘制函数里散落十六进制字面量:换配色只需要改这一处,
// 也避免同一层级在不同页面出现两种相近但不同的灰。
//
// 选色依据(240x320、RGB565、户外/室内混合使用):
//   * 底色接近纯黑,深色背景在 OLED 之外的 TFT 上也更省电、更不刺眼;
//   * 正文不用纯白(0xFFFFFF),降低长时间阅读的眩光;
//   * 强调色用青绿,与"信号/正在播放"的语义绑定,在深底上对比度足够;
//   * 危险/错误用暖红,和强调色在色相上拉开,色觉差异用户也能区分。
#pragma once

#define RADIO_COLOR_BG        0x0B0F14   // 页面底色
#define RADIO_COLOR_SURFACE   0x141A21   // 卡片/列表行
#define RADIO_COLOR_SURFACE_2 0x1E2735   // 选中行、键盘按键
#define RADIO_COLOR_LINE      0x2A3644   // 分隔线、边框
#define RADIO_COLOR_TEXT      0xE6EDF3   // 主文字
#define RADIO_COLOR_MUTED     0x93A1B0   // 次要文字、提示
#define RADIO_COLOR_ACCENT    0x2DD4BF   // 强调:正在播放、已连接
#define RADIO_COLOR_WARN      0xF5A524   // 警告:缓冲中、弱信号
#define RADIO_COLOR_DANGER    0xF87171   // 错误:播放失败、未连接
#define RADIO_COLOR_BATTERY   0x9AE6B4   // 电量图标
