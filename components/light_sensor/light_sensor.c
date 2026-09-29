/**
 * @file light_sensor.c
 * @brief 光敏电阻采集实现：ADC1 单次（oneshot）采样
 *
 * 输出 0~100 的"环境亮度百分比"，方向由 BOARD_LIGHT_SENSOR_INVERT 决定：
 *   0 -> ADC 越大越暗（默认），反向换算；
 *   1 -> ADC 越大越亮，直接换算。
 */
#include "light_sensor.h"

#include "board_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "light_sensor";
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

esp_err_t light_sensor_init(void)
{
    if (s_adc_handle != NULL) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BOARD_LIGHT_ADC_UNIT,
        .clk_src  = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 单元创建失败: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BOARD_LIGHT_ADC_ATTEN,
        .bitwidth = BOARD_LIGHT_ADC_WIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BOARD_LIGHT_ADC_CHAN, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "光敏 ADC 初始化完成 (GPIO%d, ADC1_CH%d)",
             BOARD_LIGHT_ADC_GPIO, BOARD_LIGHT_ADC_CHAN);
    return ESP_OK;
}

esp_err_t light_sensor_read_raw(int *raw)
{
    if (s_adc_handle == NULL || raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return adc_oneshot_read(s_adc_handle, BOARD_LIGHT_ADC_CHAN, raw);
}

/* 原始 ADC 值 -> 百分比。BOARD_LIGHT_SENSOR_INVERT 在编译期决定换算方向，
   结果钳位到 0~100，避免极端读数影响调光计算。 */
esp_err_t light_sensor_read_percent(int *percent)
{
    int raw = 0;
    esp_err_t err = light_sensor_read_raw(&raw);
    if (err != ESP_OK) {
        return err;
    }
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if BOARD_LIGHT_SENSOR_INVERT
    int pct = (raw * 100) / BOARD_LIGHT_ADC_MAX;
#else
    int pct = 100 - (raw * 100) / BOARD_LIGHT_ADC_MAX;
#endif
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *percent = pct;
    return ESP_OK;
}
