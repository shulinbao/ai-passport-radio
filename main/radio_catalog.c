// main/radio_catalog.c —— 内置精选电台目录(香港 / 新加坡 / 马来西亚)。
//
// 数据来源与验证方法(交付说明里也会写):
//   每一个 url 都在本机实测过 —— HTTP 200,并且在按 ICY 元数据解复用之后,
//   能连续找到 >= 5 个落在"按帧长推算出的下一个偏移"上的合法帧头
//   (MP3 帧同步 0xFFEx / AAC-ADTS 同步 0xFFFx)。只返回 200 但其实是 HTML、
//   m3u8 播放列表的地址一律不收录 —— 那种地址在设备上表现为"连上了但没声音"。
//
// 关于频率(界面右侧的小字):
//   只填【有把握】的。freq_deci 为 0 表示"这条流没有可以标在刻度上的 FM 频率",
//   界面会留空。台名本身带频率的(Class 95 / Gold 905 / BFM 89.9 ...)按台名填写;
//   RTHK、RTM 这类一台多频或 AM 播出的台一律留 0,而不是编一个看起来合理的数字。
//
// 关于编码:
//   MP3 走 esp_audio_codec 的 MP3 解码器;AAC 走它的 AAC 解码器。
//   马来西亚 Astro 系电台(含用户点名的 Mix FM / Lite FM)只有 AAC 流,
//   新加坡 Mediacorp 与马来西亚 RTM 都提供 MP3,香港 RTHK 六个频道全部是 MP3。
//
// 界面语言:台名保留当地原文(英文台名不翻译),tagline 用英文短句,
//   长度控制在列表行宽内,不会因为换行把行高撑破。
#include "radio_catalog.h"

#include <string.h>

