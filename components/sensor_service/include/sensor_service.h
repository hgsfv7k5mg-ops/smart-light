/**
 * @file sensor_service.h
 * @brief 传感器采集服务（FreeRTOS 任务）
 *
 * 光照每 500ms 采样一次（快），温度每 3s 采样一次（DS18B20 转换慢）。
 * 采样结果写入 sys_state 并置位 SYS_EV_SENSOR_UPDATED，灯光任务与云端任务据此更新。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_LIGHT_PERIOD_MS  500
#define SENSOR_TEMP_PERIOD_MS   3000

/** 启动采集任务 */
esp_err_t sensor_service_start(void);

/** 运行时调整采样周期 */
void sensor_service_set_period(uint32_t light_ms, uint32_t temp_ms);

#ifdef __cplusplus
}
#endif
