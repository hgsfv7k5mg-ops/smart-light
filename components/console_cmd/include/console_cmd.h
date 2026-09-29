/**
 * @file console_cmd.h
 * @brief 串口命令控制台（独立于主循环的任务）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动串口命令任务 */
esp_err_t console_cmd_start(void);

#ifdef __cplusplus
}
#endif
