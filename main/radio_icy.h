// main/radio_icy.h —— ICY (Shoutcast/Icecast) 流内元数据解复用器。
//
// 背景:很多网络电台在音频字节之间周期性插入一小段元数据块,块里通常是
// "StreamTitle='...';" 形式的正在播放信息。服务器通过响应头 icy-metaint 告诉
// 客户端"每 N 个音频字节后面跟一个元数据块"。客户端必须先按这个偏移把两类
// 字节分开,再把纯音频字节交给解码器 —— 否则元数据会被当成音频解码,轻则
// 解出噪声,重则解码器报错复位。
//
// 本模块是【纯逻辑】,不依赖 ESP-IDF / LVGL,因此可以在主机上直接跑单元测试
// (见 tests/test_radio_icy.c)。播放任务只需逐字节调用 radio_icy_consume()。
//
// 线程模型:所有函数都非阻塞、无锁。约定由播放任务独占一个实例,界面线程通过
// radio_icy_snapshot_title() 读取标题,读取时持锁由调用方负责(见 radio_stream.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 标题缓冲长度。StreamTitle 实际很少超过 128 字节;留 160 给 UTF-8 中文台名/歌名。
#define RADIO_ICY_TITLE_CAP 160
// 单个元数据块的字节数上限。协议里长度字段是"16 字节为单位"的 8 位值,
// 理论最大 255*16=4080;这里按实际服务器行为给一个更紧的可控上限。
#define RADIO_ICY_BLOCK_CAP 1024

typedef enum {
    RADIO_ICY_AUDIO = 0,   // 该字节属于音频流,应交给解码器
    RADIO_ICY_META,        // 该字节属于元数据块,已被本模块吞掉
} radio_icy_kind_t;

typedef struct {
    // ---- 配置 ----
    size_t metaint;        // 0 = 服务器未启用元数据,consume 永远返回 AUDIO
    // ---- 运行时状态 ----
    size_t audio_seen;     // 距下一个元数据块还剩/已消费的音频字节数
    uint8_t block_len;     // 当前元数据块声明长度(未乘 16);0 表示"空块"
    uint16_t block_pos;    // 已收到的块内字节数
    uint16_t block_fill;   // 已写入 block[] 的字节数(超过上限后停止写入)
    bool in_block;         // 正在读取"长度字节"
    bool truncated;        // 块长超过 RADIO_ICY_BLOCK_CAP 时置位,块尾被丢弃
    char title[RADIO_ICY_TITLE_CAP];  // 最近一次解析出的 StreamTitle
    uint32_t title_seq;    // 标题版本号,每次内容变化 +1,便于 UI 只刷新变化的文本
    // 元数据块原始内容。协议上限 255*16=4080,实际服务器多用 100~300,
    // 这里截到 RADIO_ICY_BLOCK_CAP;超出部分只计数不保存(truncated 置位)。
    char block[RADIO_ICY_BLOCK_CAP];
} radio_icy_t;

// 初始化。metaint 为 0 时整个解析被旁路(返回的 kind 恒为 AUDIO)。
void radio_icy_init(radio_icy_t *icy, size_t metaint);

// 消费 1 个字节,返回它属于音频还是元数据。
// 调用方按返回值决定是否把该字节写进解码输入缓冲。
radio_icy_kind_t radio_icy_consume(radio_icy_t *icy, uint8_t byte);

// 最近解析出的标题;没有标题时返回空串(而不是 NULL)。
const char *radio_icy_title(const radio_icy_t *icy);

// 从 "StreamTitle='...';StreamUrl='...';" 这类块内容里提取标题。
// 单独暴露出来是为了能直接对它做单元测试。
// 返回 true 表示找到了一个非空标题并写入了 out。
bool radio_icy_parse_block(const char *block, size_t len, char *out, size_t out_cap);
