/**
 * @file alarm_mgr.c
 * @brief 闹钟与定时开关灯实现
 *
 * 星期位掩码约定：bit0 = 周日 … bit6 = 周六，与 struct tm::tm_wday 对齐；
 * 对外（串口命令、MQTT 报文）用 1~7 表示，1 = 周一 … 7 = 周日，掩码为 0 表示"每天"。
 *
 * 任务每秒检查一次；本地时间未同步（tm_year <= 100）时直接跳过，
 * 因此未联网校时不会误触发。
 */
#include "alarm_mgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys_state.h"

static const char *TAG = "alarm_mgr";

static alarm_item_t s_alarms[ALARM_MAX_COUNT];
static timer_item_t s_timer;
static int s_last_trigger_minute = -1;   /* 防止同一分钟重复触发 */

/* -------------------------------------------------------------------------- */
/*                                 同步/工具                                    */
/* -------------------------------------------------------------------------- */
static void sync_to_state(void)
{
    sys_state_set_alarms(s_alarms, ALARM_MAX_COUNT);
    sys_state_set_timer(&s_timer);
    /* 配置一变就请云端立即补一条遥测：这样从串口改闹钟、定时器，
       大屏也能同步刷新，不用等下一次周期上报或手动刷页面。 */
    sys_state_notify(SYS_EV_POST_NOW);
}

static const char *weekday_names(int bit)
{
    static const char *names[7] = {"日", "一", "二", "三", "四", "五", "六"};
    return names[bit];
}

static void week_mask_to_string(uint8_t mask, char *buf, size_t len)
{
    if (mask == 0) {
        snprintf(buf, len, "每天");
        return;
    }
    size_t pos = 0;
    buf[0] = '\0';
    for (int b = 0; b < 7; b++) {
        if (mask & (1 << b)) {
            int w = (b == 0) ? 7 : b;    /* 内部 bit0=周日 -> 用户表示 7 */
            int written = snprintf(buf + pos, len - pos, "%s%d", pos ? "," : "", w);
            if (written <= 0 || written >= (int)(len - pos)) break;
            pos += (size_t)written;
        }
    }
}

uint8_t alarm_mgr_parse_weekdays(const char *str)
{
    uint8_t mask = 0;
    if (str == NULL) {
        return 0;
    }
    const char *p = str;
    while (*p) {
        if (*p >= '1' && *p <= '7') {
            int day = *p - '0';
            int bit = (day == 7) ? 0 : day;   /* 7=周日 -> bit0 */
            mask |= (1 << bit);
        }
        p++;
    }
    return mask;
}

/* -------------------------------------------------------------------------- */
/*                                  闹钟                                       */
/* -------------------------------------------------------------------------- */
static int find_free_slot(void)
{
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_alarms[i].enabled) {
            return i;
        }
    }
    return -1;
}

int alarm_mgr_add(uint8_t hour, uint8_t minute, alarm_repeat_t repeat, uint8_t week_mask)
{
    if (hour > 23 || minute > 59) {
        return -1;
    }
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGW(TAG, "闹钟已满（最多 %d 个）", ALARM_MAX_COUNT);
        return -1;
    }
    s_alarms[slot].hour = hour;
    s_alarms[slot].minute = minute;
    s_alarms[slot].enabled = true;
    s_alarms[slot].repeat = repeat;
    s_alarms[slot].week_mask = (repeat == ALARM_REPEAT_DAILY) ? week_mask : 0;
    sync_to_state();

    char wstr[24];
    week_mask_to_string(s_alarms[slot].week_mask, wstr, sizeof(wstr));
    ESP_LOGI(TAG, "闹钟 %d 已添加: %02d:%02d %s %s", slot + 1, hour, minute,
             (repeat == ALARM_REPEAT_ONCE) ? "单次" : "重复", wstr);
    return slot + 1;
}

esp_err_t alarm_mgr_del(int slot)
{
    int idx = slot - 1;
    if (idx < 0 || idx >= ALARM_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_alarms[idx].enabled) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(&s_alarms[idx], 0, sizeof(alarm_item_t));
    sync_to_state();
    ESP_LOGI(TAG, "闹钟 %d 已删除", slot);
    return ESP_OK;
}

esp_err_t alarm_mgr_del_all(void)
{
    memset(s_alarms, 0, sizeof(s_alarms));
    sync_to_state();
    ESP_LOGI(TAG, "已删除所有闹钟");
    return ESP_OK;
}

void alarm_mgr_print(void)
{
    int cnt = 0;
    printf("闹钟列表:\n");
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_alarms[i].enabled) continue;
        cnt++;
        char wstr[24];
        week_mask_to_string(s_alarms[i].week_mask, wstr, sizeof(wstr));
        printf("  [%d] %02d:%02d  %s  %s\n", i + 1, s_alarms[i].hour, s_alarms[i].minute,
               (s_alarms[i].repeat == ALARM_REPEAT_ONCE) ? "单次" : "重复", wstr);
    }
    if (cnt == 0) {
        printf("  (空)\n");
    }
}

void alarm_mgr_trigger_now(void)
{
    ESP_LOGI(TAG, "手动触发闹钟响铃");
    sys_state_notify(SYS_EV_ALARM_RINGING);
}

