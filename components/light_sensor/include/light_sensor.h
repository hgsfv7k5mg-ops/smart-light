/**
 * @file light_sensor.h
 * @brief 光敏电阻传感器（ADC1 单次采样）
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ADC 单元与通道 */
esp_err_t light_sensor_init(void);

/** 读取一次原始 ADC 值（0~4095） */
esp_err_t light_sensor_read_raw(int *raw);

/**
 * 读取环境光照百分比 0~100
 * 方向由 board_config.h 中 BOARD_LIGHT_SENSOR_INVERT 决定
 */
esp_err_t light_sensor_read_percent(int *percent);

#ifdef __cplusplus
}
#endif
