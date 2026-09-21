// main/radio_player.c —— 网络电台播放器实现。
#include "radio_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_frame.h"
#include "radio_icy.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp:HTTP 头名比较必须大小写无关

static const char *TAG = "radio_player";

// 输入缓冲:够装下 1~2 个压缩帧即可。esp_audio_simple_dec 内部会缓存半截帧,
// 所以不需要按"整帧"对齐;2KB 对 128kbps 的流约等于 0.13 秒。
// ⚠ 这个值直接决定 AAC 台能不能播:AAC 解码器要 55.8KB 连续堆,留给缓冲的余量
// 越少越好(真机上 4KB+8KB 时就是"解码器开起来了但缓冲分配失败")。
#define STREAM_IN_BYTES 2048
// PCM 输出缓冲:必须能装下【一帧的最大解码输出】,按最坏的那一种算:
//   MP3  MPEG-1 Layer III 每帧 1152 采样/声道,立体声 16bit = 1152*2*2 = 4608
//   AAC-LC 每帧 1024 采样/声道,立体声 16bit = 1024*2*2  = 4096
// 取 5120(=4608 向上留一档)。再大就是白占内存:实测最坏情况就是上面的 4608。
#define STREAM_OUT_BYTES 5120
// 播放任务栈。链路较深(HTTP -> TLS -> 解码器 -> I2S),8KB 是安全起点。
#define PLAYER_TASK_STACK 8192
#define PLAYER_TASK_PRIO 5

#define MAX_EMPTY_READS 3
#define RECONNECT_DELAY_MS 1200
#define RETRY_DELAY_MS 2500
#define MAX_CONSECUTIVE_FAILURES 3

/**
 * 一次播放请求的不可变快照。
 *
 * 播放任务绝不能直接读 s_player.url:换台时界面线程正在改写它,读到"新 url +
 * 旧 generation"会让播放任务用错的地址去连、而且认为请求还有效。因此每次进入
 * 连接循环前,在锁内把 url/name/codec 整体复制出来,之后整轮只用快照。
 */
typedef struct {
    char url[256];
    char name[64];
    uint8_t codec;
    uint32_t generation;
} play_request_t;

static struct {
    bool inited;
    SemaphoreHandle_t lock;         // 保护 url/name/codec/error
    SemaphoreHandle_t title_lock;   // 保护 title(标题由播放任务高频更新)
    TaskHandle_t task;

    volatile uint32_t generation;
    volatile bool want_stop;
    volatile bool paused;

    char url[256];
    char name[64];
    uint8_t codec;

    volatile uint8_t state;
    volatile uint8_t volume;
    volatile uint8_t level;

    // 流信息(界面显示用)。码率不是从帧头读的,而是按"收到的音频字节数 / 耗时"
    // 算出来的实测值 —— HE-AAC 的 ADTS 头写的是核心层码率,VBR MP3 的帧头也
    // 只是当帧的瞬时值,两者直接显示都会误导用户。
    volatile uint8_t stream_codec;   // radio_codec_t
    volatile uint16_t bitrate_kbps;  // 实测,0 = 还没测出来
    volatile uint16_t sample_rate;

    char error[48];

    radio_icy_t icy;                // 只有播放任务访问
    char title[RADIO_ICY_TITLE_CAP];
    volatile uint32_t title_seq;
} s_player;

// ---------------------------------------------------------------------------
// 状态与快照
// ---------------------------------------------------------------------------
static void set_state(radio_player_state_t state) {
    s_player.state = (uint8_t)state;
}

static void set_error(const char *text) {
    if (xSemaphoreTake(s_player.title_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_player.error, text != NULL ? text : "", sizeof(s_player.error) - 1);
        s_player.error[sizeof(s_player.error) - 1] = '\0';
        xSemaphoreGive(s_player.title_lock);
    }
}

radio_player_state_t radio_player_state(void) {
    return (radio_player_state_t)s_player.state;
}

