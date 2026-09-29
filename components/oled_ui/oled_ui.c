/**
 * @file oled_ui.c
 * @brief SSD1306 OLED 显示实现：三页面轮转 + 临时提示消息
 *
 * 页面内容：
 *   1 = 主界面（时间 / 温度 / 光照 / 模式）
 *   2 = 闹钟与定时器
 *   3 = 网络状态（WiFi / IP / 云端 / 校时）
 *
 * 刷新由 sys_state 的事件组驱动（SYS_EV_UI_REFRESH 等），无事件时按周期重绘。
 */
#include "oled_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "sys_state.h"

static const char *TAG = "oled_ui";

#define OLED_FONT_SIZE  12
#define OLED_LINE_H     16

static ssd1306_handle_t s_oled = NULL;
static uint64_t s_boot_time_us = 0;

/* 临时提示 */
static char s_msg1[24] = {0};
static char s_msg2[24] = {0};
static uint32_t s_msg_until_ms = 0;

/* -------------------------------------------------------------------------- */
/*                                 绘制工具                                     */
/* -------------------------------------------------------------------------- */
static void draw_line(int row, const char *text)
{
    ssd1306_draw_string(s_oled, 0, (uint8_t)(row * OLED_LINE_H),
                        (uint8_t *)text, OLED_FONT_SIZE, 1);
}

