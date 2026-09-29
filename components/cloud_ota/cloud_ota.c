/**
 * @file cloud_ota.c
 * @brief HTTP(S) OTA 升级实现
 *
 * 流程：下载 → 读镜像自描述（校验 project_name）→ 写入备用分区 → finish。
 * 全程只操作"当前未运行"的那个 OTA 分区，任何一步失败都调用 abort 并直接返回，
 * 不切换启动分区，设备继续运行旧固件。
 *
 * 新固件首次启动后由 main.c 的 confirm_ota_image() 标记有效，取消 bootloader 回滚。
 */
#include "cloud_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cloud_ota";

static volatile bool s_running  = false;
static volatile int  s_progress = 0;
static char s_last_error[128] = "无";
static const esp_partition_t *s_running_part = NULL;

/* 任务参数：URL 必须深拷贝，否则调用者的局部数组出栈后就悬空了 */
typedef struct {
    cloud_ota_config_t pub;
    char url[512];
} ota_job_t;

/* -------------------------------------------------------------------------- */
/*                                  查询接口                                    */
/* -------------------------------------------------------------------------- */
bool cloud_ota_is_running(void) { return s_running; }

int cloud_ota_get_progress(void) { return s_progress; }

const char *cloud_ota_get_last_error(void) { return s_last_error; }

const char *cloud_ota_running_partition(void)
{
    if (s_running_part == NULL) {
        s_running_part = esp_ota_get_running_partition();
    }
    return (s_running_part != NULL) ? s_running_part->label : "?";
}

const char *cloud_ota_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc != NULL) ? desc->version : "?";
}

/* -------------------------------------------------------------------------- */
/*                                 升级主流程                                   */
/* -------------------------------------------------------------------------- */
static esp_err_t ota_do(const cloud_ota_config_t *cfg)
{
    ESP_LOGI(TAG, "========== OTA 升级开始 ==========");
    ESP_LOGI(TAG, "  当前版本  : %s     运行分区: %s",
             cloud_ota_running_version(), cloud_ota_running_partition());
    ESP_LOGI(TAG, "  固件地址  : %s", cfg->url);
    ESP_LOGI(TAG, "  升级前堆  : %lu 字节", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  提示      : 升级期间设备保持运行，数据不受影响");

    esp_http_client_config_t http_cfg = {
        .url               = cfg->url,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 走 https 时用内置 CA 包 */
        .timeout_ms        = (cfg->timeout_ms > 0) ? cfg->timeout_ms : 30000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "连接固件服务器失败 (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "  %s", s_last_error);
        ESP_LOGE(TAG, "  排查: 1) 服务器/start-broker.cmd 是否在跑 2) 网址是否写对");
        ESP_LOGE(TAG, "        3) 设备与服务器是否同一网段 4) 服务器 ota 目录里有没有这个 bin");
        return err;
    }

    /* 读镜像里的自描述信息 —— 用来确认"确实拉到了新版本"，而不是缓存/同名旧包 */
    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
        ESP_LOGI(TAG, "  服务器固件: %s  v%s  编译于 %s %s",
                 new_desc.project_name, new_desc.version, new_desc.date, new_desc.time);
        if (strcmp(new_desc.project_name, "smart_light") != 0) {
            ESP_LOGW(TAG, "  警告: 固件名不是 smart_light，可能拉错了文件！");
        }
    }

    const int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "  固件大小  : %d 字节", total);

    int last_report = -10;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;                          /* 下载结束（成功或失败） */
        }
        const int rd = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            s_progress = (int)((int64_t)rd * 100 / total);
        }
        if (s_progress >= last_report + 10) {
            last_report = s_progress;
            ESP_LOGI(TAG, "  下载中 %3d%%   (%d/%d 字节)", s_progress, rd, total);
        }
    }

    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "下载中断 (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "  %s", s_last_error);
        esp_https_ota_abort(handle);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        snprintf(s_last_error, sizeof(s_last_error), "固件数据不完整（传输被截断）");
        ESP_LOGE(TAG, "  %s", s_last_error);
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "写入/校验失败 (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "  %s", s_last_error);
        return err;
    }

    s_progress = 100;
    ESP_LOGI(TAG, "  ✔ OTA 成功：新固件已写入备用分区，重启后生效");
    ESP_LOGI(TAG, "  升级后堆  : %lu 字节", (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

static void ota_task(void *arg)
{
    ota_job_t *job = (ota_job_t *)arg;
    const esp_err_t err = ota_do(&job->pub);
    const bool reboot = job->pub.reboot;
    free(job);

    if (err == ESP_OK && reboot) {
        ESP_LOGI(TAG, "  5 秒后自动重启进入新固件…");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    } else if (err != ESP_OK) {
        /* 关键：失败时不重启、不切换分区，设备继续跑旧固件 */
        ESP_LOGE(TAG, "OTA 失败，设备继续运行旧固件（v%s）",
                 cloud_ota_running_version());
    }

    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t cloud_ota_start(const cloud_ota_config_t *cfg)
{
    if (cfg == NULL || cfg->url == NULL || strlen(cfg->url) == 0) {
        ESP_LOGE(TAG, "固件地址为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "已有 OTA 正在进行中，忽略本次请求");
        return ESP_ERR_INVALID_STATE;
    }

    ota_job_t *job = (ota_job_t *)calloc(1, sizeof(ota_job_t));
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }
    job->pub = *cfg;
    strncpy(job->url, cfg->url, sizeof(job->url) - 1);
    job->pub.url = job->url;

    s_running  = true;
    s_progress = 0;
    snprintf(s_last_error, sizeof(s_last_error), "无");

    if (xTaskCreatePinnedToCore(ota_task, "cloud_ota", 8192, job, 5,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        free(job);
        s_running = false;
        ESP_LOGE(TAG, "OTA 任务创建失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
