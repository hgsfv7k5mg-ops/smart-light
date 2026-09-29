/**
 * @file sensor_service.c
 * @brief 传感器采集任务实现
 *
 * 任务以 200 ms 节拍轮询，光照与温度各自按独立周期采集（默认 500 ms / 3 s），
 * 温度转换耗时较长，因此两者分开计量避免互相拖慢。
 * 采集结果统一写入 sys_state，由状态中心通知其它模块。
 */
#include "sensor_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "light_sensor.h"
#include "sys_state.h"
#include "temp_sensor.h"

static const char *TAG = "sensor_service";

static uint32_t s_light_period_ms = SENSOR_LIGHT_PERIOD_MS;
static uint32_t s_temp_period_ms  = SENSOR_TEMP_PERIOD_MS;

static void sensor_task(void *arg)
{
    TickType_t last_light = 0;
    TickType_t last_temp  = 0;
    float temperature = 0.0f;
    int light_raw = 0;
    bool temp_valid = false;         /* 是否曾经读到过有效温度 */
    uint32_t fail_streak = 0;        /* 连续失败次数，用于降频打日志 */

    while (1) {
        TickType_t now = xTaskGetTickCount();

        /* ---------- 光照 ---------- */
        if ((now - last_light) >= pdMS_TO_TICKS(s_light_period_ms)) {
            last_light = now;
            if (light_sensor_read_raw(&light_raw) != ESP_OK) {
                light_raw = 0;
            }
        }

        /* ---------- 温度 ---------- */
        if ((now - last_temp) >= pdMS_TO_TICKS(s_temp_period_ms)) {
            last_temp = now;
            if (temp_sensor_is_ready()) {
                float t = 0.0f;
                if (temp_sensor_read(&t) == ESP_OK) {
                    temperature = t;
                    temp_valid = true;
                    fail_streak = 0;
                    ESP_LOGI(TAG, "温度 %.2f C", temperature);
                } else {
                    fail_streak++;
                    /* 前 3 次每次都报，之后每 10 次报一次，避免刷屏 */
                    if (fail_streak <= 3 || (fail_streak % 10) == 0) {
                        char diag[160];
                        temp_sensor_diag(diag, sizeof(diag));
                        ESP_LOGW(TAG, "温度读取失败（连续 %lu 次）: %s",
                                 (unsigned long)fail_streak, diag);
                    }
                }
            }
        }

        /* ---------- 写入状态中心 ---------- */
        sys_state_set_sensor(temperature, light_raw, temp_valid);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t sensor_service_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(sensor_task, "sensor_task", 3072,
                                             NULL, 4, NULL, tskNO_AFFINITY);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "传感器任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "传感器采集任务已启动 (光照 %lums / 温度 %lums)",
             (unsigned long)s_light_period_ms, (unsigned long)s_temp_period_ms);
    return ESP_OK;
}

void sensor_service_set_period(uint32_t light_ms, uint32_t temp_ms)
{
    if (light_ms >= 100) s_light_period_ms = light_ms;
    if (temp_ms >= 1000) s_temp_period_ms = temp_ms;
}
