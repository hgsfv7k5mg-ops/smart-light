/**
 * @file oled_ui.h
 * @brief SSD1306 OLED 显示（3 页面，独立刷新任务）
 *
 *   Page 1 : 主界面 —— 时间 / 温度 / 环境光 / 工作模式
 *   Page 2 : 闹钟 & 定时器
 *   Page 3 : 网络状态 —— 运行时长 / IP / WiFi / SNTP / 云端 / 定时器
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2C + OLED */
esp_err_t oled_ui_init(void);

/** 启动刷新任务（500ms 周期，也可被事件唤醒） */
esp_err_t oled_ui_start(void);

void oled_ui_set_page(int page);
int  oled_ui_get_page(void);

/** 翻到下一页（循环） */
void oled_ui_next_page(void);

/**
 * @brief 全屏显示一条临时提示（配网、OTA 等），duration_ms 后自动恢复原页面
 */
void oled_ui_show_message(const char *line1, const char *line2, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