static void format_week(char *buf, size_t len, uint8_t mask)
{
    if (mask == 0) {
        if (len > 0) {
            buf[0] = '\0';
        }
        return;
    }
    size_t pos = 0;
    buf[0] = '\0';
    for (int b = 0; b < 7; b++) {
        if (mask & (1 << b)) {
            int w = (b == 0) ? 7 : b;
            int n = snprintf(buf + pos, len - pos, "%d", w);
            if (n <= 0 || n >= (int)(len - pos)) break;
            pos += (size_t)n;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                  页面                                        */
/* -------------------------------------------------------------------------- */
static void page_main(const sys_state_t *st)
{
    char line[32];
    time_t now = time(NULL);
    struct tm tinfo;
    localtime_r(&now, &tinfo);

    if (tinfo.tm_year > 100) {
        snprintf(line, sizeof(line), "%02d:%02d:%02d", tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
    } else {
        snprintf(line, sizeof(line), "--:--:--");
    }
    draw_line(0, line);

    if (st->temp_valid) {
        snprintf(line, sizeof(line), "Temp: %.1fC", st->temperature);
    } else {
        snprintf(line, sizeof(line), "Temp: --");
    }
    draw_line(1, line);

    snprintf(line, sizeof(line), "Light: %3d%%", st->light_percent);
    draw_line(2, line);

    const char *mode_str = (st->mode == LIGHT_MODE_AUTO)   ? "Auto"
                         : (st->mode == LIGHT_MODE_OFF)    ? "OFF"
                                                           : "Manual";
    snprintf(line, sizeof(line), "Mode: %s", mode_str);
    draw_line(3, line);
}

static void page_alarm(const sys_state_t *st)
{
    char line[32];
    int row = 0;

    for (int i = 0; i < ALARM_MAX_COUNT && row < 3; i++) {
        if (!st->alarms[i].enabled) continue;
        char wstr[10] = {0};
        if (st->alarms[i].repeat == ALARM_REPEAT_DAILY) {
            format_week(wstr, sizeof(wstr), st->alarms[i].week_mask);
        } else {
            snprintf(wstr, sizeof(wstr), "once");
        }
        snprintf(line, sizeof(line), "%d %02d:%02d %s", i + 1,
                 st->alarms[i].hour, st->alarms[i].minute, wstr);
        draw_line(row++, line);
    }

    if (row == 0) {
        draw_line(row++, "No alarm set");
    }

    if (st->timer.enabled) {
        snprintf(line, sizeof(line), "T:%02d:%02d-%02d:%02d", st->timer.hour_on,
                 st->timer.min_on, st->timer.hour_off, st->timer.min_off);
        draw_line(3, line);
    } else {
        draw_line(3, "Page 2/3");
    }
}

static void page_network(const sys_state_t *st)
{
    char line[32];

    uint64_t up_s = (esp_timer_get_time() - s_boot_time_us) / 1000000ULL;
    snprintf(line, sizeof(line), "Up:%02d:%02d:%02d", (int)(up_s / 3600),
             (int)((up_s % 3600) / 60), (int)(up_s % 60));
    draw_line(0, line);

    snprintf(line, sizeof(line), "IP:%s", st->ip_addr);
    draw_line(1, line);

    snprintf(line, sizeof(line), "WF:%s NTP:%s", st->wifi_connected ? "Y" : "N",
             st->time_synced ? "Y" : "N");
    draw_line(2, line);

    snprintf(line, sizeof(line), "Cld:%s Tmr:%s", st->cloud_connected ? "Y" : "N",
             st->timer.enabled ? "Y" : "N");
    draw_line(3, line);
}

static void render(void)
{
    if (s_oled == NULL) {
        return;
    }

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    sys_state_t st;
    sys_state_get(&st);

    ssd1306_clear_screen(s_oled, 0x00);

    if (s_msg_until_ms > now_ms && s_msg1[0] != '\0') {
        draw_line(0, s_msg1);
        if (s_msg2[0] != '\0') {
            draw_line(2, s_msg2);
        }
    } else {
        switch (st.oled_page) {
        case 1: page_main(&st);    break;
        case 2: page_alarm(&st);   break;
        case 3: page_network(&st); break;
        default: break;
        }
    }

    ssd1306_refresh_gram(s_oled);
}

/* -------------------------------------------------------------------------- */
/*                                  任务                                        */
/* -------------------------------------------------------------------------- */
static void oled_task(void *arg)
{
    EventGroupHandle_t events = sys_state_events();
    while (1) {
        render();
        xEventGroupWaitBits(events, SYS_EV_UI_REFRESH | SYS_EV_NET_CHANGED,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
    }
}

/* -------------------------------------------------------------------------- */
/*                                  生命周期                                    */
/* -------------------------------------------------------------------------- */
esp_err_t oled_ui_init(void)
{
    if (s_oled != NULL) {
        return ESP_OK;
    }

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_OLED_SDA_GPIO,
        .scl_io_num = BOARD_OLED_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_OLED_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(BOARD_OLED_I2C_PORT, &i2c_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 参数配置失败: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(BOARD_OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }

    s_oled = ssd1306_create(BOARD_OLED_I2C_PORT, SSD1306_I2C_ADDRESS);
    if (s_oled == NULL) {
        ESP_LOGE(TAG, "OLED 创建失败");
        return ESP_FAIL;
    }

    s_boot_time_us = esp_timer_get_time();
    ssd1306_clear_screen(s_oled, 0x00);
    ssd1306_draw_string(s_oled, 0, 0, (uint8_t *)"Smart Light", OLED_FONT_SIZE, 1);
    ssd1306_draw_string(s_oled, 0, 32, (uint8_t *)"Booting...", OLED_FONT_SIZE, 1);
    ssd1306_refresh_gram(s_oled);

    ESP_LOGI(TAG, "OLED 初始化完成 (SCL=%d SDA=%d)", BOARD_OLED_SCL_GPIO, BOARD_OLED_SDA_GPIO);
    return ESP_OK;
}

esp_err_t oled_ui_start(void)
{
    if (xTaskCreatePinnedToCore(oled_task, "oled_task", 4096, NULL, 3,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "OLED 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "OLED 刷新任务已启动");
    return ESP_OK;
}

void oled_ui_set_page(int page)
{
    sys_state_set_page(page);
}

int oled_ui_get_page(void)
{
    return sys_state_get_page();
}

void oled_ui_next_page(void)
{
    int p = sys_state_get_page();
    sys_state_set_page((p >= 3) ? 1 : p + 1);
}

void oled_ui_show_message(const char *line1, const char *line2, uint32_t duration_ms)
{
    if (line1) {
        strncpy(s_msg1, line1, sizeof(s_msg1) - 1);
        s_msg1[sizeof(s_msg1) - 1] = '\0';
    } else {
        s_msg1[0] = '\0';
    }
    if (line2) {
        strncpy(s_msg2, line2, sizeof(s_msg2) - 1);
        s_msg2[sizeof(s_msg2) - 1] = '\0';
    } else {
        s_msg2[0] = '\0';
    }
    s_msg_until_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + duration_ms;
    sys_state_notify(SYS_EV_UI_REFRESH);
}
