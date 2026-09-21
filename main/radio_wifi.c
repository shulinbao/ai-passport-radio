// main/radio_wifi.c —— Wi-Fi 扫描 / 连接 / 凭据持久化实现。
#include "radio_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "radio_wifi";

#define NVS_NAMESPACE "radio"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

// 连接超时:家用/办公 2.4G 网络在这个时间内连不上,基本就是密码错或信号太差。
#define CONNECT_TIMEOUT_MS 15000
// 扫描时把 AP 记录放在静态区,避免在任务栈上开 2KB 以上的数组。
#define AP_RECORD_MAX RADIO_WIFI_SCAN_MAX

#define BIT_GOT_IP BIT0
#define BIT_FAILED BIT1

typedef enum {
    CMD_SCAN = 0,
    CMD_CONNECT,
    CMD_AUTOCONNECT,
    CMD_FORGET,
} wifi_cmd_kind_t;

typedef struct {
    wifi_cmd_kind_t kind;
    bool save;
    char ssid[RADIO_WIFI_SSID_MAX];
    char pass[RADIO_WIFI_PASS_MAX];
} wifi_cmd_t;

static struct {
    bool inited;
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    TaskHandle_t task;
    EventGroupHandle_t events;
    esp_netif_t *netif;

    radio_wifi_state_t state;
    char target_ssid[RADIO_WIFI_SSID_MAX];   // 正在连接/已连接的 SSID
    char ip[16];
    int rssi;

    radio_wifi_ap_t aps[RADIO_WIFI_SCAN_MAX];
    int ap_count;

    TickType_t last_rssi_poll;
} s_wifi;

// 扫描结果由驱动直接写入静态数组,避免在 wifi 任务栈上放大数组。
static wifi_ap_record_t s_records[AP_RECORD_MAX];

// ---------------------------------------------------------------------------
// NVS 凭据读写。密码只在这里出现,不进日志。
// ---------------------------------------------------------------------------
static bool nvs_read_str(const char *key, char *out, size_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = cap;
    const esp_err_t err = nvs_get_str(handle, key, out, &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

static esp_err_t nvs_write_str(const char *key, const char *value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, key, value != NULL ? value : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// 注意:不能叫 nvs_erase_key —— 那是 NVS 自己的函数名,同名会把内部调用解析到
// 本函数上,变成参数个数不匹配的编译错误(或者更糟:意外的自递归)。
static void nvs_remove_key(const char *key) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_erase_key(handle, key);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

// ---------------------------------------------------------------------------
// 状态访问。界面线程会随时读,所以统一走锁。
// ---------------------------------------------------------------------------
static void set_state(radio_wifi_state_t state) {
    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_wifi.state = state;
    xSemaphoreGive(s_wifi.lock);
}

radio_wifi_state_t radio_wifi_state(void) {
    if (!s_wifi.inited) return RADIO_WIFI_STATE_IDLE;
    radio_wifi_state_t state = RADIO_WIFI_STATE_IDLE;
    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        state = s_wifi.state;
        xSemaphoreGive(s_wifi.lock);
    }
    return state;
}

const char *radio_wifi_state_text(void) {
    // 顶栏只放得下几个字符,所以这里是给界面用的短标签,不是完整句子。
    switch (radio_wifi_state()) {
    case RADIO_WIFI_STATE_SCANNING: return "Scan";
    case RADIO_WIFI_STATE_CONNECTING: return "Link";
    case RADIO_WIFI_STATE_CONNECTED: return "Online";
    case RADIO_WIFI_STATE_FAILED: return "Fail";
    case RADIO_WIFI_STATE_IDLE:
    default: return "No net";
    }
}

const char *radio_wifi_current_ssid(void) {
    return s_wifi.target_ssid;
}

const char *radio_wifi_ip(void) {
    return s_wifi.ip;
}

int radio_wifi_rssi(void) {
    return s_wifi.rssi;
}

bool radio_wifi_has_credentials(void) {
    char ssid[RADIO_WIFI_SSID_MAX];
    return nvs_read_str(NVS_KEY_SSID, ssid, sizeof(ssid));
}

bool radio_wifi_saved_ssid(char *out, size_t cap) {
    return nvs_read_str(NVS_KEY_SSID, out, cap);
}

bool radio_wifi_saved_password(char *out, size_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = cap;
    const esp_err_t err = nvs_get_str(handle, NVS_KEY_PASS, out, &length);
    nvs_close(handle);
    // 开放网络没有密码:返回 true 但内容为空串。
    return err == ESP_OK;
}

// ---------------------------------------------------------------------------
// 事件处理。回调运行在系统事件任务里,只做状态更新,不做耗时操作。
// ---------------------------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        // 只打印 reason code,不打印 SSID/密码。
        ESP_LOGW(TAG, "断开连接 reason=%d", disc ? disc->reason : -1);
        if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_wifi.ip[0] = '\0';
            s_wifi.rssi = 0;
            // 主动断开(切网/清除凭据)不该显示成"连接失败"。
            if (s_wifi.state == RADIO_WIFI_STATE_CONNECTING) {
                s_wifi.state = RADIO_WIFI_STATE_FAILED;
            } else if (s_wifi.state == RADIO_WIFI_STATE_CONNECTED) {
                s_wifi.state = RADIO_WIFI_STATE_FAILED;
            }
            xSemaphoreGive(s_wifi.lock);
        }
        if (s_wifi.events) xEventGroupSetBits(s_wifi.events, BIT_FAILED);
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *got = (const ip_event_got_ip_t *)data;
    char text[16];
    esp_ip4addr_ntoa(&got->ip_info.ip, text, sizeof(text));
    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(s_wifi.ip, text, sizeof(s_wifi.ip) - 1);
        s_wifi.ip[sizeof(s_wifi.ip) - 1] = '\0';
        s_wifi.state = RADIO_WIFI_STATE_CONNECTED;
        xSemaphoreGive(s_wifi.lock);
    }
    ESP_LOGI(TAG, "已获取 IP: %s", text);
    if (s_wifi.events) xEventGroupSetBits(s_wifi.events, BIT_GOT_IP);
}

