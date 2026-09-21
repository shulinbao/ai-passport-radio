// main/radio_player.h —— 网络电台播放器:HTTP 流 -> 解码 -> I2S。
//
// 职责边界:
//   * 只认"一条流地址 + 它的编码格式",不关心目录、收藏、地区 —— 那些在 radio_app
//     与 radio_catalog 里。这样播放器可以被单元测试之外的东西单独替换。
//   * 唯一的音频设备持有者。任何时刻最多一个播放任务在跑,换台时先把旧任务停掉
//     并等它退出,避免两个任务同时写 I2S。
//
// 线程模型:所有耗时动作(HTTP、解码、I2S 写入)都在内部播放任务里;对外接口只
// 改状态后返回,界面/按键回调不会被阻塞。
//
// 内存:本板无 PSRAM,所以输入缓冲与 PCM 缓冲都按"够解码器吃满一帧"来配,并在
// 日志里打印最小空闲堆与最大连续块,便于按证据调,而不是凭感觉放大。
#pragma once

#include "esp_err.h"
#include "radio_catalog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RADIO_PLAYER_STOPPED = 0,
    RADIO_PLAYER_CONNECTING,
    RADIO_PLAYER_BUFFERING,
    RADIO_PLAYER_PLAYING,
    RADIO_PLAYER_RECONNECTING,
    RADIO_PLAYER_ERROR,
} radio_player_state_t;

// 初始化音频 BSP 与解码器注册表。可重复调用。
esp_err_t radio_player_init(void);

// 开始播放。会复制 station 里的 url/codec,不持有调用方的指针。
// 传入 NULL 等价于 stop()。
void radio_player_play(const radio_station_t *station);
void radio_player_stop(void);

void radio_player_set_paused(bool paused);
bool radio_player_paused(void);

// 音量 0..100,直接作用到 ES8311。
void radio_player_set_volume(uint8_t volume);
uint8_t radio_player_volume(void);

radio_player_state_t radio_player_state(void);
// 状态对应的中文短语,直接给界面用。
const char *radio_player_state_text(void);

// ICY 标题快照(线程安全)。无标题时写入空串。
void radio_player_title(char *out, size_t cap);
// 标题版本号,变化即说明需要刷新界面。
uint32_t radio_player_title_seq(void);

// 当前响度 0..100,供界面画电平条;没有声音时为 0。
uint8_t radio_player_level(void);

// 流信息一行文本,例如 "MP3 128kbps 44.1kHz",供界面显示。
// 码率是【实测】的(按收到的音频字节数与耗时算出来):HE-AAC 的 ADTS 头与
// VBR MP3 的帧头都给不出可信码率,实测值反而更实在。还没解出格式时写入空串。
void radio_player_stream_info(char *out, size_t cap);

// 最近一次失败的原因短语(中文),供界面提示。
const char *radio_player_last_error(void);