/* -------------------------------------------------------------------------- */
/*                                 定时器                                      */
/* -------------------------------------------------------------------------- */
esp_err_t alarm_mgr_timer_set(uint8_t h_on, uint8_t m_on, uint8_t h_off, uint8_t m_off)
{
    if (h_on > 23 || m_on > 59 || h_off > 23 || m_off > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    s_timer.hour_on = h_on;
    s_timer.min_on = m_on;
    s_timer.hour_off = h_off;
    s_timer.min_off = m_off;
    s_timer.enabled = true;
    s_timer.one_shot = false;
    sync_to_state();

    bool cross_day = (h_on * 60 + m_on) > (h_off * 60 + m_off);
    ESP_LOGI(TAG, "定时器已设置: %02d:%02d - %02d:%02d %s", h_on, m_on, h_off, m_off,
             cross_day ? "(跨天)" : "");
    return ESP_OK;
}

esp_err_t alarm_mgr_timer_disable(void)
{
    s_timer.enabled = false;
    sync_to_state();
    ESP_LOGI(TAG, "定时器已关闭");
    return ESP_OK;
}

esp_err_t alarm_mgr_timer_set_week(uint8_t week_mask)
{
    s_timer.week_mask = week_mask;
    sync_to_state();
    char wstr[24];
    week_mask_to_string(week_mask, wstr, sizeof(wstr));
    ESP_LOGI(TAG, "定时器重复: %s", wstr);
    return ESP_OK;
}

void alarm_mgr_timer_print(void)
{
    if (!s_timer.enabled) {
        printf("定时器: 未启用\n");
        return;
    }
    char wstr[24];
    week_mask_to_string(s_timer.week_mask, wstr, sizeof(wstr));
    printf("定时器: %02d:%02d - %02d:%02d  %s\n",
           s_timer.hour_on, s_timer.min_on, s_timer.hour_off, s_timer.min_off, wstr);
}

/* -------------------------------------------------------------------------- */
/*                                  检查逻辑                                    */
/* -------------------------------------------------------------------------- */
static void check_alarms(const struct tm *timeinfo)
{
    int minute_key = timeinfo->tm_hour * 60 + timeinfo->tm_min;
    if (minute_key == s_last_trigger_minute) {
        return;   /* 这一分钟已经触发过 */
    }

    int wday = timeinfo->tm_wday;   /* 0=周日 */

    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_alarms[i].enabled) continue;
        if (timeinfo->tm_hour != s_alarms[i].hour || timeinfo->tm_min != s_alarms[i].minute) {
            continue;
        }

        bool should_trigger = false;
        if (s_alarms[i].repeat == ALARM_REPEAT_ONCE) {
            should_trigger = true;
        } else if (s_alarms[i].week_mask == 0) {
            should_trigger = true;
        } else if (s_alarms[i].week_mask & (1 << wday)) {
            should_trigger = true;
        }

        if (should_trigger) {
            s_last_trigger_minute = minute_key;
            ESP_LOGI(TAG, "闹钟 %d 触发: %02d:%02d (周%s)", i + 1,
                     s_alarms[i].hour, s_alarms[i].minute, weekday_names(wday));
            sys_state_notify(SYS_EV_ALARM_RINGING);

            if (s_alarms[i].repeat == ALARM_REPEAT_ONCE) {
                memset(&s_alarms[i], 0, sizeof(alarm_item_t));
                sync_to_state();
            }
            break;
        }
    }
}

/**
 * 定时器开关灯判定。开灯时间晚于关灯时间即视为跨天（22:00 → 次日 06:00），
 * 用"当前秒数落在 [on, off) 区间"这一统一形式处理两种情况。
 * 定时只在 OFF 与 AUTO 之间切换，不会覆盖用户手动设置的颜色。
 */
static void check_timer(const struct tm *timeinfo)
{
    if (!s_timer.enabled) {
        return;
    }
    if (s_timer.week_mask != 0 && !(s_timer.week_mask & (1 << timeinfo->tm_wday))) {
        return;
    }

    uint32_t now_sec = timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
    uint32_t on_sec  = s_timer.hour_on * 3600 + s_timer.min_on * 60;
    uint32_t off_sec = s_timer.hour_off * 3600 + s_timer.min_off * 60;

    bool should_be_on = (on_sec < off_sec) ? (now_sec >= on_sec && now_sec < off_sec)
                                           : (now_sec >= on_sec || now_sec < off_sec);

    light_mode_t mode = sys_state_get_mode();

    if (should_be_on) {
        if (mode == LIGHT_MODE_OFF) {
            ESP_LOGI(TAG, "定时器到点，开灯");
            sys_state_set_mode(LIGHT_MODE_AUTO);
            if (s_timer.one_shot) {
                s_timer.enabled = false;
                sync_to_state();
            }
        }
    } else {
        if (mode == LIGHT_MODE_AUTO) {
            ESP_LOGI(TAG, "定时器到点，关灯");
            sys_state_set_mode(LIGHT_MODE_OFF);
            if (s_timer.one_shot) {
                s_timer.enabled = false;
                sync_to_state();
            }
        }
    }
}

static void alarm_task(void *arg)
{
    while (1) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year > 100) {   /* 时间已同步 */
            check_alarms(&timeinfo);
            check_timer(&timeinfo);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* -------------------------------------------------------------------------- */
/*                                  生命周期                                    */
/* -------------------------------------------------------------------------- */
esp_err_t alarm_mgr_init(void)
{
    memset(s_alarms, 0, sizeof(s_alarms));
    memset(&s_timer, 0, sizeof(s_timer));
    s_last_trigger_minute = -1;
    sync_to_state();
    ESP_LOGI(TAG, "闹钟/定时器模块初始化完成");
    return ESP_OK;
}

esp_err_t alarm_mgr_start(void)
{
    if (xTaskCreatePinnedToCore(alarm_task, "alarm_task", 2560, NULL, 4,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "闹钟任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "闹钟/定时器任务已启动");
    return ESP_OK;
}