// ---------------------------------------------------------------------------
// 工作任务
// ---------------------------------------------------------------------------
static void do_scan(void) {
    set_state(RADIO_WIFI_STATE_SCANNING);
    // 阻塞式扫描:这一步在 wifi 任务里做,不会卡住界面。
    const esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        set_state(RADIO_WIFI_STATE_IDLE);
        return;
    }
    uint16_t count = AP_RECORD_MAX;
    if (esp_wifi_scan_get_ap_records(&count, s_records) != ESP_OK) {
        ESP_LOGW(TAG, "读取扫描结果失败");
        count = 0;
    }
    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        int written = 0;
        for (uint16_t i = 0; i < count && written < RADIO_WIFI_SCAN_MAX; i++) {
            // 隐藏网络(空 SSID)在屏幕上没法选,直接跳过。
            if (s_records[i].ssid[0] == '\0') continue;
            radio_wifi_ap_t *ap = &s_wifi.aps[written];
            strncpy(ap->ssid, (const char *)s_records[i].ssid, sizeof(ap->ssid) - 1);
            ap->ssid[sizeof(ap->ssid) - 1] = '\0';
            ap->rssi = s_records[i].rssi;
            ap->secure = (s_records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
            written++;
        }
        s_wifi.ap_count = written;
        s_wifi.state = RADIO_WIFI_STATE_IDLE;
        xSemaphoreGive(s_wifi.lock);
    }
    ESP_LOGI(TAG, "扫描完成,可用网络 %d 个", radio_wifi_scan_count());
}

static void do_connect(const char *ssid, const char *pass, bool save) {
    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, pass != NULL ? pass : "",
            sizeof(config.sta.password) - 1);
    // 阈值留 OPEN,这样开放网络与各类加密网络都能连;具体能否连上由 AP 决定。
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        strncpy(s_wifi.target_ssid, ssid, sizeof(s_wifi.target_ssid) - 1);
        s_wifi.target_ssid[sizeof(s_wifi.target_ssid) - 1] = '\0';
        s_wifi.ip[0] = '\0';
        s_wifi.state = RADIO_WIFI_STATE_CONNECTING;
        xSemaphoreGive(s_wifi.lock);
    }

    xEventGroupClearBits(s_wifi.events, BIT_GOT_IP | BIT_FAILED);
    (void)esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        ESP_LOGE(TAG, "写入 Wi-Fi 配置失败");
        set_state(RADIO_WIFI_STATE_FAILED);
        return;
    }
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "发起连接失败");
        set_state(RADIO_WIFI_STATE_FAILED);
        return;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi.events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & BIT_GOT_IP) != 0) {
        // 连上之后才写凭据:避免把错误密码存下来,下次开机又失败一遍。
        if (save) {
            (void)nvs_write_str(NVS_KEY_SSID, ssid);
            (void)nvs_write_str(NVS_KEY_PASS, pass != NULL ? pass : "");
        }
        return;
    }
    ESP_LOGW(TAG, "连接超时或失败");
    set_state(RADIO_WIFI_STATE_FAILED);
}

