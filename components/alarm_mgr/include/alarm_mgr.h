/**
 * @file alarm_mgr.h
 * @brief 闹钟 & 定时开关灯管理（FreeRTOS 任务，1 秒节拍）
 *
 * 闹钟：最多 3 个，支持 单次 / 每日 / 星期筛选，触发时置位 SYS_EV_ALARM_RINGING，
 *       由 light_ctrl 负责红灯闪烁 + 蜂鸣器响铃。
 * 定时器：支持跨天（如 22:00-06:00）、星期筛选，自动在 AUTO / OFF 之间切换。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sys_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（清空闹钟与定时器） */
esp_err_t alarm_mgr_init(void);

/** 启动 1 秒节拍的检查任务 */
esp_err_t alarm_mgr_start(void);

/* ------------------------------- 闹钟 ------------------------------------ */

/**
 * @brief 添加闹钟
 * @return 成功返回槽位号(1~3)，失败返回 -1
 */
int alarm_mgr_add(uint8_t hour, uint8_t minute, alarm_repeat_t repeat, uint8_t week_mask);

/** 删除指定槽位（1~3） */
esp_err_t alarm_mgr_del(int slot);

/** 删除全部 */
esp_err_t alarm_mgr_del_all(void);

/** 打印当前闹钟列表 */
void alarm_mgr_print(void);

/** 测试用：立即触发一次响铃 */
void alarm_mgr_trigger_now(void);

/* ------------------------------- 定时器 ---------------------------------- */

esp_err_t alarm_mgr_timer_set(uint8_t h_on, uint8_t m_on, uint8_t h_off, uint8_t m_off);
esp_err_t alarm_mgr_timer_disable(void);
esp_err_t alarm_mgr_timer_set_week(uint8_t week_mask);
void alarm_mgr_timer_print(void);

/* ------------------------------- 工具 ------------------------------------ */

/**
 * @brief 解析星期字符串，如 "1,3,5"（1=周一 ... 7=周日）
 * @return 位掩码 bit0=周日 ... bit6=周六
 */
uint8_t alarm_mgr_parse_weekdays(const char *str);

#ifdef __cplusplus
}
#endif
