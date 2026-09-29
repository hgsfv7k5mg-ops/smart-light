/**
 * @file buzzer.h
 * @brief 蜂鸣器驱动（2kHz PWM 方波）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t buzzer_init(void);
esp_err_t buzzer_set(bool on);

/** 阻塞式短响（内部用 vTaskDelay，请勿在 ISR 调用） */
esp_err_t buzzer_beep(uint32_t ms);

#ifdef __cplusplus
}
#endif
