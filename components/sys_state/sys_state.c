/**
 * @file sys_state.c
 * @brief 系统状态中心实现
 *
 * 所有状态读写都经过互斥锁；写入完成后通过事件组通知订阅者，
 * 因此调用方不需要自己加锁或手动通知其它模块。
 *
 * 注意：通知必须在释放互斥锁之后发出，避免订阅者在回调里
 * 再调用 sys_state_get() 时形成死锁（本文件统一遵循该顺序）。
 */
#include "sys_state.h"

#include <string.h>
#include "board_config.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "sys_state";

static sys_state_t s_state = {
    .mode = LIGHT_MODE_AUTO,
    .rgb = {0, 0, 0},
    .manual_rgb = {255, 100, 50},
    .temperature = 0.0f,
    .light_adc = 0,
    .light_percent = 0,
    .wifi_connected = false,
    .time_synced = false,
    .cloud_connected = false,
    .ip_addr = "0.0.0.0",
    .ssid = "",
    .oled_page = 1,
};

static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_events = NULL;

esp_err_t sys_state_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_state.alarms, 0, sizeof(s_state.alarms));
    memset(&s_state.timer, 0, sizeof(s_state.timer));
    ESP_LOGI(TAG, "状态中心初始化完成");
    return ESP_OK;
}

EventGroupHandle_t sys_state_events(void)
{
    return s_events;
}

void sys_state_notify(uint32_t bits)
{
    if (s_events) {
        xEventGroupSetBits(s_events, bits);
    }
}

void sys_state_get(sys_state_t *out)
{
    if (s_mutex == NULL || out == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    memcpy(out, &s_state, sizeof(sys_state_t));
    xSemaphoreGive(s_mutex);
}

/* --------------------------------- 灯光 ---------------------------------- */

void sys_state_set_mode(light_mode_t mode)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_state.mode != mode) {
        s_state.mode = mode;
        xSemaphoreGive(s_mutex);
        sys_state_notify(SYS_EV_MODE_CHANGED | SYS_EV_POST_NOW);
        ESP_LOGI(TAG, "模式 -> %d", mode);
    } else {
        xSemaphoreGive(s_mutex);
    }
}

light_mode_t sys_state_get_mode(void)
{
    light_mode_t m = LIGHT_MODE_AUTO;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        m = s_state.mode;
        xSemaphoreGive(s_mutex);
    }
    return m;
}

void sys_state_set_manual_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.manual_rgb[0] = r;
    s_state.manual_rgb[1] = g;
    s_state.manual_rgb[2] = b;
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_COLOR_CHANGED | SYS_EV_POST_NOW);
}

void sys_state_get_manual_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (r) *r = s_state.manual_rgb[0];
    if (g) *g = s_state.manual_rgb[1];
    if (b) *b = s_state.manual_rgb[2];
    xSemaphoreGive(s_mutex);
}

void sys_state_set_output_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.rgb[0] = r;
    s_state.rgb[1] = g;
    s_state.rgb[2] = b;
    xSemaphoreGive(s_mutex);
}

void sys_state_get_output_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (r) *r = s_state.rgb[0];
    if (g) *g = s_state.rgb[1];
    if (b) *b = s_state.rgb[2];
    xSemaphoreGive(s_mutex);
}

/* -------------------------------- 传感器 ---------------------------------- */

void sys_state_set_sensor(float temperature, int light_adc, bool temp_valid)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.temperature = temperature;
    s_state.temp_valid = temp_valid;
    s_state.light_adc = light_adc;
#if BOARD_LIGHT_SENSOR_INVERT
    int pct = (light_adc * 100) / BOARD_LIGHT_ADC_MAX;      /* ADC 越大 = 越亮 */
#else
    int pct = 100 - (light_adc * 100) / BOARD_LIGHT_ADC_MAX; /* ADC 越大 = 越暗 */
#endif
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_state.light_percent = pct;
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_SENSOR_UPDATED);
}

/* --------------------------------- 网络 ----------------------------------- */

void sys_state_set_wifi(bool connected, const char *ip, const char *ssid)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.wifi_connected = connected;
    if (ip) {
        strncpy(s_state.ip_addr, ip, sizeof(s_state.ip_addr) - 1);
        s_state.ip_addr[sizeof(s_state.ip_addr) - 1] = '\0';
    }
    if (ssid) {
        strncpy(s_state.ssid, ssid, sizeof(s_state.ssid) - 1);
        s_state.ssid[sizeof(s_state.ssid) - 1] = '\0';
    }
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_NET_CHANGED);
}

void sys_state_set_cloud(bool connected)
{
    bool changed = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    changed = (s_state.cloud_connected != connected);
    s_state.cloud_connected = connected;
    xSemaphoreGive(s_mutex);

    if (changed) {
        sys_state_notify(SYS_EV_NET_CHANGED);
        ESP_LOGI(TAG, "云端 %s", connected ? "已连接" : "已断开");
    }
}

void sys_state_set_time_synced(bool synced)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.time_synced = synced;
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_NET_CHANGED);
}

/* ---------------------------------- UI ------------------------------------ */

void sys_state_set_page(int page)
{
    if (page < 1) page = 1;
    if (page > 3) page = 3;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state.oled_page = page;
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_UI_REFRESH);
}

int sys_state_get_page(void)
{
    int p = 1;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        p = s_state.oled_page;
        xSemaphoreGive(s_mutex);
    }
    return p;
}

/* ---------------------------- 闹钟 / 定时器 ------------------------------- */

esp_err_t sys_state_set_alarms(const alarm_item_t *alarms, int count)
{
    if (alarms == NULL || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    int n = (count > ALARM_MAX_COUNT) ? ALARM_MAX_COUNT : count;
    memcpy(s_state.alarms, alarms, sizeof(alarm_item_t) * n);
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_UI_REFRESH | SYS_EV_POST_NOW);
    return ESP_OK;
}

void sys_state_get_alarms(alarm_item_t *alarms, int count)
{
    if (alarms == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    int n = (count > ALARM_MAX_COUNT) ? ALARM_MAX_COUNT : count;
    memcpy(alarms, s_state.alarms, sizeof(alarm_item_t) * n);
    xSemaphoreGive(s_mutex);
}

esp_err_t sys_state_set_timer(const timer_item_t *timer)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    memcpy(&s_state.timer, timer, sizeof(timer_item_t));
    xSemaphoreGive(s_mutex);
    sys_state_notify(SYS_EV_UI_REFRESH | SYS_EV_POST_NOW);
    return ESP_OK;
}

void sys_state_get_timer(timer_item_t *timer)
{
    if (timer == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    memcpy(timer, &s_state.timer, sizeof(timer_item_t));
    xSemaphoreGive(s_mutex);
}
