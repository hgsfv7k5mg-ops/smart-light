/**
 * @file cloud_mqtt.c
 * @brief 通用 MQTT 接入实现：周期上报、下发解析、遗嘱与重连
 *
 * 任务启动后先等待 WiFi 就绪，再创建 esp-mqtt 客户端（自带自动重连）。
 * 下发报文支持三类：灯控（mode / rgb）、定时与闹钟指令（cmd=timer / alarm）、
 * 远程 OTA（cmd=ota）；处理完成后立即补发一条遥测作为回执。
 */
#include "cloud_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_https_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "alarm_mgr.h"
#include "cloud_ota.h"
#include "mqtt_client.h"
#include "net_prov.h"
#include "sys_state.h"

static const char *TAG = "cloud_mqtt";

static cloud_mqtt_config_t s_cfg = {0};
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static uint32_t s_post_count = 0;
static uint32_t s_set_count = 0;
static uint32_t s_retry_count = 0;

static char s_topic_telemetry[128];
static char s_topic_state[128];
static char s_topic_set[128];
static char s_topic_status[128];
static char s_dev_id[32];

/* -------------------------------------------------------------------------- */
/*                                  工具                                        */
/* -------------------------------------------------------------------------- */
static bool topic_equals(const char *topic, int topic_len, const char *expected)
{
    size_t expected_len = strlen(expected);
    return ((int)expected_len == topic_len) && (strncmp(topic, expected, expected_len) == 0);
}

static int mode_to_int(light_mode_t mode)
{
    switch (mode) {
    case LIGHT_MODE_OFF:    return 0;
    case LIGHT_MODE_MANUAL: return 2;
    case LIGHT_MODE_AUTO:
    default:                return 1;
    }
}

static light_mode_t mode_from_int(int value)
{
    switch (value) {
    case 0:  return LIGHT_MODE_OFF;
    case 2:  return LIGHT_MODE_MANUAL;
    case 1:
    default: return LIGHT_MODE_AUTO;
    }
}

/*
 * 星期的两种表示要来回换：
 *   设备内部 week_mask  bit0=周日 ... bit6=周六（跟 struct tm::tm_wday 对齐）；
 *   云端报文用 [1..7] 数组，1=周一 ... 7=周日（跟串口命令、界面一致）。
 * 掩码为 0 表示"每天"，序列化成空数组。
 */
/** 位掩码 -> JSON 数组（1=周一 ... 7=周日），掩码为 0 时得到空数组 */
static cJSON *week_mask_to_json(uint8_t mask)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (int day = 1; day <= 7; day++) {
        if (mask & (1 << ((day == 7) ? 0 : day))) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(day));
        }
    }
    return arr;
}

static uint8_t week_json_to_mask(const cJSON *arr)
{
    uint8_t mask = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(item)) {
            continue;
        }
        int day = (int)item->valuedouble;
        if (day >= 1 && day <= 7) {
            mask |= (uint8_t)(1 << ((day == 7) ? 0 : day));
        }
    }
    return mask;
}

