/**
 * @file rgb_led.h
 * @brief RGB 全彩灯驱动（LEDC PWM）
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RGB 三路 PWM
 */
esp_err_t rgb_led_init(void);

/**
 * @brief 设置颜色（0~255）
 */
esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 熄灭
 */
esp_err_t rgb_led_off(void);

/**
 * @brief 读回当前占空比
 */
esp_err_t rgb_led_get(uint8_t *r, uint8_t *g, uint8_t *b);

#ifdef __cplusplus
}
#endif
