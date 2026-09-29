/**
 * @file main.c
 * @brief Smart Light 应用入口：初始化硬件 -> 启动各模块任务 -> 联网 -> 接云 -> OTA
 *
 * 启动顺序（任何一步失败只影响自身功能，不阻塞后续）：
 *   1. NVS / 状态中心 / OTA 回滚确认
 *   2. 硬件驱动：RGB、DS18B20、OLED
 *   3. 采集与调光：光敏 ADC、传感器采集任务、灯光控制任务
 *   4. 交互：蜂鸣器、闹钟/定时器、串口控制台
 *   5. 联网：NVS 凭据 -> 编译期默认凭据 -> BLE 配网
 *   6. 校时：SNTP（闹钟/定时器依赖本地时间）
 *   7. 云端：MQTT（本地 Broker / EMQX / ThingsBoard）
 *   8. OTA：双分区远程升级
 *
 * 任务划分（FreeRTOS）：
 *   sensor_task   优先级 4  周期性采集光照/温度
 *   light_ctrl    优先级 5  灯光状态机 + 闹钟响铃（事件驱动）
 *   alarm_task    优先级 4  闹钟与定时器 1 秒节拍检查
 *   oled_task     优先级 3  OLED 三页面刷新
 *   console_cmd   优先级 3  串口命令
 *   cloud_task    优先级 5  MQTT 上报 / 下发 / OTA
 */
#include <stdio.h>
#include <string.h>
#include "alarm_mgr.h"
#include "board_config.h"
#include "buzzer.h"
#include "cloud_mqtt.h"
#include "cloud_ota.h"
#include "console_cmd.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "light_ctrl.h"
#include "light_sensor.h"
#include "net_prov.h"
#include "nvs_flash.h"
#include "oled_ui.h"
#include "rgb_led.h"
#include "sensor_service.h"
#include "sntp_time.h"
#include "sys_state.h"
#include "temp_sensor.h"

static const char *TAG = "smart_light";

/* -------------------------------------------------------------------------- */
/*                                 NVS                                          */
/* -------------------------------------------------------------------------- */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区异常，擦除后重新初始化");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/* -------------------------------------------------------------------------- */
/*                              OTA 回滚确认                                    */
/* -------------------------------------------------------------------------- */
/**
 * 新固件首次启动时把当前分区标记为有效，取消 bootloader 的自动回滚。
 * 配合 cloud_ota 的 "升级失败不切换分区" 策略，保证设备不会变砖。
 */
static void confirm_ota_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "新固件首次启动，标记为有效（取消回滚）");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    ESP_LOGI(TAG, "运行分区: %s (offset 0x%lx)", running->label, (unsigned long)running->address);
}

/* -------------------------------------------------------------------------- */
/*                              启动配置体检                                     */
/* -------------------------------------------------------------------------- */
/**
 * 开机打印一张"配置体检表"，不用猜：哪些配好了、哪些还缺。
 * 缺的项只影响对应功能，不会阻塞其它功能。
 */
static void print_config_diagnostics(void)
{
    printf("\n");
    printf("========== 配置体检 ==========\n");

    if (strlen(CONFIG_WIFI_STA_SSID) > 0) {
        printf("  WiFi 凭据  : 已填 -> %s（开机直接连接）\n", CONFIG_WIFI_STA_SSID);
    } else {
        printf("  WiFi 凭据  : 未填 -> 首次启动自动进入 BLE 配网\n");
        printf("               也可用串口命令 PROV 随时开启配网\n");
    }
    printf("  配网 POP   : %s（手机 App 连接时要填）\n", CONFIG_PROV_POP);

#ifdef CONFIG_CLOUD_MQTT_ENABLE
    printf("  MQTT 云端  : %s（上报周期 %ds）\n",
           CONFIG_CLOUD_MQTT_URI, CONFIG_CLOUD_MQTT_POST_INTERVAL_SEC);
#else
    printf("  MQTT 云端  : 未启用（menuconfig -> MQTT 云端）\n");
#endif

#ifdef CONFIG_OTA_ENABLE
    printf("  OTA 固件源 : %s\n", CONFIG_OTA_FIRMWARE_URL);
#else
    printf("  OTA 固件源 : 未启用\n");
#endif

    printf("  常用命令   : HELP / STATUS / WIFI STATUS / PROV / OTA\n");
    printf("==============================\n\n");
}

