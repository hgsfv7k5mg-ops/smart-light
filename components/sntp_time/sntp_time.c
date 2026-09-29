/**
 * @file sntp_time.c
 * @brief SNTP 校时实现
 *
 * 闹钟与定时器依赖本地时间，因此必须先校时成功（tm_year > 100）才会生效。
 * 时区固定为 UTC+8（中国不使用夏令时）。
 */
#include "sntp_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sys_state.h"

static const char *TAG = "sntp_time";

#define SNTP_SYNCED_BIT BIT0

static EventGroupHandle_t s_sntp_events = NULL;
static bool s_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "时间同步成功");
    s_synced = true;
    sys_state_set_time_synced(true);
    if (s_sntp_events) {
        xEventGroupSetBits(s_sntp_events, SNTP_SYNCED_BIT);
    }

    time_t now = time(NULL);
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    ESP_LOGI(TAG, "当前北京时间: %04d-%02d-%02d %02d:%02d:%02d",
             tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
             tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
}

esp_err_t sntp_time_init(void)
{
    s_sntp_events = xEventGroupCreate();
    if (s_sntp_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "cn.pool.ntp.org");
    esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    /* 北京时间 UTC+8（中国不使用夏令时） */
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_LOGI(TAG, "SNTP 已启动，等待校时...");
    return ESP_OK;
}

bool sntp_time_wait_sync(uint32_t timeout_ms)
{
    if (s_sntp_events == NULL) {
        return false;
    }
    /* 可能在校时等待前就已经同步完成 */
    if (s_synced) {
        return true;
    }
    EventBits_t bits = xEventGroupWaitBits(s_sntp_events, SNTP_SYNCED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return ((bits & SNTP_SYNCED_BIT) != 0);
}

bool sntp_time_is_synced(void)
{
    return s_synced;
}
