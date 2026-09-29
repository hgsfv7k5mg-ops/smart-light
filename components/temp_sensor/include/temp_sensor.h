/**
 * @file temp_sensor.h
 * @brief DS18B20 温度传感器驱动（1-Wire / RMT）
 *
 * 约定：
 *   - 读到上电默认值 85℃ 时自动重新转换一次，不作为致命错误返回；
 *   - 1-Wire 时序对任务抢占敏感，总线事务期间本模块临时提高当前任务优先级；
 *   - 未找到设备时 init 返回 ESP_FAIL，便于上层给出醒目提示。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 1-Wire 总线并枚举 DS18B20（内部重试 3 次）
 * @return ESP_OK 成功；ESP_FAIL 总线上找不到可用设备
 */
esp_err_t temp_sensor_init(void);

/** DS18B20 是否可用 */
bool temp_sensor_is_ready(void);

/**
 * @brief 重新扫描 1-Wire 总线（热插拔恢复）
 *
 * 若设备已就绪则直接返回；否则清掉上次失败的句柄后重新枚举。
 * 用于"上电时线没接好，插好后不想重启"的场景：插好 DQ/VCC/GND，
 * 串口敲 TEMP 即可当场恢复，采集任务下一轮就会自动开始读温度。
 */
esp_err_t temp_sensor_rescan(void);

/**
 * @brief 读取温度（阻塞约 800ms，内含转换等待与失败重试）
 * @param[out] celsius 摄氏度
 */
esp_err_t temp_sensor_read(float *celsius);

/**
 * @brief 直接读 9 字节 scratchpad（诊断用，可看出是不是 85℃ 上电值）
 * @param[out] out 9 字节缓冲
 */
esp_err_t temp_sensor_read_raw(uint8_t out[9]);

/**
 * @brief 单独做一次总线复位探测（不读温度）
 * @param[out] present true = 有设备回应 presence 脉冲
 * @note 用来区分"线没接/没上电"和"接了但通信出错"
 */
esp_err_t temp_sensor_probe_presence(bool *present);

/**
 * @brief 生成一行诊断信息（地址 / 成功次数 / 失败次数 / 最近错误原因）
 * @return 写入 buf 的字符数
 */
int temp_sensor_diag(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
