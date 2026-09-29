/**
 * @file net_prov.c
 * @brief 网络连接与 BLE 配网实现
 *
 * 启动时的凭据优先级：NVS 中已保存的配网凭据 > 编译期默认凭据 > 进入 BLE 配网。
 * 用户通过手机配网保存的凭据应当覆盖固件内置默认值。
 *
 * 两个与蓝牙相关的约束（详见 README「已知问题」）：
 *   1) wifi_prov_mgr_init() 全局只调用一次，不做 init/deinit 循环；
 *   2) 配网结束后不调用 deinit，直接重启。
 */
#include "net_prov.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "net_prov.h"
#include "nvs_flash.h"
#include "sys_state.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "net_prov";

#define NET_CONNECTED_BIT   BIT0

#define WIFI_MAX_RETRY      10          /* 连续断开超过该次数不再自动重连 */

static EventGroupHandle_t s_net_events = NULL;
static bool s_connected = false;
static int s_retry_count = 0;
static bool s_prov_running = false;
static bool s_prov_inited = false;    /* wifi_prov_mgr 是否已 init（保证只 init 一次） */
static bool s_wifi_started = false;
static char s_ssid[33] = {0};
static char s_ip[16] = "0.0.0.0";

/* -------------------------------------------------------------------------- */
/*                                WiFi 事件                                     */
/* -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA 启动，开始连接...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        sys_state_set_wifi(false, "0.0.0.0", NULL);
        if (s_prov_running) {
            return;
        }
        if (++s_retry_count <= WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "WiFi 断开，第 %d 次重连", s_retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "重连 %d 次仍失败，停止自动重连（可执行 PROV 重新配网）", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        strncpy(s_ip, ip, sizeof(s_ip) - 1);

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            strncpy(s_ssid, (char *)ap_info.ssid, sizeof(s_ssid) - 1);
            s_ssid[sizeof(s_ssid) - 1] = '\0';
        }

        s_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "已连接 WiFi: %s, IP: %s", s_ssid, s_ip);
        sys_state_set_wifi(true, s_ip, s_ssid);
        if (s_net_events) {
            xEventGroupSetBits(s_net_events, NET_CONNECTED_BIT);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                 BLE 配网                                     */
/* -------------------------------------------------------------------------- */
static void prov_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base != WIFI_PROV_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "配网已启动，等待手机 App 连接");
        break;

    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "收到 WiFi 凭据: SSID=%s", (const char *)cfg->ssid);
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "凭据校验失败: %s，将重新开始配网",
                 (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "密码错误" : "AP 不可达");
        break;
    }

    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "配网成功，已连上 AP");
        break;

    case WIFI_PROV_END: {
        ESP_LOGI(TAG, "配网流程结束，正在重启以应用新配置");
        s_prov_running = false;
        /* 不要在这里调 wifi_prov_mgr_deinit()：它会再走一次 BT controller deinit，
           可能触发内存异常。反正马上 esp_restart()，让重启清理一切更安全。 */
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        break;
    }

    default:
        break;
    }
}

static void build_service_name(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "PROV_LIGHT_%02X%02X", mac[4], mac[5]);
}

