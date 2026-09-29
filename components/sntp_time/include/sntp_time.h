/**
 * @file sntp_time.h
 * @brief SNTP 网络校时（北京时间 UTC+8），供闹钟与定时器使用
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 SNTP（非阻塞，后台持续同步） */
esp_err_t sntp_time_init(void);

/** 阻塞等待同步成功 */
bool sntp_time_wait_sync(uint32_t timeout_ms);

bool sntp_time_is_synced(void);

#ifdef __cplusplus
}
#endif
