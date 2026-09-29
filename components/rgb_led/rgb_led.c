/**
 * @file rgb_led.c
 * @brief RGB 灯驱动实现：三路 LEDC PWM，共用 TIMER_0
 *
 * 共阳极接法（BOARD_RGB_INVERT=1）时 LEDC 输出取反：duty 越大越暗，
 * 因此 rgb_led_set(0,0,0) 表示熄灭，与"0 = 关灯"的语义一致。
 */
#include "rgb_led.h"

#include "board_config.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "rgb_led";

static bool s_initialized = false;

static esp_err_t config_channel(ledc_channel_t ch, gpio_num_t gpio)
{
    ledc_channel_config_t cfg = {
        .speed_mode = BOARD_LEDC_MODE,
        .channel    = ch,
        .timer_sel  = BOARD_LEDC_RGB_TIMER,
        .gpio_num   = gpio,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = BOARD_RGB_INVERT,
    };
    return ledc_channel_config(&cfg);
}

esp_err_t rgb_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BOARD_LEDC_MODE,
        .duty_resolution = BOARD_LEDC_RES,
        .timer_num       = BOARD_LEDC_RGB_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ESP_ERROR_CHECK(config_channel(BOARD_LEDC_CH_R, BOARD_RGB_R_GPIO));
    ESP_ERROR_CHECK(config_channel(BOARD_LEDC_CH_G, BOARD_RGB_G_GPIO));
    ESP_ERROR_CHECK(config_channel(BOARD_LEDC_CH_B, BOARD_RGB_B_GPIO));

    rgb_led_off();
    s_initialized = true;
    ESP_LOGI(TAG, "RGB 初始化完成 (R=%d G=%d B=%d, 共阳极=%d)",
             BOARD_RGB_R_GPIO, BOARD_RGB_G_GPIO, BOARD_RGB_B_GPIO, BOARD_RGB_INVERT);
    return ESP_OK;
}

esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ledc_set_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_R, r);
    ledc_set_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_G, g);
    ledc_set_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_B, b);
    ledc_update_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_R);
    ledc_update_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_G);
    ledc_update_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_B);
    return ESP_OK;
}

esp_err_t rgb_led_off(void)
{
    return rgb_led_set(0, 0, 0);
}

esp_err_t rgb_led_get(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (r) *r = ledc_get_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_R);
    if (g) *g = ledc_get_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_G);
    if (b) *b = ledc_get_duty(BOARD_LEDC_MODE, BOARD_LEDC_CH_B);
    return ESP_OK;
}