/** 取整数字段，缺省或类型不符时返回 fallback */
static int json_int(const cJSON *obj, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

/* -------------------------------------------------------------------------- */
/*                                 上报                                         */
/* -------------------------------------------------------------------------- */
static esp_err_t post_telemetry(void)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    sys_state_t st;
    sys_state_get(&st);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 温度无效时不上报 temp：报 0 会在大屏画出 0℃ 假曲线，缺字段反而是诚实的"无数据" */
    if (s_cfg.tb_mode) {
        /* ThingsBoard 扁平结构：{"temperature":28.0,"light":45,"r":..,"g":..,"b":..} */
        if (st.temp_valid) {
            cJSON_AddNumberToObject(root, "temperature", roundf(st.temperature * 10.0f) / 10.0f);
        }
        cJSON_AddNumberToObject(root, "light", st.light_percent);
        cJSON_AddNumberToObject(root, "mode", mode_to_int(st.mode));
        cJSON_AddNumberToObject(root, "r", st.rgb[0]);
        cJSON_AddNumberToObject(root, "g", st.rgb[1]);
        cJSON_AddNumberToObject(root, "b", st.rgb[2]);
    } else {
        /* 通用嵌套结构，便于人读与规则引擎 */
        if (st.temp_valid) {
            cJSON_AddNumberToObject(root, "temp", roundf(st.temperature * 10.0f) / 10.0f);
        }
        cJSON_AddNumberToObject(root, "light", st.light_percent);
        cJSON_AddNumberToObject(root, "mode", mode_to_int(st.mode));
        cJSON_AddNumberToObject(root, "r", st.rgb[0]);
        cJSON_AddNumberToObject(root, "g", st.rgb[1]);
        cJSON_AddNumberToObject(root, "b", st.rgb[2]);
        cJSON_AddNumberToObject(root, "ts", (double)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        /*
         * 定时器与闹钟跟着遥测一起下发：大屏直接渲染，不用再发一次查询请求；
         * 设备重启、大屏晚开都能自愈，不会出现两边各说各话。
         * （ThingsBoard 分支保持扁平结构，不带这两个对象。）
         */
        cJSON *timer = cJSON_AddObjectToObject(root, "timer");
        if (timer != NULL) {
            char on[8], off[8];
            snprintf(on, sizeof(on), "%02u:%02u", st.timer.hour_on, st.timer.min_on);
            snprintf(off, sizeof(off), "%02u:%02u", st.timer.hour_off, st.timer.min_off);
            cJSON_AddNumberToObject(timer, "enable", st.timer.enabled ? 1 : 0);
            cJSON_AddStringToObject(timer, "on", on);
            cJSON_AddStringToObject(timer, "off", off);
            cJSON_AddItemToObject(timer, "week", week_mask_to_json(st.timer.week_mask));
        }

        cJSON *alarms = cJSON_AddArrayToObject(root, "alarms");
        int alarm_count = (int)(sizeof(st.alarms) / sizeof(st.alarms[0]));
        for (int i = 0; i < alarm_count && alarms != NULL; i++) {
            if (!st.alarms[i].enabled) {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) {
                break;
            }
            cJSON_AddNumberToObject(item, "slot", i + 1);
            cJSON_AddNumberToObject(item, "hour", st.alarms[i].hour);
            cJSON_AddNumberToObject(item, "minute", st.alarms[i].minute);
            cJSON_AddStringToObject(item, "repeat",
                                    (st.alarms[i].repeat == ALARM_REPEAT_ONCE) ? "once" : "daily");
            cJSON_AddItemToObject(item, "week", week_mask_to_json(st.alarms[i].week_mask));
            cJSON_AddItemToArray(alarms, item);
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int ret = esp_mqtt_client_publish(s_client, s_topic_telemetry, payload, 0, 0, 0);
    if (ret >= 0) {
        s_post_count++;
        ESP_LOGI(TAG, "上报 #%lu: %s", (unsigned long)s_post_count, payload);
    } else {
        ESP_LOGW(TAG, "上报失败: %d", ret);
        free(payload);
        return ESP_FAIL;
    }
    free(payload);
    return ESP_OK;
}

static void publish_state(void)
{
    if (s_client == NULL || !s_connected) {
        return;
    }
    sys_state_t st;
    sys_state_get(&st);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddNumberToObject(root, "mode", mode_to_int(st.mode));
    cJSON_AddNumberToObject(root, "r", st.rgb[0]);
    cJSON_AddNumberToObject(root, "g", st.rgb[1]);
    cJSON_AddNumberToObject(root, "b", st.rgb[2]);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != NULL) {
        esp_mqtt_client_publish(s_client, s_topic_state, payload, 0, 0, 1);
        free(payload);
    }
}

/* -------------------------------------------------------------------------- */
/*                              下发处理                                        */
/* -------------------------------------------------------------------------- */
/**
 * 闹钟指令：
 *   {"cmd":"alarm","action":"add","hour":7,"minute":30,"repeat":"daily","week":[1,3,5]}
 *   {"cmd":"alarm","action":"del","slot":2}      /  {"action":"clear"}  /  {"action":"test"}
 * week 省略或为空数组 = 每天；repeat 省略按 daily 处理（一周内每天）。
 * 增删由 alarm_mgr 统一维护，改完立即回报，大屏看到的就是设备端真实列表。
 */
static void handle_alarm_cmd(const cJSON *root)
{
    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const char *action = (cJSON_IsString(action_item) && action_item->valuestring != NULL)
                             ? action_item->valuestring : "add";

    if (strcasecmp(action, "del") == 0) {
        int slot = json_int(root, "slot", -1);
        if (alarm_mgr_del(slot) != ESP_OK) {
            ESP_LOGW(TAG, "删除闹钟 %d 失败（不存在）", slot);
        }
    } else if (strcasecmp(action, "clear") == 0 || strcasecmp(action, "delall") == 0) {
        alarm_mgr_del_all();
    } else if (strcasecmp(action, "test") == 0) {
        alarm_mgr_trigger_now();    /* 只验证响铃效果，不依赖校时 */
    } else if (strcasecmp(action, "add") == 0) {
        int hour = json_int(root, "hour", -1);
        int minute = json_int(root, "minute", -1);
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            ESP_LOGW(TAG, "闹钟时间非法: %d:%d", hour, minute);
            return;
        }
        cJSON *rep = cJSON_GetObjectItem(root, "repeat");
        alarm_repeat_t repeat = (cJSON_IsString(rep) && rep->valuestring != NULL &&
                                 strcasecmp(rep->valuestring, "once") == 0)
                                    ? ALARM_REPEAT_ONCE : ALARM_REPEAT_DAILY;
        uint8_t mask = week_json_to_mask(cJSON_GetObjectItem(root, "week"));

        int slot = alarm_mgr_add((uint8_t)hour, (uint8_t)minute, repeat, mask);
        if (slot < 0) {
            ESP_LOGW(TAG, "闹钟添加失败: 槽位已满，先删掉一个再试");
        } else {
            ESP_LOGI(TAG, "云端添加闹钟 %d: %02d:%02d", slot, hour, minute);
        }
    } else {
        ESP_LOGW(TAG, "未知闹钟指令: %s", action);
    }
}

/**
 * 定时开关灯指令：
 *   {"cmd":"timer","action":"set","hour_on":22,"minute_on":0,"hour_off":6,"minute_off":0,"week":[1,2,3,4,5]}
 *   {"cmd":"timer","action":"week","week":[6,7]}    仅改星期
 *   {"cmd":"timer","action":"off"}                  关闭定时
 * 支持跨天：开灯时间晚于关灯时间即视为 22:00-次日 06:00。
 */
static void handle_timer_cmd(const cJSON *root)
{
    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    const char *action = (cJSON_IsString(action_item) && action_item->valuestring != NULL)
                             ? action_item->valuestring : "set";

    if (strcasecmp(action, "off") == 0) {
        alarm_mgr_timer_disable();
        return;
    }
    if (strcasecmp(action, "week") == 0) {
        alarm_mgr_timer_set_week(week_json_to_mask(cJSON_GetObjectItem(root, "week")));
        return;
    }

    int h_on  = json_int(root, "hour_on", -1);
    int m_on  = json_int(root, "minute_on", -1);
    int h_off = json_int(root, "hour_off", -1);
    int m_off = json_int(root, "minute_off", -1);
    if (h_on < 0 || h_on > 23 || m_on < 0 || m_on > 59 ||
        h_off < 0 || h_off > 23 || m_off < 0 || m_off > 59) {
        ESP_LOGW(TAG, "定时器时间非法: %d:%d-%d:%d", h_on, m_on, h_off, m_off);
        return;
    }
    if (alarm_mgr_timer_set((uint8_t)h_on, (uint8_t)m_on, (uint8_t)h_off, (uint8_t)m_off) != ESP_OK) {
        return;
    }
    /* week 可选：带上了就顺带一起设，没带则保留设备原有设置 */
    cJSON *week = cJSON_GetObjectItem(root, "week");
    if (cJSON_IsArray(week)) {
        alarm_mgr_timer_set_week(week_json_to_mask(week));
    }
}

static void handle_set(const char *payload, int payload_len)
{
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "控制 JSON 解析失败: %.*s", payload_len, payload);
        return;
    }

    s_set_count++;
    ESP_LOGI(TAG, "收到控制 #%lu: %.*s", (unsigned long)s_set_count, payload_len, payload);

    /* 远程 OTA 指令：{"cmd":"ota","url":"http://192.168.1.100:8083/ota/smart_light.bin"}
       url 可省略，省略时用 menuconfig 里的默认固件地址。
       这条指令不当作灯控处理，直接返回。 */
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd_item) && cmd_item->valuestring != NULL &&
        strcasecmp(cmd_item->valuestring, "ota") == 0) {
        char url[512] = {0};
        cJSON *u = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(u) && u->valuestring != NULL) {
            strncpy(url, u->valuestring, sizeof(url) - 1);
        }