static void wifi_task(void *arg) {
    (void)arg;
    wifi_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_wifi.queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
        case CMD_SCAN:
            do_scan();
            break;
        case CMD_CONNECT:
            do_connect(cmd.ssid, cmd.pass, cmd.save);
            break;
        case CMD_AUTOCONNECT: {
            char ssid[RADIO_WIFI_SSID_MAX];
            char pass[RADIO_WIFI_PASS_MAX];
            if (!nvs_read_str(NVS_KEY_SSID, ssid, sizeof(ssid))) {
                set_state(RADIO_WIFI_STATE_IDLE);
                break;
            }
            (void)radio_wifi_saved_password(pass, sizeof(pass));
            do_connect(ssid, pass, false);
            break;
        }
        case CMD_FORGET:
            (void)esp_wifi_disconnect();
            nvs_remove_key(NVS_KEY_SSID);
            nvs_remove_key(NVS_KEY_PASS);
            if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
                s_wifi.target_ssid[0] = '\0';
                s_wifi.ip[0] = '\0';
                s_wifi.state = RADIO_WIFI_STATE_IDLE;
                xSemaphoreGive(s_wifi.lock);
            }
            ESP_LOGI(TAG, "已清除保存的 Wi-Fi 凭据");
            break;
        default:
            break;
        }
    }
}

static esp_err_t post(const wifi_cmd_t *cmd) {
    if (!s_wifi.inited || s_wifi.queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_wifi.queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
esp_err_t radio_wifi_init(void) {
    if (s_wifi.inited) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区写满或版本不匹配:擦掉重建。这里存的是 Wi-Fi 凭据和设置,
        // 擦除后果是"需要重新配网",不会损坏硬件。
        ESP_LOGW(TAG, "NVS 需要重建");
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_wifi.lock = xSemaphoreCreateMutex();
    if (s_wifi.lock == NULL) return ESP_ERR_NO_MEM;
    s_wifi.events = xEventGroupCreate();
    if (s_wifi.events == NULL) return ESP_ERR_NO_MEM;
    s_wifi.queue = xQueueCreate(4, sizeof(wifi_cmd_t));
    if (s_wifi.queue == NULL) return ESP_ERR_NO_MEM;

    // esp_netif / 事件循环可能已被别的模块初始化过,重复调用会返回
    // ESP_ERR_INVALID_STATE,这里视为"已经就绪"。
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_wifi.netif = esp_netif_create_default_wifi_sta();
    if (s_wifi.netif == NULL) return ESP_FAIL;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_ip_event, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    // 关掉省电模式:省电会在音频缓冲需要突发下载时拉高延迟,得不偿失。
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    if (xTaskCreate(wifi_task, "radio_wifi", 4096, NULL, 4, &s_wifi.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_wifi.inited = true;
    return ESP_OK;
}

esp_err_t radio_wifi_scan_start(void) {
    wifi_cmd_t cmd = {0};
    cmd.kind = CMD_SCAN;
    const esp_err_t err = post(&cmd);
    if (err == ESP_OK) set_state(RADIO_WIFI_STATE_SCANNING);
    return err;
}

int radio_wifi_scan_count(void) {
    if (!s_wifi.inited) return 0;
    int count = 0;
    if (xSemaphoreTake(s_wifi.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = s_wifi.ap_count;
        xSemaphoreGive(s_wifi.lock);
    }
    return count;
}

const radio_wifi_ap_t *radio_wifi_scan_at(int index) {
    if (!s_wifi.inited || index < 0 || index >= RADIO_WIFI_SCAN_MAX) return NULL;
    return &s_wifi.aps[index];
}

bool radio_wifi_scan_contains(const char *ssid) {
    if (ssid == NULL) return false;
    const int count = radio_wifi_scan_count();
    for (int i = 0; i < count; i++) {
        if (strcmp(s_wifi.aps[i].ssid, ssid) == 0) return true;
    }
    return false;
}

esp_err_t radio_wifi_connect(const char *ssid, const char *password, bool save) {
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    wifi_cmd_t cmd = {0};
    cmd.kind = CMD_CONNECT;
    cmd.save = save;
    strncpy(cmd.ssid, ssid, sizeof(cmd.ssid) - 1);
    if (password != NULL) strncpy(cmd.pass, password, sizeof(cmd.pass) - 1);
    return post(&cmd);
}

esp_err_t radio_wifi_autoconnect(void) {
    wifi_cmd_t cmd = {0};
    cmd.kind = CMD_AUTOCONNECT;
    return post(&cmd);
}

esp_err_t radio_wifi_forget(void) {
    wifi_cmd_t cmd = {0};
    cmd.kind = CMD_FORGET;
    return post(&cmd);
}

void radio_wifi_tick(void) {
    if (!s_wifi.inited || radio_wifi_state() != RADIO_WIFI_STATE_CONNECTED) return;
    const TickType_t now = xTaskGetTickCount();
    // 每 2 秒更新一次信号强度即可,刷太勤没有意义还会占总线。
    if (now - s_wifi.last_rssi_poll < pdMS_TO_TICKS(2000)) return;
    s_wifi.last_rssi_poll = now;
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        s_wifi.rssi = info.rssi;
    }
}
