/**
 * @file cloud_ota.h
 * @brief 通用 OTA 升级（HTTP / HTTPS 均可）
 *
 *  不绑定任何云平台：
 *  只要给出一个能下载固件 .bin 的 URL 即可，本地 HTTP 服务、GitHub、对象存储都能用。
 *
 *  典型用法：
 *      cloud_ota_config_t cfg = { .url = "http://192.168.1.100:8083/ota/smart_light.bin" };
 *      cloud_ota_start(&cfg);      // 立即返回，升级在后台任务中进行
 *      // 之后可用 cloud_ota_get_progress() 查询进度，成功后自动重启
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;        /**< 固件地址，http:// 或 https:// 均可 */
    bool        reboot;     /**< 写入成功后是否自动重启（true = 自动重启） */
    int         timeout_ms; /**< 网络超时，0 表示默认 30 秒 */
} cloud_ota_config_t;

/**
 * @brief 启动一次 OTA 升级（非阻塞，内部起独立任务）
 *
 * @return ESP_OK           已开始
 *         ESP_ERR_INVALID_STATE 已有升级在进行
 */
esp_err_t cloud_ota_start(const cloud_ota_config_t *cfg);

/** 是否正在升级 */
bool cloud_ota_is_running(void);

/** 当前进度 0~100 */
int cloud_ota_get_progress(void);

/** 最近一次失败原因（失败时有效，成功或未开始时为 "无"） */
const char *cloud_ota_get_last_error(void);

/** 当前运行分区名（ota_0 / ota_1） */
const char *cloud_ota_running_partition(void);

/** 当前固件版本号 */
const char *cloud_ota_running_version(void);

#ifdef __cplusplus
}
#endif
