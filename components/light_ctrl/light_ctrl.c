/**
 * @file light_ctrl.c
 * @brief 灯光控制任务实现：目标颜色计算 + 渐变输出 + 闹钟响铃效果
 *
 * 输出不直接跳变，而是每拍向目标颜色靠近 CTRL_RAMP_STEP，实现平滑过渡。
 * 闹钟响铃期间（SYS_EV_ALARM_RINGING）接管输出：红灯闪烁 + 蜂鸣器同步鸣响，
 * 持续 ALARM_RING_MS 后恢复常规状态机。
 */
#include "light_ctrl.h"

#include <math.h>
#include "board_config.h"
#include "buzzer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "rgb_led.h"
#include "sys_state.h"

static const char *TAG = "light_ctrl";

#define CTRL_TICK_MS        200     /* 常规刷新周期 */
#define CTRL_RING_TICK_MS   100     /* 响铃期间刷新周期 */
#define CTRL_RAMP_STEP      24      /* 渐变步长（越大越快） */

#define CTRL_WAIT_BITS  (SYS_EV_MODE_CHANGED | SYS_EV_COLOR_CHANGED | \
                         SYS_EV_SENSOR_UPDATED | SYS_EV_ALARM_RINGING)

static uint8_t s_cur[3] = {0, 0, 0};

/* -------------------------------------------------------------------------- */
/*                              温度 -> 颜色                                   */
/* -------------------------------------------------------------------------- */
void light_ctrl_temp_to_rgb(float temperature, uint8_t out[3])
{
    float ratio;
    int r, g, b;

    if (temperature <= TEMP_COLD_MID) {
        /* 冷区：纯蓝 -> 青绿        (<=15℃) */
        ratio = (temperature - TEMP_COLD_LOW) / (TEMP_COLD_MID - TEMP_COLD_LOW);
        ratio = (ratio < 0.0f) ? 0.0f : (ratio > 1.0f) ? 1.0f : ratio;
        r = 0;
        g = (int)(ratio * 255.0f);
        b = 255;
    } else if (temperature <= TEMP_WARM_MID) {
        /* 常温区：青绿 -> 纯绿      (15~30℃) */
        ratio = (temperature - TEMP_COLD_MID) / (TEMP_WARM_MID - TEMP_COLD_MID);
        ratio = (ratio < 0.0f) ? 0.0f : (ratio > 1.0f) ? 1.0f : ratio;
        r = 0;
        g = 255;
        b = (int)((1.0f - ratio) * 255.0f);
    } else {
        /* 高温区：纯绿 -> 纯红      (>30℃) */
        ratio = (temperature - TEMP_WARM_MID) / TEMP_HOT_SPAN;
        ratio = (ratio < 0.0f) ? 0.0f : (ratio > 1.0f) ? 1.0f : ratio;
        r = (int)(ratio * 255.0f);
        g = (int)((1.0f - ratio) * 255.0f);
        b = 0;
    }

    out[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    out[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
    out[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

/* -------------------------------------------------------------------------- */
/*                              渐变输出                                       */
/* -------------------------------------------------------------------------- */
static void ramp_to(const uint8_t target[3], uint8_t step)
{
    for (int i = 0; i < 3; i++) {
        if (s_cur[i] < target[i]) {
            s_cur[i] = (uint8_t)((s_cur[i] + step > target[i]) ? target[i] : s_cur[i] + step);
        } else if (s_cur[i] > target[i]) {
            s_cur[i] = (uint8_t)((s_cur[i] < step || s_cur[i] - step < target[i]) ? target[i] : s_cur[i] - step);
        }
    }
    rgb_led_set(s_cur[0], s_cur[1], s_cur[2]);
    sys_state_set_output_color(s_cur[0], s_cur[1], s_cur[2]);
}

/* -------------------------------------------------------------------------- */
/*                              目标颜色计算                                   */
/* -------------------------------------------------------------------------- */
static void compute_target(uint8_t target[3])
{
    sys_state_t st;
    sys_state_get(&st);

    switch (st.mode) {
    case LIGHT_MODE_MANUAL:
        target[0] = st.manual_rgb[0];
        target[1] = st.manual_rgb[1];
        target[2] = st.manual_rgb[2];
        break;

    case LIGHT_MODE_OFF:
        target[0] = 0;
        target[1] = 0;
        target[2] = 0;
        break;

    case LIGHT_MODE_AUTO:
    default: {
        uint8_t base[3];
        light_ctrl_temp_to_rgb(st.temperature, base);

        /* 环境光百分比 0=全暗 100=最亮 -> 越暗灯越亮 */
        float factor = 1.0f - (float)st.light_percent / 100.0f;
        if (factor < AUTO_LIGHT_MIN_FACTOR) factor = AUTO_LIGHT_MIN_FACTOR;
        if (factor > AUTO_LIGHT_MAX_FACTOR) factor = AUTO_LIGHT_MAX_FACTOR;

        target[0] = (uint8_t)(base[0] * factor);
        target[1] = (uint8_t)(base[1] * factor);
        target[2] = (uint8_t)(base[2] * factor);
        break;
    }
    }
}

/* -------------------------------------------------------------------------- */
/*                                 任务                                        */
/* -------------------------------------------------------------------------- */
static void light_ctrl_task(void *arg)
{
    EventGroupHandle_t events = sys_state_events();
    uint8_t target[3] = {0, 0, 0};

    bool ringing = false;
    uint32_t ring_start_ms = 0;
    uint32_t last_toggle_ms = 0;
    bool ring_led_on = false;

    while (1) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        EventBits_t bits = xEventGroupWaitBits(events, CTRL_WAIT_BITS,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(ringing ? CTRL_RING_TICK_MS
                                                                     : CTRL_TICK_MS));

        if (bits & SYS_EV_ALARM_RINGING) {
            if (!ringing) {
                ringing = true;
                ring_start_ms = now_ms;
                last_toggle_ms = 0;
                ring_led_on = false;
                ESP_LOGI(TAG, "闹钟响铃开始，持续 %d ms", ALARM_RING_MS);
            }
        }

        /* ---------------- 响铃效果：红灯闪烁 + 蜂鸣 ---------------- */
        if (ringing) {
            if ((now_ms - ring_start_ms) >= ALARM_RING_MS) {
                ringing = false;
                buzzer_set(false);
                rgb_led_off();
                s_cur[0] = s_cur[1] = s_cur[2] = 0;
                ESP_LOGI(TAG, "闹钟响铃结束");
            } else if ((now_ms - last_toggle_ms) >= ALARM_BLINK_MS) {
                last_toggle_ms = now_ms;
                ring_led_on = !ring_led_on;
                if (ring_led_on) {
                    rgb_led_set(255, 0, 0);
                    buzzer_set(true);
                } else {
                    rgb_led_set(0, 0, 0);
                    buzzer_set(false);
                }
            }
            continue;
        }

        /* ---------------- 常规状态机 ---------------- */
        compute_target(target);
        ramp_to(target, CTRL_RAMP_STEP);
    }
}

esp_err_t light_ctrl_start(void)
{
    if (xTaskCreatePinnedToCore(light_ctrl_task, "light_ctrl", 3072, NULL, 5,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "灯光控制任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "灯光控制任务已启动");
    return ESP_OK;
}

void light_ctrl_request_update(void)
{
    sys_state_notify(SYS_EV_SENSOR_UPDATED);
}