/* -------------------------------------------------------------------------- */
/*                                 app_main                                     */
/* -------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "========== Smart Light 启动 ==========");
    ESP_LOGI(TAG, "IDF 版本: %s", esp_get_idf_version());

    print_config_diagnostics();

    /* 1. 基础服务 */
    init_nvs();
    ESP_ERROR_CHECK(sys_state_init());
    confirm_ota_image();

    /* 2. 硬件驱动 */
    ESP_ERROR_CHECK(rgb_led_init());
    if (temp_sensor_init() != ESP_OK) {
        ESP_LOGW(TAG, "温度功能禁用（查接线/4.7k 上拉），插好后敲 TEMP 可热重扫恢复");
    }
    ESP_ERROR_CHECK(oled_ui_init());

    /* 3. 采集与调光 */
    ESP_ERROR_CHECK(light_sensor_init());
    ESP_ERROR_CHECK(sensor_service_start());
    ESP_ERROR_CHECK(light_ctrl_start());
    {
        int raw = 0, pct = 0;
        if (light_sensor_read_raw(&raw) == ESP_OK) {
            light_sensor_read_percent(&pct);
            ESP_LOGI(TAG, "光敏自检: ADC=%d -> %d%%", raw, pct);
        }
    }

    /* 4. 交互：蜂鸣器 / 闹钟 / OLED / 串口控制台 */
    ESP_ERROR_CHECK(buzzer_init());
    ESP_ERROR_CHECK(alarm_mgr_init());
    ESP_ERROR_CHECK(alarm_mgr_start());
    buzzer_beep(80);
    ESP_ERROR_CHECK(oled_ui_start());
    ESP_ERROR_CHECK(console_cmd_start());
    ESP_LOGI(TAG, "本地功能就绪（输入 HELP 查看串口命令）");

    /* 5. 联网：NVS 凭据 -> 编译期默认凭据 -> BLE 配网 */
    ESP_ERROR_CHECK(net_prov_start(CONFIG_WIFI_CONNECT_TIMEOUT_SEC * 1000));
    {
        sys_state_t nst;
        sys_state_get(&nst);
        ESP_LOGI(TAG, "WiFi 连接=%d SSID=%s IP=%s",
                 (int)nst.wifi_connected,
                 nst.ssid[0] ? nst.ssid : "(无)",
                 nst.wifi_connected ? nst.ip_addr : "-");
        ESP_LOGI(TAG, "联网后空闲堆=%lu 字节", (unsigned long)esp_get_free_heap_size());
    }

    /* 6. SNTP 校时 */
    if (net_prov_is_connected()) {
        ESP_ERROR_CHECK(sntp_time_init());
        if (sntp_time_wait_sync(CONFIG_SNTP_SYNC_TIMEOUT_SEC * 1000)) {
            ESP_LOGI(TAG, "SNTP 校时成功");
        } else {
            ESP_LOGW(TAG, "SNTP 校时超时（联网后会自动同步）");
        }
    } else {
        ESP_LOGW(TAG, "WiFi 未连接，跳过校时");
    }

    /* 7. 配网状态提示 */
    if (net_prov_is_connected()) {
        ESP_LOGI(TAG, "已联网，BLE 广播未开启（重新配网：敲 PROV，或 WIFI RESET 后重启）");
    } else {
        ESP_LOGI(TAG, "BLE 配网广播中，手机 App（ESP BLE Prov）搜 PROV_LIGHT_xxxx 即可配网");
    }

    /* 8. 云端接入：通用 MQTT（本地 Broker / EMQX / ThingsBoard） */
#ifdef CONFIG_CLOUD_MQTT_ENABLE
    {
        cloud_mqtt_config_t mqtt_cfg = {
            .uri              = CONFIG_CLOUD_MQTT_URI,
            .username         = CONFIG_CLOUD_MQTT_USERNAME,
            .password         = CONFIG_CLOUD_MQTT_PASSWORD,
            .device_id        = CONFIG_CLOUD_MQTT_DEVICE_ID,
            .keepalive_sec    = 60,
            .post_interval_ms = CONFIG_CLOUD_MQTT_POST_INTERVAL_SEC * 1000,
            /* tb_mode 是 bool 项，未勾选时 sdkconfig.h 不会定义该宏，
               直接引用会编译报 undeclared，必须用 #ifdef 判断 */
#ifdef CONFIG_CLOUD_MQTT_TB_MODE
            .tb_mode          = true,
#else
            .tb_mode          = false,
#endif
        };
        if (cloud_mqtt_start(&mqtt_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "MQTT 云端已启用 (%s)", CONFIG_CLOUD_MQTT_URI);
        } else {
            ESP_LOGW(TAG, "MQTT 云端启动失败 (%s)", CONFIG_CLOUD_MQTT_URI);
        }
    }
#else
    ESP_LOGW(TAG, "MQTT 云端未启用（menuconfig -> Smart Light 配置 -> MQTT 云端 -> 启用）");
#endif

    /* 9. OTA 远程升级 */
    ESP_LOGI(TAG, "OTA 就绪  当前固件 v%s  运行分区 %s",
             cloud_ota_running_version(), cloud_ota_running_partition());
#ifdef CONFIG_OTA_ENABLE
    ESP_LOGI(TAG, "默认固件地址: %s", CONFIG_OTA_FIRMWARE_URL);
#endif
    ESP_LOGI(TAG, "触发方式：串口敲 OTA，或云端下发 {\"cmd\":\"ota\"}");

    ESP_LOGI(TAG, "========== 初始化完成，进入运行态 ==========");
    oled_ui_show_message("Ready", "Online", 3000);

    /* 主任务：每 5 秒打印一次运行摘要 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        sys_state_t st;
        sys_state_get(&st);
        ESP_LOGI(TAG, "温度=%s%.1fC 光照=%d%% 颜色=%d,%d,%d WiFi=%d 云端=%d 空闲堆=%lu 固件=v%s",
                 st.temp_valid ? "" : "无效:", st.temperature,
                 st.light_percent, st.rgb[0], st.rgb[1], st.rgb[2],
                 (int)st.wifi_connected, (int)st.cloud_connected,
                 (unsigned long)esp_get_free_heap_size(),
                 cloud_ota_running_version());
    }
}
