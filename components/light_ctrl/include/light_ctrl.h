/**
 * @file light_ctrl.h
 * @brief 灯光控制任务：灯光状态机 + 闹钟响铃效果
 *
 * 状态机三种模式：
 *   AUTO   —— 温度决定颜色（蓝→绿→红），环境光决定亮度（越暗越亮）
 *   MANUAL —— 输出手动设定的颜色
 *   OFF    —— 熄灭
 *
 * 闹钟触发时接管输出 10 秒：红灯闪烁 + 蜂鸣器鸣响。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动灯光控制任务 */
esp_err_t light_ctrl_start(void);

/** 温度 -> RGB 映射（低温蓝 → 常温绿 → 高温红） */
void light_ctrl_temp_to_rgb(float temperature, uint8_t out[3]);

/** 立即请求一次输出刷新 */
void light_ctrl_request_update(void);

#ifdef __cplusplus
}
#endif
