/**
 * @file sys_state.h
 * @brief 系统状态中心（唯一的“全局变量”持有者）
 *
 * 设计要点：
 *   - 所有模块不再持有自己的全局变量，统一读写这里的状态快照；
 *   - 内部用互斥锁保护，读写都是线程安全的；
 *   - 状态变化会置位事件组，灯光任务 / OLED 任务 / 云端任务等待事件即可，
 *     避免轮询，也避免模块之间互相调用形成耦合。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 基础类型 ==================================== */

/** 灯光工作模式 */
typedef enum {
    LIGHT_MODE_AUTO   = 0,   /**< 自动：温度定色 + 光照定亮度 */
    LIGHT_MODE_MANUAL = 1,   /**< 手动：固定颜色 */
    LIGHT_MODE_OFF    = 2    /**< 关闭 */
} light_mode_t;

/** 闹钟重复类型 */
typedef enum {
    ALARM_REPEAT_ONCE  = 0,  /**< 单次，触发后自动删除 */
    ALARM_REPEAT_DAILY = 1   /**< 重复（可配合 week_mask 做星期筛选） */
} alarm_repeat_t;

/**
 * 星期掩码：bit0=周日, bit1=周一 ... bit6=周六（与 struct tm::tm_wday 一致）
 * 串口命令里用户填写 1~7（1=周一 ... 7=周日），转换函数见 alarm_mgr。
 */
typedef struct {
    uint8_t  hour;
    uint8_t  minute;
    bool     enabled;
    alarm_repeat_t repeat;
    uint8_t  week_mask;      /**< 0 = 每天 */
} alarm_item_t;

/** 定时开关灯（支持跨天） */
typedef struct {
    uint8_t hour_on;
    uint8_t min_on;
    uint8_t hour_off;
    uint8_t min_off;
    bool    enabled;
    uint8_t week_mask;       /**< 0 = 每天 */
    bool    one_shot;        /**< true = 触发一次后自动禁用 */
} timer_item_t;

/** 系统状态快照 */
typedef struct {
    /* --- 灯光 --- */
    light_mode_t mode;
    uint8_t      rgb[3];         /**< 当前实际输出颜色 */
    uint8_t      manual_rgb[3];  /**< 手动模式颜色 */

    /* --- 传感器 --- */
    float temperature;           /**< 摄氏度 */
    bool  temp_valid;            /**< 温度是否有效（DS18B20 读到过有效值） */
    int   light_adc;             /**< 光敏原始 ADC 0~4095 */
    int   light_percent;         /**< 环境光照百分比 0~100（0=全暗 100=最亮） */

    /* --- 网络 --- */
    bool wifi_connected;
    bool time_synced;
    bool cloud_connected;
    char ip_addr[16];
    char ssid[33];

    /* --- UI --- */
    int  oled_page;              /**< 1~3 */

    /* --- 闹钟 / 定时器 --- */
    alarm_item_t alarms[3];
    timer_item_t timer;
} sys_state_t;

/* ============================ 事件位 ====================================== */
#define SYS_EV_MODE_CHANGED     BIT0   /**< 模式变化 */
#define SYS_EV_COLOR_CHANGED    BIT1   /**< 手动颜色变化 */
#define SYS_EV_SENSOR_UPDATED   BIT2   /**< 温/光采样更新 */
#define SYS_EV_NET_CHANGED      BIT3   /**< 网络状态变化 */
#define SYS_EV_ALARM_RINGING    BIT4   /**< 闹钟触发 */
#define SYS_EV_TIMER_CHANGED    BIT5   /**< 定时器配置变化 */
#define SYS_EV_POST_NOW         BIT6   /**< 请求立即上报云端 */
#define SYS_EV_UI_REFRESH       BIT7   /**< 请求刷新 OLED */

/* ============================ API ========================================= */

/** 初始化状态中心（必须在任何模块使用前调用） */
esp_err_t sys_state_init(void);

/** 获取状态快照（线程安全） */
void sys_state_get(sys_state_t *out);

/** 获取事件组句柄，用于 xEventGroupWaitBits */
EventGroupHandle_t sys_state_events(void);

/** 置位事件（通知关心该事件的模块） */
void sys_state_notify(uint32_t bits);

/* --- 灯光 --- */
void sys_state_set_mode(light_mode_t mode);
light_mode_t sys_state_get_mode(void);
void sys_state_set_manual_color(uint8_t r, uint8_t g, uint8_t b);
void sys_state_get_manual_color(uint8_t *r, uint8_t *g, uint8_t *b);
void sys_state_set_output_color(uint8_t r, uint8_t g, uint8_t b);
void sys_state_get_output_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* --- 传感器 --- */
void sys_state_set_sensor(float temperature, int light_adc, bool temp_valid);

/* --- 网络 --- */
void sys_state_set_wifi(bool connected, const char *ip, const char *ssid);
void sys_state_set_cloud(bool connected);
void sys_state_set_time_synced(bool synced);

/* --- UI --- */
void sys_state_set_page(int page);
int  sys_state_get_page(void);

/* --- 闹钟 / 定时器 --- */
esp_err_t sys_state_set_alarms(const alarm_item_t *alarms, int count);
void sys_state_get_alarms(alarm_item_t *alarms, int count);
esp_err_t sys_state_set_timer(const timer_item_t *timer);
void sys_state_get_timer(timer_item_t *timer);

#ifdef __cplusplus
}
#endif
