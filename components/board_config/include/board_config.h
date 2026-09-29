/**
 * @file board_config.h
 * @brief 板级硬件配置：引脚、外设参数集中定义
 *
 * 硬件：ESP32-S3-N16R8
 * 修改接线只需改本文件。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ RGB LED (共阳极) ============================ */
/* 共阳极：输出低电平点亮 -> LEDC 通道需要 output_invert = 1 */
#define BOARD_RGB_R_GPIO        GPIO_NUM_18
#define BOARD_RGB_G_GPIO        GPIO_NUM_16
#define BOARD_RGB_B_GPIO        GPIO_NUM_17
#define BOARD_RGB_INVERT        1           /* 1=共阳极  0=共阴极 */

/* ============================ 光敏传感器 (ADC) ============================ */
#define BOARD_LIGHT_ADC_UNIT    ADC_UNIT_1
#define BOARD_LIGHT_ADC_CHAN    ADC_CHANNEL_3   /* GPIO4 = ADC1_CH3 */
#define BOARD_LIGHT_ADC_GPIO    GPIO_NUM_4
#define BOARD_LIGHT_ADC_ATTEN   ADC_ATTEN_DB_12
#define BOARD_LIGHT_ADC_WIDTH   ADC_BITWIDTH_DEFAULT
#define BOARD_LIGHT_ADC_MAX     4095

/**
 * 光敏模块方向校准：
 *   0 -> ADC 值越大表示环境越暗（默认）
 *   1 -> ADC 值越大表示环境越亮（如果模块相反，改成 1）
 */
#define BOARD_LIGHT_SENSOR_INVERT   0

/* ============================ DS18B20 (1-Wire) ============================ */
#define BOARD_DS18B20_GPIO      GPIO_NUM_9      /* 需 4.7k 上拉 */
#define BOARD_DS18B20_MAX_DEV   1

/**
 * 1-Wire 总线后端选择：
 *   1 = RMT（默认，官方示例用法）
 *   2 = UART（备选）
 *
 * RMT 后端在 reset 时只允许接收 2 个 RMT 符号，总线电平不干净（上拉太弱、
 * 线太长、干扰）时会报 "user buffer too small, received symbols truncated"
 * 并导致 "reset bus failed: no devices found"。
 * 如果出现该报错且确认上拉没问题，把这里改成 2 换 UART 后端试试。
 */
#define BOARD_ONEWIRE_BACKEND       1
#define BOARD_ONEWIRE_UART_PORT     UART_NUM_1  /* 后端=2 时生效（TX/RX 复用同一 GPIO） */

/* ============================ OLED SSD1306 (I2C) ========================== */
#define BOARD_OLED_I2C_PORT     I2C_NUM_0
#define BOARD_OLED_SCL_GPIO     GPIO_NUM_7
#define BOARD_OLED_SDA_GPIO     GPIO_NUM_8
#define BOARD_OLED_I2C_FREQ_HZ 400000

/* ============================ 蜂鸣器 ====================================== */
#define BOARD_BUZZER_GPIO       GPIO_NUM_6
#define BOARD_BUZZER_FREQ_HZ   2000            /* 2kHz 方波 */

/* ============================ LEDC PWM ==================================== */
#define BOARD_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define BOARD_LEDC_RES          LEDC_TIMER_8_BIT     /* duty 0~255 */
#define BOARD_LEDC_DUTY_MAX     255

/* RGB 使用 TIMER_0 / CH0~2 */
#define BOARD_LEDC_RGB_TIMER    LEDC_TIMER_0
#define BOARD_LEDC_CH_R         LEDC_CHANNEL_0
#define BOARD_LEDC_CH_G         LEDC_CHANNEL_1
#define BOARD_LEDC_CH_B         LEDC_CHANNEL_2

/* 蜂鸣器使用独立定时器（频率不同） */
#define BOARD_LEDC_BUZZ_TIMER   LEDC_TIMER_1
#define BOARD_LEDC_BUZZ_CH      LEDC_CHANNEL_3
#define BOARD_BUZZER_DUTY       128             /* 50% 占空比 */

/* ============================ 温度 -> 颜色映射区间 ========================= */
#define TEMP_COLD_LOW           (-10.0f)        /* 最冷，纯蓝 */
#define TEMP_COLD_MID           15.0f           /* 蓝 -> 绿 结束 */
#define TEMP_WARM_MID           30.0f           /* 绿 -> 红 结束 */
#define TEMP_HOT_SPAN           15.0f           /* 红区渐变量程 */

/* ============================ 自动调光参数 ================================= */
#define AUTO_LIGHT_MIN_FACTOR   0.10f           /* 环境最亮时仍保留的最低亮度 */
#define AUTO_LIGHT_MAX_FACTOR   1.00f

/* ============================ 闹钟 ======================================== */
#define ALARM_MAX_COUNT         3               /* 最多 3 个闹钟 */
#define ALARM_RING_MS           10000           /* 响铃时长 10s */
#define ALARM_BLINK_MS          300             /* 红闪半周期 */

#ifdef __cplusplus
}
#endif
