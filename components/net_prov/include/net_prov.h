/**
 * @file net_prov.h
 * @brief 网络连接管理：WiFi STA 自动连接 + 断线重连 + BLE 配网
 *
 * 启动策略（按优先级）：
 *   1. NVS 中已有配网凭据（手机配网时存下的） -> 直接连接；
 *   2. 没有凭据但 menuconfig 里填了默认 SSID -> 用默认凭据连接；
 *   3. 两者都没有 -> 直接开启 BLE 配网广播，等待手机 App 下发凭据；
 *   4. 有凭据但连不上 -> 超时后自动开启 BLE 配网广播；
 *   5. 配网成功后自动重启并用新凭据连接。
 *
 * 配网走的是 wifi_prov_scheme_ble（protocomm BLE / GATT server），
 * 配套 App 是 ESP BLE Prov（ESP-IDF Provisioning），不是 BluFi、不要用 EspBlufi。
 *
 * 串口命令 PROV 可随时手动开启配网，WIFI RESET 清除已保存凭据。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 + 连接。wait_ms=0 表示不阻塞等待 */
esp_err_t net_prov_start(uint32_t wait_ms);

/** 阻塞等待拿到 IP */
bool net_prov_wait_connected(uint32_t timeout_ms);

bool net_prov_is_connected(void);

/** 手动开启 BLE 配网（会把 WiFi 切到配网模式） */
esp_err_t net_prov_start_ble(void);

/** 清除 NVS 中的配网凭据（下次启动进入配网模式） */
esp_err_t net_prov_reset(void);

/** 获取当前 SSID / IP 字符串 */
esp_err_t net_prov_get_ssid(char *buf, size_t len);
esp_err_t net_prov_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