#ifdef CONFIG_OTA_ENABLE
        else {
            strncpy(url, CONFIG_OTA_FIRMWARE_URL, sizeof(url) - 1);
        }
#endif
        if (url[0] != '\0') {
            ESP_LOGI(TAG, "收到远程 OTA 指令: %s", url);
            cloud_ota_config_t ota_cfg = {
                .url        = url,
                .reboot     = true,
                .timeout_ms = 30000,
            };
            esp_err_t oerr = cloud_ota_start(&ota_cfg);
            if (oerr != ESP_OK) {
                ESP_LOGW(TAG, "  OTA 启动失败: %s", esp_err_to_name(oerr));
            }
        } else {
            ESP_LOGW(TAG, "OTA 指令缺少 url，且未配置默认固件地址");
        }
        cJSON_Delete(root);
        return;
    }

    /* 定时器 / 闹钟指令：同样不当作灯控处理，改完立刻回报最新配置 */
    if (cJSON_IsString(cmd_item) && cmd_item->valuestring != NULL) {
        bool is_alarm = (strcasecmp(cmd_item->valuestring, "alarm") == 0);
        bool is_timer = (strcasecmp(cmd_item->valuestring, "timer") == 0);
        if (is_alarm || is_timer) {
            if (is_alarm) {
                handle_alarm_cmd(root);
            } else {
                handle_timer_cmd(root);
            }
            cJSON_Delete(root);
            publish_state();
            post_telemetry();
            return;
        }
    }

    /* 纯文本 ON / OFF（方便 mqttx 手测） */
    if (cJSON_IsString(root) && root->valuestring != NULL) {
        if (strcasecmp(root->valuestring, "ON") == 0) {
            sys_state_set_mode(LIGHT_MODE_AUTO);
        } else if (strcasecmp(root->valuestring, "OFF") == 0) {
            sys_state_set_mode(LIGHT_MODE_OFF);
        }
        cJSON_Delete(root);
        publish_state();
        post_telemetry();
        return;
    }

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsNumber(mode_item)) {
        light_mode_t m = mode_from_int((int)mode_item->valuedouble);
        ESP_LOGI(TAG, "  设置模式 = %d", mode_to_int(m));
        sys_state_set_mode(m);
    }

    cJSON *r = cJSON_GetObjectItem(root, "r");
    cJSON *g = cJSON_GetObjectItem(root, "g");
    cJSON *b = cJSON_GetObjectItem(root, "b");
    if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
        uint8_t rr = (uint8_t)r->valuedouble;
        uint8_t gg = (uint8_t)g->valuedouble;
        uint8_t bb = (uint8_t)b->valuedouble;
        ESP_LOGI(TAG, "  设置颜色 = %d,%d,%d", rr, gg, bb);
        sys_state_set_manual_color(rr, gg, bb);
        if (!cJSON_IsNumber(mode_item)) {
            sys_state_set_mode(LIGHT_MODE_MANUAL);
        }
    }

    /* 兼容 ThingsBoard RPC：{"method":"setColor","params":{"r":..}} */
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsString(method) && params != NULL) {
        cJSON *pr = cJSON_GetObjectItem(params, "r");
        cJSON *pg = cJSON_GetObjectItem(params, "g");
        cJSON *pb = cJSON_GetObjectItem(params, "b");
        if (cJSON_IsNumber(pr) && cJSON_IsNumber(pg) && cJSON_IsNumber(pb)) {
            sys_state_set_manual_color((uint8_t)pr->valuedouble,
                                       (uint8_t)pg->valuedouble,
                                       (uint8_t)pb->valuedouble);
            sys_state_set_mode(LIGHT_MODE_MANUAL);
            ESP_LOGI(TAG, "  TB RPC setColor = %d,%d,%d",
                     (int)pr->valuedouble, (int)pg->valuedouble, (int)pb->valuedouble);
        }
    }

    cJSON_Delete(root);
    publish_state();
    post_telemetry();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接: %s", s_cfg.uri);
        s_connected = true;
        sys_state_set_cloud(true);
        esp_mqtt_client_subscribe(s_client, s_topic_set, 1);
        ESP_LOGI(TAG, "已订阅: %s", s_topic_set);
        /* 上线通知（LWT 会让 Broker 在掉线时发 offline） */
        esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 0, 1);
        publish_state();
        post_telemetry();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_retry_count++;
        ESP_LOGW(TAG, "MQTT 断开（第 %lu 次），esp-mqtt 会自动重连",
                 (unsigned long)s_retry_count);
        s_connected = false;
        sys_state_set_cloud(false);
        break;

    case MQTT_EVENT_DATA:
        if (event->topic == NULL) {
            break;
        }
        if (topic_equals(event->topic, event->topic_len, s_topic_set)) {
            handle_set(event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 错误 (类型=%d)", event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "连接被拒绝 —— 检查 Broker 地址/端口/用户名密码");
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "TCP 传输错误 —— 检查 WiFi 能否访问 Broker、端口是否放行");
        }
        if (event->error_handle->connect_return_code != 0) {
            ESP_LOGE(TAG, "CONNACK 返回码 %d (4=用户名密码错 5=未授权)",
                     event->error_handle->connect_return_code);
        }
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*                                  任务                                        */
/* -------------------------------------------------------------------------- */
static void cloud_task(void *arg)
{
    uint32_t interval_ms = (s_cfg.post_interval_ms > 0) ? s_cfg.post_interval_ms : 5000;
    EventGroupHandle_t events = sys_state_events();

    while (1) {
        if (!net_prov_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_client == NULL) {
            esp_mqtt_client_config_t mqtt_cfg = {
                .broker.address.uri = s_cfg.uri,
                .credentials.username = s_cfg.username,
                .credentials.authentication.password = s_cfg.password,
                .credentials.client_id = s_dev_id,
                .session.keepalive = (s_cfg.keepalive_sec > 0) ? s_cfg.keepalive_sec : 60,
                .session.disable_clean_session = false,
                .network.timeout_ms = 10000,
                .network.reconnect_timeout_ms = 10000,
                .task.stack_size = 6144,
                .task.priority = 5,
                /* LWT 遗嘱：掉线时 Broker 自动发 offline */
                .session.last_will.topic = s_topic_status,
                .session.last_will.msg = "offline",
                .session.last_will.msg_len = 7,
                .session.last_will.qos = 1,
                .session.last_will.retain = 1,
            };

            s_client = esp_mqtt_client_init(&mqtt_cfg);
            if (s_client == NULL) {
                ESP_LOGE(TAG, "MQTT 客户端创建失败，10 秒后重试");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(s_client);
            ESP_LOGI(TAG, "连接 Broker: %s (clientId=%s)", s_cfg.uri, s_dev_id);
        }

        xEventGroupWaitBits(events, SYS_EV_POST_NOW, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(interval_ms));
        if (s_connected) {
            post_telemetry();
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                  生命周期                                    */
/* -------------------------------------------------------------------------- */
esp_err_t cloud_mqtt_start(const cloud_mqtt_config_t *cfg)
{
    if (cfg == NULL || cfg->uri == NULL || strlen(cfg->uri) == 0) {
        ESP_LOGW(TAG, "Broker 地址未配置，云端功能禁用");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_cfg, cfg, sizeof(cloud_mqtt_config_t));

    /* 设备 ID：优先用配置值，否则取 MAC 尾号 */
    if (cfg->device_id != NULL && strlen(cfg->device_id) > 0) {
        strncpy(s_dev_id, cfg->device_id, sizeof(s_dev_id) - 1);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_dev_id, sizeof(s_dev_id), "light_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    if (cfg->tb_mode) {
        /* ThingsBoard 标准主题 */
        snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "v1/devices/me/telemetry");
        snprintf(s_topic_state, sizeof(s_topic_state), "v1/devices/me/attributes");
        snprintf(s_topic_set, sizeof(s_topic_set), "v1/devices/me/rpc/request/+");
        snprintf(s_topic_status, sizeof(s_topic_status), "v1/devices/me/attributes");
    } else {
        snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "light/%s/telemetry", s_dev_id);
        snprintf(s_topic_state, sizeof(s_topic_state), "light/%s/state", s_dev_id);
        snprintf(s_topic_set, sizeof(s_topic_set), "light/%s/set", s_dev_id);
        snprintf(s_topic_status, sizeof(s_topic_status), "light/%s/status", s_dev_id);
    }

    ESP_LOGI(TAG, "Broker   : %s", s_cfg.uri);
    ESP_LOGI(TAG, "设备 ID  : %s", s_dev_id);
    ESP_LOGI(TAG, "上报主题 : %s", s_topic_telemetry);
    ESP_LOGI(TAG, "控制主题 : %s", s_topic_set);

    if (xTaskCreatePinnedToCore(cloud_task, "cloud_mqtt", 5120, NULL, 5,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "云端任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MQTT 任务已启动 (上报周期 %lums)",
             (unsigned long)((cfg->post_interval_ms > 0) ? cfg->post_interval_ms : 5000));
    return ESP_OK;
}

esp_err_t cloud_mqtt_post_now(void)
{
    sys_state_notify(SYS_EV_POST_NOW);
    return ESP_OK;
}

bool cloud_mqtt_is_connected(void)
{
    return s_connected;
}

int cloud_mqtt_get_status(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    if (s_cfg.uri == NULL || strlen(s_cfg.uri) == 0) {
        return snprintf(buf, len, "未配置（Broker 地址为空，云端已禁用）");
    }
    return snprintf(buf, len,
                    "%s | 已上报 %lu 条 | 收到控制 %lu 条 | 断连 %lu 次\n"
                    "     Broker: %s\n"
                    "     设备  : %s",
                    s_connected ? "已连接" : "未连接",
                    (unsigned long)s_post_count,
                    (unsigned long)s_set_count,
                    (unsigned long)s_retry_count,
                    s_cfg.uri, s_dev_id);
}
