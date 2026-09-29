/**
 * @file buzzer.c
 * @brief 蜂鸣器驱动实现：LEDC 输出固定频率方波
 *
 * 使用与 RGB 不同的 LEDC 定时器（BOARD_LEDC_BUZZ_TIMER），
 * 因为蜂鸣器需要 2 kHz 而 RGB 用的是 PWM 调光频率。
 */
#include "buzzer.h"

#include "board_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buzzer";
static bool s_initialized = false;

esp_err_t buzzer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BOARD_LEDC_MODE,
        .duty_resolution = BOARD_LEDC_RES,
        .timer_num       = BOARD_LEDC_BUZZ_TIMER,
        .freq_hz         = BOARD_BUZZER_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .speed_mode = BOARD_LEDC_MODE,
        .channel    = BOARD_LEDC_BUZZ_CH,
        .timer_sel  = BOARD_LEDC_BUZZ_TIMER,
        .gpio_num   = BOARD_BUZZER_GPIO,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    buzzer_set(false);
    s_initialized = true;
    ESP_LOGI(TAG, "蜂鸣器初始化完成 (GPIO%d, %dHz)", BOARD_BUZZER_GPIO, BOARD_BUZZER_FREQ_HZ);
    return ESP_OK;
}

esp_err_t buzzer_set(bool on)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ledc_set_duty(BOARD_LEDC_MODE, BOARD_LEDC_BUZZ_CH, on ? BOARD_BUZZER_DUTY : 0);
    ledc_update_duty(BOARD_LEDC_MODE, BOARD_LEDC_BUZZ_CH);
    return ESP_OK;
}

esp_err_t buzzer_beep(uint32_t ms)
{
    buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_set(false);
    return ESP_OK;
}
