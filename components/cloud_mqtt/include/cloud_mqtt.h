/**
 * @file cloud_mqtt.h
 * @brief 通用 MQTT 接入：可连接 EMQX / Mosquitto / ThingsBoard 等任意标准 Broker
 *
 * 设计取向：
 *   - 不绑定云厂商，不需要三元组签名（HMAC），Broker 地址直接配；
 *   - Topic 用简洁的 light/<device_id>/... 结构，便于规则引擎对接；
 *   - 支持可选用户名/密码（多数 Broker 默认允许匿名）。
 *
 * 主题约定（device_id 为空时自动用 MAC 尾号）：
 *   上报   light/<id>/telemetry           JSON: {temp, light, mode, r, g, b}
 *   状态   light/<id>/state               JSON: {mode, r, g, b} 或 "ON"/"OFF"
 *   控制   light/<id>/set                 JSON: {"mode":1} / {"r":..,"g":..,"b":..}
 *   可用性 light/<id>/status              "online" / "offline"（LWT 遗嘱）
 *
 * 与 ThingsBoard 对接时：
 *   把 broker 指向 TB 的 MQTT 端口，用户名 = 设备 Access Token，
 *   telemetry topic 直接用 TB 的 v1/devices/me/telemetry（见配置项 tb_mode）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *uri;              /**< 如 "mqtt://192.168.1.100:1883" */
    const char *username;         /**< 可空 */
    const char *password;         /**< 可空 */
    const char *device_id;        /**< 可空：默认取 MAC 尾号 */
    uint16_t    keepalive_sec;    /**< 默认 60 */
    uint32_t    post_interval_ms; /**< 周期上报间隔，默认 5000 */
    bool        tb_mode;          /**< true=按 ThingsBoard 主题（v1/devices/me/...） */
} cloud_mqtt_config_t;

/** 启动 MQTT 任务（内部等待 WiFi 就绪后连接） */
esp_err_t cloud_mqtt_start(const cloud_mqtt_config_t *cfg);

/** 请求立即上报一次 */
esp_err_t cloud_mqtt_post_now(void);

bool cloud_mqtt_is_connected(void);

/** 状态描述，便于串口排查 */
int cloud_mqtt_get_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