const char *radio_player_state_text(void) {
    switch (radio_player_state()) {
    case RADIO_PLAYER_CONNECTING: return "Connecting";
    case RADIO_PLAYER_BUFFERING: return "Buffering";
    case RADIO_PLAYER_PLAYING: return "Playing";
    case RADIO_PLAYER_RECONNECTING: return "Reconnecting";
    case RADIO_PLAYER_ERROR: return "Failed";
    case RADIO_PLAYER_STOPPED:
    default: return "Stopped";
    }
}

const char *radio_player_last_error(void) {
    return s_player.error;
}

void radio_player_title(char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (s_player.title_lock == NULL) return;
    if (xSemaphoreTake(s_player.title_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    strncpy(out, s_player.title, cap - 1);
    out[cap - 1] = '\0';
    xSemaphoreGive(s_player.title_lock);
}

uint32_t radio_player_title_seq(void) {
    return s_player.title_seq;
}

uint8_t radio_player_level(void) {
    return s_player.level;
}

void radio_player_stream_info(char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    const uint8_t codec = s_player.stream_codec;
    const uint16_t hz = s_player.sample_rate;
    if (codec == RADIO_CODEC_UNKNOWN || hz < 1000) return;   // 还没解出格式

    // 采样率用整数拼,不依赖浮点 printf(%f 在 nano 格式化下会被砍掉)。
    char rate[20] = "";
    const uint16_t kbps = s_player.bitrate_kbps;
    if (kbps > 0) {
        snprintf(rate, sizeof(rate), "%ukbps ", (unsigned)kbps);
    }
    snprintf(out, cap, "%s %s%u.%ukHz", codec == RADIO_CODEC_AAC ? "AAC" : "MP3",
             rate, (unsigned)(hz / 1000u), (unsigned)((hz % 1000u) / 100u));
}

uint8_t radio_player_volume(void) {
    return s_player.volume;
}

bool radio_player_paused(void) {
    return s_player.paused;
}

static void publish_title(radio_icy_t *icy) {
    const char *title = radio_icy_title(icy);
    if (s_player.title_lock == NULL) return;
    if (xSemaphoreTake(s_player.title_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (strcmp(s_player.title, title) != 0) {
        strncpy(s_player.title, title, sizeof(s_player.title) - 1);
        s_player.title[sizeof(s_player.title) - 1] = '\0';
        s_player.title_seq++;
    }
    xSemaphoreGive(s_player.title_lock);
}

// 在锁内取一份请求快照。锁外整轮都用它。
static bool snapshot_request(play_request_t *out) {
    if (xSemaphoreTake(s_player.lock, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    const bool has_url = (s_player.url[0] != '\0');
    memcpy(out->url, s_player.url, sizeof(out->url));
    memcpy(out->name, s_player.name, sizeof(out->name));
    out->codec = s_player.codec;
    out->generation = s_player.generation;
    xSemaphoreGive(s_player.lock);
    return has_url;
}

static bool request_current(uint32_t generation) {
    return !s_player.want_stop && s_player.generation == generation;
}

// ---------------------------------------------------------------------------
// 解码辅助
// ---------------------------------------------------------------------------
static esp_audio_simple_dec_type_t simple_dec_type(uint8_t codec) {
    // 目录里目前只有这两种格式;未知格式按 MP3 试,失败会在日志里明确报出来,
    // 不会变成"界面显示在播放但没声音"。
    return codec == RADIO_CODEC_AAC ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC
                                    : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

// 立体声 -> 单声道。ES8311 这条链路按单声道 16bit 打开,不做下混就会把左右交错的
// 数据当成双倍长度的单声道播出去(音调变高、语速翻倍)。
static size_t downmix_to_mono(uint8_t *pcm, size_t bytes, uint8_t channels) {
    if (pcm == NULL || channels == 0) return 0;
    if (channels == 1) return bytes & ~(size_t)1;

    int16_t *samples = (int16_t *)pcm;
    const size_t frames = bytes / (sizeof(int16_t) * channels);
    for (size_t frame = 0; frame < frames; frame++) {
        int32_t mixed = 0;
        for (uint8_t ch = 0; ch < channels; ch++) {
            mixed += samples[frame * channels + ch];
        }
        samples[frame] = (int16_t)(mixed / (int32_t)channels);
    }
    return frames * sizeof(int16_t);
}

// 用整块 PCM 的 RMS 估算响度(0..100),给界面画电平条。
static void update_level(const uint8_t *pcm, size_t bytes) {
    if (pcm == NULL || bytes < sizeof(int16_t) * 32) {
        s_player.level = 0;
        return;
    }
    const int16_t *samples = (const int16_t *)pcm;
    const size_t count = bytes / sizeof(int16_t);
    int64_t energy = 0;
    for (size_t i = 0; i < count; i++) {
        energy += (int64_t)samples[i] * samples[i];
    }
    const double rms = sqrt((double)(energy / (int64_t)count));
    if (rms < 1.0) {
        s_player.level = 0;
        return;
    }
    // -60dBFS..0dBFS 映射到 0..100。
    double db = 20.0 * log10(rms / 32768.0);
    if (db < -60.0) db = -60.0;
    if (db > 0.0) db = 0.0;
    s_player.level = (uint8_t)((db + 60.0) * 100.0 / 60.0);
}

// ---------------------------------------------------------------------------
// 响应头收集
//
// 为什么不用 esp_http_client_get_header():那个 API 读的是【请求】头
// (实现里就是 http_header_get(client->request->headers, ...)),拿不到
// icy-metaint / Location 这类响应头。响应头只能通过 HTTP_EVENT_ON_HEADER 回调
// 拿到。这里踩过一次:icy-metaint 永远读到 0,于是带元数据的流从不解复用,
// 元数据字节被当音频送进解码器。
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t metaint;
    bool has_location;
    char location[256];
} http_probe_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    http_probe_t *probe = (http_probe_t *)evt->user_data;
    if (probe == NULL) return ESP_OK;
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    if (evt->header_key == NULL || evt->header_value == NULL) return ESP_OK;

    if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        probe->metaint = (uint32_t)strtoul(evt->header_value, NULL, 10);
    } else if (strcasecmp(evt->header_key, "location") == 0) {
        strncpy(probe->location, evt->header_value, sizeof(probe->location) - 1);
        probe->location[sizeof(probe->location) - 1] = '\0';
        probe->has_location = true;
    }
    return ESP_OK;
}

// HTTP 3xx 需要重新跟随。注意马来西亚(Astro/Revma)与新加坡(StreamTheWorld)
// 给出的"稳定"地址都会 302 到一个带一次性令牌的边缘主机,主机和令牌每次连接
// 都变,所以必须每一跳重新跟随,绝不能缓存跳转后的地址。
static bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

// ---------------------------------------------------------------------------
// 单次连接与播放。返回 true 表示"确实播出过音频"。
// ---------------------------------------------------------------------------
static bool stream_once(const play_request_t *req) {
    bool played = false;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *in_buf = NULL;
    uint8_t *out_buf = NULL;

    // 换台/重连都要把流信息清掉,否则会拿上一个台的码率去显示新台。
    s_player.stream_codec = req->codec;
    s_player.bitrate_kbps = 0;
    s_player.sample_rate = 0;

    // ⚠ 先分配解码器和流缓冲,再建立 HTTP/TLS 连接 —— 这个顺序是有原因的:
    //   AAC 解码器要求一次性拿到几十 KB【连续】堆(HE-AAC 更多),本板没有 PSRAM。
    //   连接建立之后 TLS/TCP 会话已经把堆切碎,那时再 open 就是
    //   esp_audio_simple_dec_open -> -2 (ESP_AUDIO_ERR_MEM_LACK),
    //   真机现象为"Mix FM connecting 然后 stopped"。趁堆还完整时先拿大块。
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = simple_dec_type(req->codec),
        // dec_cfg = NULL:AAC 走"不启用 AAC-Plus(SBR)"的默认配置。
        // 这是【内存换音质】的取舍 —— 本板只有几十 KB 堆,AAC-Plus 只会要得更多。
        // Revma 的 HE-AAC 流在 SBR 关闭时按核心层解码(22050 Hz),人声/流行乐
        // 听起来正常,只是少了高频细节。宁可这样,也好过整台放不出来。
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    ESP_LOGI(TAG, "解码器分配前:codec=%u codec_name=%s heap=%u largest=%u", (unsigned)req->codec,
             req->codec == RADIO_CODEC_AAC ? "AAC" : "MP3",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器打开失败(codec=%u, heap=%u largest=%u)", (unsigned)req->codec,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        // 这台设备上唯一会走到这里的原因是内存:AAC 解码器要的连续块比本板能给的更大。
        // 文案必须说清楚"是内存不是网络",否则用户会一直去查 Wi-Fi。
        set_error(req->codec == RADIO_CODEC_AAC ? "AAC needs more RAM than free"
                                                : "Decoder init failed");
        return false;
    }

    in_buf = (uint8_t *)malloc(STREAM_IN_BYTES);
    out_buf = (uint8_t *)malloc(STREAM_OUT_BYTES);
    if (in_buf == NULL || out_buf == NULL) {
        ESP_LOGE(TAG, "流缓冲分配失败(需 %u+%u 字节) heap=%u largest=%u",
                 (unsigned)STREAM_IN_BYTES, (unsigned)STREAM_OUT_BYTES,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        set_error(req->codec == RADIO_CODEC_AAC ? "AAC needs more RAM than free"
                                                : "Out of memory");
        esp_audio_simple_dec_close(decoder);
        free(out_buf);
        free(in_buf);
        return false;
    }

    // 当前要连的地址。跟随 302 时会换成 Location 里的新地址。
    char url[256];
    strncpy(url, req->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    // 最多跟随 4 跳。Revma(Astro 系)与 StreamTheWorld(Mediacorp/RTM)给出的"稳定"
    // 地址都是"一跳 302 到带一次性令牌的边缘主机",主机与令牌每次连接都变,
    // 所以绝不能缓存跳转后的地址,必须每次连接重新跟随。
    for (int hop = 0; hop <= 4; hop++) {
        esp_http_client_config_t config = {0};
        http_probe_t probe = {0};
        config.url = url;
        config.timeout_ms = 10000;
        config.buffer_size = 2048;
        config.buffer_size_tx = 512;
        // 必须使用"播放器"风格的 User-Agent,不要伪装成浏览器:实测 StreamTheWorld
        // 对完整 Chrome UA 会先返回 200 和正确的音频响应头,然后在【恰好 32768 字节】
        // 处直接断开(约 2.7 秒音频)。现象是"播一下就哑了",而且状态码、响应头全都
        // 正常,极难从日志定位。播放器 UA 能完整读完。
        config.user_agent = "AI-Passport-Radio/1.0";
        config.keep_alive_enable = true;
        // 关掉库自带的自动跳转:它埋在 fetch_headers 的状态机里,不跟跳时只留下一个
        // 302 状态码,很难判断卡在哪。这里自己处理,并把每一跳写进日志。
        config.disable_auto_redirect = true;
        config.event_handler = http_event_cb;
        config.user_data = &probe;
        if (strncmp(url, "https://", 8) == 0) {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            set_error("Out of memory");
            break;
        }
        // 这次要元数据:服务器会在音频字节之间插入 StreamTitle 块。
        esp_http_client_set_header(client, "Icy-MetaData", "1");
        esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/aac,*/*");

        set_state(RADIO_PLAYER_CONNECTING);
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "打开流失败");
            set_error("Cannot reach server");
            esp_http_client_cleanup(client);
            break;
        }
        (void)esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);

        if (is_redirect_status(status)) {
            if (!probe.has_location || probe.location[0] == '\0') {
                ESP_LOGW(TAG, "HTTP %d 但响应里没有 Location", status);
                set_error("Bad redirect");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                break;
            }
            ESP_LOGI(TAG, "HTTP %d,跟随第 %d 跳", status, hop + 1);
            strncpy(url, probe.location, sizeof(url) - 1);
            url[sizeof(url) - 1] = '\0';
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP 状态码 %d", status);
            set_error("Server error");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            break;
        }

        do {
        // icy-metaint:每多少个音频字节后面跟一个元数据块;取不到就按无元数据处理。
        const size_t metaint = (size_t)probe.metaint;
        radio_icy_init(&s_player.icy, metaint);
        publish_title(&s_player.icy);

        ESP_LOGI(TAG, "开始播放 %s metaint=%u heap=%u largest=%u", req->name,
                 (unsigned)metaint, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        set_state(RADIO_PLAYER_BUFFERING);
        bool format_ready = false;
        bool stream_start_synced = false;
        uint8_t channels = 1;
        int empty_reads = 0;
        // 实测码率的滑动窗口:2 秒统计一次。位/毫秒在数值上就等于 kbps。
        uint32_t window_bytes = 0;
        int64_t window_start_us = esp_timer_get_time();

        while (request_current(req->generation)) {
            // 暂停时不读网络也不写 I2S,但保持连接;继续时从当前位置接着播。
            if (s_player.paused) {
                s_player.level = 0;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const int received = esp_http_client_read(client, (char *)in_buf, STREAM_IN_BYTES);
            if (received < 0) {
                ESP_LOGW(TAG, "流读取错误");
                set_error("Network lost");
                break;
            }
            if (received == 0) {
                if (++empty_reads > MAX_EMPTY_READS) {
                    ESP_LOGW(TAG, "服务端停止推送数据");
                    set_error("Stream closed");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            empty_reads = 0;

            // ICY 解复用:只把音频字节交给解码器。不解复用的话元数据会被当成音频帧,
            // 解码器要么吐噪声、要么直接报错复位。
            size_t audio_len = 0;
            for (int i = 0; i < received; i++) {
                if (radio_icy_consume(&s_player.icy, in_buf[i]) == RADIO_ICY_AUDIO) {
                    in_buf[audio_len++] = in_buf[i];
                }
            }
            publish_title(&s_player.icy);
            if (audio_len == 0) continue;

            // 实测码率:只统计真正的音频字节(ICY 元数据不算),否则带元数据的流
            // 会被算高。2 秒一个窗口,短窗口能让界面尽快显示出来。
            window_bytes += (uint32_t)audio_len;
            const int64_t now_us = esp_timer_get_time();
            const int64_t elapsed_us = now_us - window_start_us;
            if (elapsed_us >= 2000000) {
                const uint32_t kbps =
                    (uint32_t)(((int64_t)window_bytes * 8) / (elapsed_us / 1000));
                s_player.bitrate_kbps = (uint16_t)(kbps > 2000u ? 2000u : kbps);
                window_bytes = 0;
                window_start_us = now_us;
            }

            // 流的第一批字节通常落在某一帧的中间(Icecast 从"正在播"的位置
            // 开始发),RTHK 六个频道实测偏移 12~93 字节。把这段半截帧丢掉再交给
            // 解码器,可以避免开头一声爆音或解码器直接判定格式错误。
            // 找不到就原样交给解码器自己对齐 —— 不在这里死等,否则一个不含合法
            // 帧头的流会让播放永远停在"缓冲中"。
            if (!stream_start_synced) {
                const radio_frame_kind_t kind =
                    (req->codec == RADIO_CODEC_AAC) ? RADIO_FRAME_KIND_AAC_ADTS
                                                    : RADIO_FRAME_KIND_MP3;
                size_t skip = 0;
                if (radio_frame_find(in_buf, audio_len, kind, &skip)) {
                    if (skip > 0) {
                        memmove(in_buf, in_buf + skip, audio_len - skip);
                        audio_len -= skip;
                        ESP_LOGI(TAG, "跳过开头 %u 字节的半截帧", (unsigned)skip);
                    }
                    stream_start_synced = true;
                }
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = (uint32_t)audio_len,
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };

            while (raw.len > 0 && request_current(req->generation)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = out_buf,
                    .len = STREAM_OUT_BYTES,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                const uint32_t before = raw.len;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(TAG, "解码输出缓冲不足,需要 %u 字节",
                             (unsigned)frame.needed_size);
                    set_error("Decode buffer too small");
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "解码失败: %d", (int)result);
                    set_error("Unsupported stream");
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    if (!format_ready) {
                        esp_audio_simple_dec_info_t info = {0};
                        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                            raw.len = 0;
                            break;
                        }
                        // 采样率一律以解码器实际吐出的为准:HE-AAC 的 ADTS 头里写的是
                        // 核心采样率,解码后可能翻倍,直接用头里的值会让音调偏低。
                        channels = (uint8_t)(info.channel < 1 ? 1 : info.channel);
                        if (channels > 2) channels = 2;
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(TAG, "不支持的音频格式: %luHz/%ubit/%uch",
                                     (unsigned long)info.sample_rate,
                                     (unsigned)info.bits_per_sample, (unsigned)channels);
                            set_error("Unsupported format");
                            raw.len = 0;
                            break;
                        }
                        bsp_audio_set_volume(s_player.volume);
                        s_player.sample_rate = (uint16_t)info.sample_rate;
                        format_ready = true;
                        ESP_LOGI(TAG, "%s: %luHz %uch -> 单声道 16bit", req->name,
                                 (unsigned long)info.sample_rate, (unsigned)channels);
                    }

                    const size_t mono_bytes =
                        downmix_to_mono(frame.buffer, frame.decoded_size, channels);
                    update_level(frame.buffer, mono_bytes);
                    if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK) {
                        continue;
                    }
                    if (!played) {
                        played = true;
                        set_state(RADIO_PLAYER_PLAYING);
                    }
                }

                // 解析器可能把一个不完整的尾巴留在内部:本轮没有进展就必须跳出去取
                // 新的网络数据,否则会在这里空转把 CPU 吃满并触发看门狗。
                if (raw.len == before || raw.consumed == 0) break;
            }
        }
    } while (false);

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        // 本次连接已结束(播出过音频,或断流/失败);不再继续跟随跳转,
        // 交由 player_task 决定重连还是换台。
        break;
    }

    s_player.level = 0;
    if (decoder != NULL) esp_audio_simple_dec_close(decoder);
    free(out_buf);
    free(in_buf);
    return played;
}

// ---------------------------------------------------------------------------
// 播放任务
// ---------------------------------------------------------------------------
static void player_task(void *arg) {
    (void)arg;
    play_request_t req;
    uint32_t last_generation = 0;
    uint8_t failures = 0;

    for (;;) {
        if (s_player.want_stop) {
            // ⚠ 出错时也会置 want_stop(看下面的 MAX_CONSECUTIVE_FAILURES 分支)。
            // 早先这里无条件写 STOPPED,结果永远覆盖掉刚设好的 ERROR:
            // 界面上只显示 "Stopped",真正的失败原因(内存不足/服务器错误/格式不支持)
            // 一次都看不到 —— 真机上就是"Mix FM connecting 然后 stopped"。
            if (radio_player_state() != RADIO_PLAYER_ERROR) {
                set_state(RADIO_PLAYER_STOPPED);
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        if (!snapshot_request(&req)) {
            set_state(RADIO_PLAYER_STOPPED);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (req.generation != last_generation) {
            last_generation = req.generation;
            failures = 0;
        }

        const bool played = stream_once(&req);
        if (!request_current(req.generation)) continue;   // 用户换台/停止:立刻重来

        if (played) {
            // 播出过声音说明这个台是活的,只是断流了:保持选中并重连。
            failures = 0;
            set_state(RADIO_PLAYER_RECONNECTING);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        failures++;
        if (failures < MAX_CONSECUTIVE_FAILURES) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        // 连续失败就停下来并保留原因,由界面提示用户换台;继续无脑重试既费电,
        // 也会让界面状态一直在"连接中/失败"之间闪。
        set_state(RADIO_PLAYER_ERROR);
        s_player.want_stop = true;
        failures = 0;
    }
}

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
esp_err_t radio_player_init(void) {
    if (s_player.inited) return ESP_OK;

    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 55;
    s_player.state = (uint8_t)RADIO_PLAYER_STOPPED;
    s_player.want_stop = true;   // 没有请求时任务只空转

    s_player.lock = xSemaphoreCreateMutex();
    s_player.title_lock = xSemaphoreCreateMutex();
    if (s_player.lock == NULL || s_player.title_lock == NULL) return ESP_ERR_NO_MEM;

    if (bsp_audio_init() != ESP_OK) return ESP_FAIL;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    // simple_dec 层负责把裸流切成帧(MP3 帧头/ADTS 头),必须单独注册。
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return ESP_FAIL;

    // 播放任务常驻:换台只改请求并递增 generation,避免频繁创建/销毁任务。
    if (xTaskCreate(player_task, "radio_play", PLAYER_TASK_STACK, NULL,
                    PLAYER_TASK_PRIO, &s_player.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_player.inited = true;
    return ESP_OK;
}

void radio_player_play(const radio_station_t *station) {
    if (!s_player.inited || s_player.lock == NULL) return;
    if (station == NULL || station->url == NULL || station->url[0] == '\0') {
        radio_player_stop();
        return;
    }
    if (xSemaphoreTake(s_player.lock, pdMS_TO_TICKS(200)) != pdTRUE) return;

    // 先把旧请求作废,再整体写入新请求,最后递增 generation 放行。
    // 顺序反了会让播放任务读到"新 url + 认为仍然有效的旧 generation"。
    s_player.want_stop = true;
    s_player.generation++;
    strncpy(s_player.url, station->url, sizeof(s_player.url) - 1);
    s_player.url[sizeof(s_player.url) - 1] = '\0';
    strncpy(s_player.name, station->name != NULL ? station->name : "",
            sizeof(s_player.name) - 1);
    s_player.name[sizeof(s_player.name) - 1] = '\0';
    s_player.codec = (uint8_t)station->codec;
    s_player.paused = false;
    // 立刻清掉上一个台的流信息,避免"换台后还显示旧台的码率"这种假信息。
    s_player.stream_codec = RADIO_CODEC_UNKNOWN;
    s_player.bitrate_kbps = 0;
    s_player.sample_rate = 0;
    strncpy(s_player.error, "", sizeof(s_player.error));
    set_state(RADIO_PLAYER_CONNECTING);
    s_player.generation++;
    s_player.want_stop = false;
    xSemaphoreGive(s_player.lock);
}

void radio_player_stop(void) {
    s_player.want_stop = true;
    s_player.generation++;
    s_player.paused = false;
    s_player.level = 0;
    s_player.stream_codec = RADIO_CODEC_UNKNOWN;
    s_player.bitrate_kbps = 0;
    s_player.sample_rate = 0;
    set_state(RADIO_PLAYER_STOPPED);
}

void radio_player_set_paused(bool paused) {
    s_player.paused = paused;
    if (paused) {
        s_player.level = 0;
    }
}

void radio_player_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    s_player.volume = volume;
    bsp_audio_set_volume(volume);
}