static esp_err_t start_ble_provisioning(void)
{
    if (s_prov_running) {
        return ESP_OK;
    }

    /* 配网前先停掉 STA，避免状态冲突 */
    if (s_wifi_started) {
        ESP_LOGI(TAG, "配网：停 WiFi STA，切到蓝牙（此时空闲堆=%lu）",
                 (unsigned long)esp_get_free_heap_size());
        esp_wifi_stop();
        s_wifi_started = false;
    }

    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    /* 关键：wifi_prov_mgr_init 全局只调用一次。重复 init/deinit 会让
       BT controller 反复 init->deinit->init，在 IDF 5.2 下触发 btdm 的
       free() target pointer is outside heap areas 断言并复位。 */
    if (!s_prov_inited) {
        ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                   &prov_event_handler, NULL));
        s_prov_inited = true;
    }

    char service_name[24];
    build_service_name(service_name, sizeof(service_name));

    /* 设备名会以 PROV_ 前缀广播，手机 App 扫描即可发现 */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = CONFIG_PROV_POP;

    ESP_LOGI(TAG, "========== BLE 配网已开启 ==========");
    ESP_LOGI(TAG, "  设备名    : %s", service_name);
    ESP_LOGI(TAG, "  校验码POP : %s", pop);
    ESP_LOGI(TAG, "  手机 App  : ESP BLE Prov（ESP-IDF Provisioning）");
    ESP_LOGI(TAG, "  操作      : 打开 App → 扫描（设备名以 PROV_ 开头）→");
    ESP_LOGI(TAG, "              输入 POP → 选择目标 WiFi 并填密码 → 下发");
    ESP_LOGI(TAG, "  下发成功后设备会自动重启并用新凭据联网");
    ESP_LOGI(TAG, "====================================");

    esp_err_t err = wifi_prov_mgr_start_provisioning(security, (const void *)pop,
                                                     service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动配网失败: %s", esp_err_to_name(err));
        return err;
    }
    s_prov_running = true;
    ESP_LOGI(TAG, "配网已启动：蓝牙栈就绪，空闲堆=%lu（比停 WiFi 前少的就是蓝牙占用）",
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                                 公共 API                                     */
/* -------------------------------------------------------------------------- */
static esp_err_t sta_connect(const char *ssid, const char *password)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (ssid != NULL && ssid[0] != '\0') {
        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        if (password != NULL) {
            strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
        }
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "使用指定凭据连接: %s", ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    } else {
        /* 【重要】这里绝对不能拿一个空配置去 esp_wifi_set_config()。
           那会把 NVS 里手机刚配网存下的 SSID/密码整个覆盖成空 ——
           表现就是"配网成功了，但一重启又要重新配"。
           什么都不设，esp_wifi_start() 会自动从 NVS 加载凭据。 */
        ESP_LOGI(TAG, "使用 NVS 中已保存的凭据连接");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    return ESP_OK;
}

esp_err_t net_prov_start(uint32_t wait_ms)
{
    s_net_events = xEventGroupCreate();
    if (s_net_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    /* 不要在此处用 wifi_prov_mgr_init()+deinit() 探测配网状态：BT controller 会走
       init→deinit→init，在 IDF 5.2 下命中 btdm 的 free(): target pointer is
       outside heap areas 断言，表现为起蓝牙配网瞬间重启。仅在真正需要配网时初始化一次。 */

    bool cfg_has_creds = (strlen(CONFIG_WIFI_STA_SSID) > 0);

    /* 是否已有配网凭据？esp_wifi_get_config() 只读 WiFi 驱动配置、不碰 wifi_prov_mgr，
       因此不会触发上述 BT controller 缺陷。 */
    wifi_config_t stored = {0};
    bool nvs_has_creds = (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK)
                         && (stored.sta.ssid[0] != '\0');

    /* 优先级：NVS 里手机配网存下的凭据 > 编译期默认凭据 > 蓝牙配网
       —— 用户自己配的 WiFi 应该覆盖固件里的出厂默认值。 */
    if (nvs_has_creds) {
        ESP_LOGI(TAG, "检测到已保存的配网凭据: %s（直接连接，无需重新配网）",
                 (const char *)stored.sta.ssid);
        sta_connect(NULL, NULL);
    } else if (cfg_has_creds) {
        ESP_LOGI(TAG, "使用固件内置凭据: %s", CONFIG_WIFI_STA_SSID);
        sta_connect(CONFIG_WIFI_STA_SSID, CONFIG_WIFI_STA_PASSWORD);
    } else {
        ESP_LOGW(TAG, "尚未配网，进入蓝牙配网模式（等待手机 App 下发 WiFi 凭据）");
        start_ble_provisioning();
        return ESP_OK;
    }

    if (wait_ms > 0) {
        net_prov_wait_connected(wait_ms);
    }

#if CONFIG_PROV_AUTO_START_ON_FAIL
    if (!s_connected) {
        ESP_LOGW(TAG, "连接失败，自动开启蓝牙配网");
        start_ble_provisioning();
    }
#endif
    return ESP_OK;
}

bool net_prov_wait_connected(uint32_t timeout_ms)
{
    if (s_net_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_net_events, NET_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return ((bits & NET_CONNECTED_BIT) != 0);
}

bool net_prov_is_connected(void)
{
    return s_connected;
}

esp_err_t net_prov_start_ble(void)
{
    return start_ble_provisioning();
}

esp_err_t net_prov_reset(void)
{
    ESP_LOGW(TAG, "清除配网信息，稍后重启进入配网模式");
    wifi_prov_mgr_reset_provisioning();
    return ESP_OK;
}

esp_err_t net_prov_get_ssid(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(buf, s_ssid, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t net_prov_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(buf, s_ip, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}