// 表内顺序即界面显示顺序:每区内部按"用户更可能常听"排,而不是按字母。
static const radio_station_t kStations[] = {
    // ---------------------------------------------------------------------
    // 香港 —— 香港电台 RTHK 六个频道。
    // 实测:MPEG-2 Layer III,32 kbps,22050 Hz,立体声,Icecast,无 ICY 元数据,
    // 且流的开头落在帧中间(偏移 6~103 字节,每次不同),播放器必须先扫描帧头。
    // ---------------------------------------------------------------------
    {  1, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK Radio 1", "Cantonese news/talk",
       "http://stm1.rthk.hk/radio1" },
    {  2, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK Radio 2", "Cantonese lifestyle",
       "http://stm2.rthk.hk/radio2" },
    {  3, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK Radio 3", "English news/talk",
       "http://stm3.rthk.hk/radio3" },
    {  4, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK Radio 4", "Classical music",
       "http://stm1.rthk.hk/radio4" },
    {  5, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK Radio 5", "Cantonese opera",
       "http://stm1.rthk.hk/radio5" },
    {  6, RADIO_REGION_HK, RADIO_CODEC_MP3, 0,
       "RTHK PTH", "Mandarin general",
       "http://stm1.rthk.hk/radiopth" },

    // ---------------------------------------------------------------------
    // 新加坡 —— Mediacorp 全系,统一使用 StreamTheWorld 的可移植跳转地址。
    // 实测:MP3 44100 Hz 立体声 96 kbps(88.3JIA 为 48000 Hz / 128 kbps),
    // 流从头就是帧边界。
    // 用 playerservices 跳转形式而不是带编号的边缘主机:编号主机是动态分配的,
    // 写死会在某天变成 404。
    //
    // 每条挂载名的具体写法(CLASS95.mp3 还是 MP3_CAPITAL958FM_SC)不是随手选的:
    // StreamTheWorld 对不同挂载名的跳转目标不一致,有少数挂载会 302 到明文
    // http:80。这里全部采用实测"最终跳转仍为 https"的那一个写法。
    // ---------------------------------------------------------------------
    {  7, RADIO_REGION_SG, RADIO_CODEC_MP3, 950,
       "Class 95", "English adult hits",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/CLASS95.mp3" },
    {  8, RADIO_REGION_SG, RADIO_CODEC_MP3, 905,
       "Gold 905", "English classics",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/GOLD905_SC" },
    {  9, RADIO_REGION_SG, RADIO_CODEC_MP3, 933,
       "YES 933", "Mandarin pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/YES933.mp3" },
    { 10, RADIO_REGION_SG, RADIO_CODEC_MP3, 958,
       "Capital 958", "Mandarin lifestyle",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/MP3_CAPITAL958FM_SC" },
    { 11, RADIO_REGION_SG, RADIO_CODEC_MP3, 987,
       "987FM", "English hits",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/MP3_987FM_SC" },
    { 12, RADIO_REGION_SG, RADIO_CODEC_MP3, 0,
       "CNA 938", "English news/talk",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/MP3_938NOW_SC" },
    { 13, RADIO_REGION_SG, RADIO_CODEC_MP3, 924,
       "Symphony 924", "Classical music",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/SYMPHONY924.mp3" },
    { 14, RADIO_REGION_SG, RADIO_CODEC_MP3, 942,
       "Warna 942", "Malay general",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/WARNA942FM_SC" },
    { 15, RADIO_REGION_SG, RADIO_CODEC_MP3, 968,
       "Oli 968", "Tamil general",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/OLI968FM_SC" },
    { 16, RADIO_REGION_SG, RADIO_CODEC_MP3, 972,
       "LOVE 972", "Mandarin classics",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/LOVE972FM_SC" },
    { 17, RADIO_REGION_SG, RADIO_CODEC_MP3, 920,
       "Kiss92", "English hits",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/KISS_92_SC" },
    { 18, RADIO_REGION_SG, RADIO_CODEC_MP3, 883,
       "88.3JIA", "Mandarin/Cantonese",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/883JIA_SC" },
    { 19, RADIO_REGION_SG, RADIO_CODEC_MP3, 1003,
       "UFM 100.3", "Mandarin pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/UFM_1003_SC" },
    { 20, RADIO_REGION_SG, RADIO_CODEC_MP3, 893,
       "Money FM 89.3", "Business and talk",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/MONEY_893_SC" },
    { 21, RADIO_REGION_SG, RADIO_CODEC_MP3, 897,
       "RIA 897", "Malay pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/RIA897FM.mp3" },
    { 22, RADIO_REGION_SG, RADIO_CODEC_MP3, 913,
       "ONE FM 91.3", "English hits",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/ONE_FM_913_SC" },
    { 23, RADIO_REGION_SG, RADIO_CODEC_MP3, 963,
       "Hao 96.3", "Mandarin pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/HAO_963_SC" },

    // ---------------------------------------------------------------------
    // 马来西亚 —— Astro 系(AAC/ADTS,经 Revma 分发)。
    // 实测:ADTS AAC-LC,核心采样率 22050 Hz(带隐式 SBR,解码后约 44100 Hz),
    // 立体声,~49 kbps。流的开头正好落在 ADTS 帧边界上。
    // 地址用不带令牌的 stream.rcs.revma.com/<id>:实测连续 3 轮重新请求都正常,
    // 每次 302 到不同的边缘主机并带上一次性 rj-tok,所以固件【不能】缓存跳转后
    // 的地址,必须每次连接都跟随 302。
    //
    // ⚠ 这里用 http:// 而不是 https:// 是【内存】决定的,不是随手写的:
    //   esp_audio_codec 的 AAC 解码器一次性要几十 KB 连续堆(HE-AAC 更多),本板
    //   没有 PSRAM。实测 https 时 esp_audio_simple_dec_open 返回 -2
    //   (ESP_AUDIO_ERR_MEM_LACK),真机现象就是"Mix FM connecting 然后 stopped"。
    //   Revma 的 80 端口返回同样的音频(实测 200 + audio/aac + icy-metaint=16000),
    //   省掉整个 TLS 会话的内存后才有可能把 AAC 解码器开起来。
    // ---------------------------------------------------------------------
    { 24, RADIO_REGION_MY, RADIO_CODEC_AAC, 945,
       "Mix FM", "English adult hits",
       "http://stream.rcs.revma.com/v5pq3htbv4uvv" },
    { 25, RADIO_REGION_MY, RADIO_CODEC_AAC, 1057,
       "Lite FM", "English easy listening",
       "http://stream.rcs.revma.com/bn4ex8sbv4uvv" },
    { 26, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Hitz FM", "English hits",
       "http://stream.rcs.revma.com/488kt4sbv4uvv" },
    { 27, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "MY FM", "Mandarin pop",
       "http://stream.rcs.revma.com/hc3unrtbv4uvv" },
    { 28, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Melody FM", "Mandarin classics",
       "http://stream.rcs.revma.com/2u1n6dtbv4uvv" },
    { 29, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "ERA FM", "Malay pop",
       "http://stream.rcs.revma.com/crec9cmbv4uvv" },
    { 30, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Sinar FM", "Malay classics",
       "http://stream.rcs.revma.com/azatk0tbv4uvv" },
    { 31, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "THR Raaga", "Tamil general",
       "http://stream.rcs.revma.com/1ut6qwtbv4uvv" },
    { 32, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Fly FM", "English hits",
       "http://stream.rcs.revma.com/q17aka9mtd3vv" },
    { 33, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Hot FM", "Malay pop",
       "http://stream.rcs.revma.com/drakdf8mtd3vv" },
    { 34, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Zayan", "Malay contemporary",
       "http://stream.rcs.revma.com/7ww2a4tbv4uvv" },
    { 35, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "GoXuan", "Mandarin youth",
       "http://stream.rcs.revma.com/xt9nhzsbv4uvv" },
    { 36, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Eight FM", "Mandarin general",
       "http://stream.rcs.revma.com/qp0xrd9mtd3vv" },
    { 37, RADIO_REGION_MY, RADIO_CODEC_AAC, 899,
       "BFM 89.9", "Business and talk",
       "http://stream.rcs.revma.com/s91qy9p0zs3vv" },
    { 38, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "CITYPlus FM", "City info",
       "http://stream.rcs.revma.com/9ykdmcawe1bwv" },
    { 39, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "Cats FM", "Sarawak local",
       "http://stream.rcs.revma.com/51qe53rv0y3vv" },
    { 40, RADIO_REGION_MY, RADIO_CODEC_AAC, 0,
       "THR Gegar", "East coast local",
       "http://stream.rcs.revma.com/cn0zcqsbv4uvv" },

    // ---------------------------------------------------------------------
    // 马来西亚 —— RTM 国营电台(MP3,经 StreamTheWorld)。
    // 实测:MP3 44100 Hz 立体声 96 kbps,流从头就是帧边界。
    // ---------------------------------------------------------------------
    { 41, RADIO_REGION_MY, RADIO_CODEC_MP3, 988,
       "988 FM", "Mandarin pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/988_FM_SC" },
    { 42, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Traxx FM", "English adult hits",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/TRAXX_FM.mp3" },
    { 43, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Ai FM", "Mandarin general",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/AI_FM.mp3" },
    { 44, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Nasional FM", "Malay general",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/NASIONAL_FM.mp3" },
    { 45, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Radio Klasik", "Malay classics",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/RADIO_KLASIK.mp3" },
    { 46, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Asyik FM", "Malay rural",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/ASYIK_FM.mp3" },
    { 47, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Minnal FM", "Tamil general",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/MINNAL_FM.mp3" },
    { 48, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Langkawi FM", "Northern local",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/LANGKAWI_FM.mp3" },
    { 49, RADIO_REGION_MY, RADIO_CODEC_MP3, 0,
       "Suria FM", "Malay pop",
       "https://playerservices.streamtheworld.com/api/livestream-redirect/SURIA_FM_SC" },
};

#define STATION_COUNT ((int)(sizeof(kStations) / sizeof(kStations[0])))

// 地区内序号 -> 全表下标。表很小(49 条),线性扫描比多维护一张索引表更不容易出错。
static int region_to_table(uint8_t region, int index) {
    if (index < 0) return -1;
    int seen = 0;
    for (int i = 0; i < STATION_COUNT; i++) {
        if (kStations[i].region != region) continue;
        if (seen == index) return i;
        seen++;
    }
    return -1;
}

int radio_catalog_total(void) {
    return STATION_COUNT;
}

const radio_station_t *radio_catalog_all_at(int index) {
    if (index < 0 || index >= STATION_COUNT) return NULL;
    return &kStations[index];
}

int radio_catalog_region_count(uint8_t region) {
    if (region >= RADIO_REGION_COUNT) return 0;
    int count = 0;
    for (int i = 0; i < STATION_COUNT; i++) {
        if (kStations[i].region == region) count++;
    }
    return count;
}

const radio_station_t *radio_catalog_region_at(uint8_t region, int index) {
    if (region >= RADIO_REGION_COUNT) return NULL;
    const int table = region_to_table(region, index);
    if (table < 0) return NULL;
    return &kStations[table];
}

const radio_station_t *radio_catalog_by_id(uint16_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < STATION_COUNT; i++) {
        if (kStations[i].id == id) return &kStations[i];
    }
    return NULL;
}

const radio_station_t *radio_catalog_by_url(const char *url) {
    if (url == NULL || url[0] == '\0') return NULL;
    for (int i = 0; i < STATION_COUNT; i++) {
        if (strcmp(kStations[i].url, url) == 0) return &kStations[i];
    }
    return NULL;
}

int radio_catalog_resolve_ids(const uint16_t *ids, int count,
                              const radio_station_t **out, int out_cap) {
    if (ids == NULL || out == NULL || count <= 0 || out_cap <= 0) return 0;
    int written = 0;
    for (int i = 0; i < count && written < out_cap; i++) {
        // 目录里可能已经没有这个 id(换过固件、某个台被移除),安静跳过,
        // 而不是在收藏列表里留一个空行。
        const radio_station_t *s = radio_catalog_by_id(ids[i]);
        if (s == NULL) continue;
        out[written++] = s;
    }
    return written;
}
