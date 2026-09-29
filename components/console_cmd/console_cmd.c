/**
 * @file console_cmd.c
 * @brief 串口命令控制台实现
 *
 * 命令以表驱动方式注册（名称 / 用法 / 说明 / 处理函数），
 * 新增命令只需在命令表里加一项，不需要改动解析逻辑。
 * 输入不区分大小写，参数部分原样传给对应处理函数。
 */
#include "console_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "alarm_mgr.h"
#include "board_config.h"
#include "buzzer.h"
#include "cloud_mqtt.h"
#include "cloud_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_prov.h"
#include "oled_ui.h"
#include "sys_state.h"
#include "temp_sensor.h"

static const char *TAG = "console_cmd";

#define CMD_BUF_SIZE 96

typedef esp_err_t (*cmd_handler_t)(const char *args);

typedef struct {
    const char     *name;
    const char     *usage;
    const char     *desc;
    cmd_handler_t   handler;
} cmd_entry_t;

/* ============================== 命令实现 ================================== */

static esp_err_t cmd_auto(const char *args)
{
    (void)args;
    sys_state_set_mode(LIGHT_MODE_AUTO);
    printf("[OK] 自动模式（温度定色 + 光照定亮度）\n");
    return ESP_OK;
}

static esp_err_t cmd_manual(const char *args)
{
    (void)args;
    sys_state_set_mode(LIGHT_MODE_MANUAL);
    printf("[OK] 手动模式\n");
    return ESP_OK;
}

static esp_err_t cmd_off(const char *args)
{
    (void)args;
    sys_state_set_mode(LIGHT_MODE_OFF);
    printf("[OK] 灯光关闭\n");
    return ESP_OK;
}

static esp_err_t cmd_color(const char *args)
{
    int r, g, b;
    if (sscanf(args, "%d,%d,%d", &r, &g, &b) != 3) {
        printf("[ERR] 用法: COLOR 255,100,50\n");
        return ESP_ERR_INVALID_ARG;
    }
    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);
    sys_state_set_manual_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
    sys_state_set_mode(LIGHT_MODE_MANUAL);
    printf("[OK] 颜色: %d,%d,%d\n", r, g, b);
    return ESP_OK;
}

static esp_err_t cmd_page(const char *args)
{
    int p = 0;
    if (sscanf(args, "%d", &p) != 1 || p < 1 || p > 3) {
        printf("[ERR] 用法: PAGE 1|2|3\n");
        return ESP_ERR_INVALID_ARG;
    }
    oled_ui_set_page(p);
    printf("[OK] 页面 %d\n", p);
    return ESP_OK;
}

static esp_err_t cmd_buzz(const char *args)
{
    if (strncasecmp(args, "ON", 2) == 0) {
        buzzer_set(true);
        printf("[OK] 蜂鸣器响\n");
    } else if (strncasecmp(args, "OFF", 3) == 0) {
        buzzer_set(false);
        printf("[OK] 蜂鸣器停\n");
    } else {
        printf("[ERR] 用法: BUZZ ON|OFF\n");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------- 闹钟 ------------------------------------- */
static esp_err_t cmd_alarm(const char *args)
{
    if (strncasecmp(args, "ADD", 3) == 0) {
        int h, m;
        char repeat_str[12] = {0};
        char week_str[24] = {0};
        int parsed = sscanf(args + 3, "%d:%d %11s %23s", &h, &m, repeat_str, week_str);
        if (parsed < 3 || h < 0 || h > 23 || m < 0 || m > 59) {
            printf("[ERR] 用法: ALARM ADD 07:30 daily|once [1,3,5]\n");
            return ESP_ERR_INVALID_ARG;
        }
        alarm_repeat_t repeat;
        if (strcasecmp(repeat_str, "daily") == 0) {
            repeat = ALARM_REPEAT_DAILY;
        } else if (strcasecmp(repeat_str, "once") == 0) {
            repeat = ALARM_REPEAT_ONCE;
        } else {
            printf("[ERR] 重复类型只能是 daily / once\n");
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t mask = (parsed >= 4) ? alarm_mgr_parse_weekdays(week_str) : 0;
        int slot = alarm_mgr_add((uint8_t)h, (uint8_t)m, repeat, mask);
        if (slot < 0) {
            printf("[ERR] 闹钟已满（最多 3 个）\n");
            return ESP_ERR_NO_MEM;
        }
        printf("[OK] 闹钟 %d 已添加: %02d:%02d\n", slot, h, m);

    } else if (strncasecmp(args, "DEL", 3) == 0) {
        char arg[12] = {0};
        if (sscanf(args + 3, "%11s", arg) != 1) {
            printf("[ERR] 用法: ALARM DEL 1|2|3|ALL\n");
            return ESP_ERR_INVALID_ARG;
        }
        if (strcasecmp(arg, "ALL") == 0) {
            alarm_mgr_del_all();
            printf("[OK] 已删除所有闹钟\n");
        } else {
            int slot = atoi(arg);
            if (alarm_mgr_del(slot) != ESP_OK) {
                printf("[ERR] 闹钟 %d 不存在\n", slot);
                return ESP_ERR_NOT_FOUND;
            }
            printf("[OK] 闹钟 %d 已删除\n", slot);
        }

    } else if (strncasecmp(args, "LIST", 4) == 0 || args[0] == '\0') {
        alarm_mgr_print();
    } else if (strncasecmp(args, "TEST", 4) == 0) {
        alarm_mgr_trigger_now();
        printf("[OK] 已触发一次响铃\n");
    } else {
        printf("[ERR] ALARM ADD|DEL|LIST|TEST\n");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------- 定时器 ----------------------------------- */
static esp_err_t cmd_timer(const char *args)
{
    if (strncasecmp(args, "OFF", 3) == 0) {
        alarm_mgr_timer_disable();
        printf("[OK] 定时器已关闭\n");
    } else if (strncasecmp(args, "ON", 2) == 0) {
        int h1, m1, h2, m2;
        if (sscanf(args + 2, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4 ||
            h1 > 23 || m1 > 59 || h2 > 23 || m2 > 59) {
            printf("[ERR] 用法: TIMER ON 22:00-6:00\n");
            return ESP_ERR_INVALID_ARG;
        }
        alarm_mgr_timer_set((uint8_t)h1, (uint8_t)m1, (uint8_t)h2, (uint8_t)m2);
        printf("[OK] 定时器: %02d:%02d - %02d:%02d%s\n", h1, m1, h2, m2,
               ((h1 * 60 + m1) > (h2 * 60 + m2)) ? " (跨天)" : "");
    } else if (strncasecmp(args, "REPEAT", 6) == 0) {
        char week_str[24] = {0};
        if (sscanf(args + 6, "%23s", week_str) != 1) {
            printf("[ERR] 用法: TIMER REPEAT 1,3,5（0 或 清空 = 每天）\n");
            return ESP_ERR_INVALID_ARG;
        }
        alarm_mgr_timer_set_week(alarm_mgr_parse_weekdays(week_str));
        printf("[OK] 定时器星期已设置\n");
    } else if (strncasecmp(args, "SHOW", 4) == 0 || args[0] == '\0') {
        alarm_mgr_timer_print();
    } else {
        printf("[ERR] TIMER ON|OFF|REPEAT|SHOW\n");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------- 温度诊断 --------------------------------- */
static esp_err_t cmd_temp(const char *args)
{
    (void)args;
    char diag[192];

    printf("\n========== DS18B20 诊断 ==========\n");

    /*
     * 不可用时先热重扫一次：很多时候只是上电瞬间线没插好，
     * 插好之后不想重启，敲一下 TEMP 就能当场恢复。
     */
    if (!temp_sensor_is_ready()) {
        printf("  当前不可用，正在重新扫描 1-Wire 总线 (GPIO%d)...\n",
               BOARD_DS18B20_GPIO);
        if (temp_sensor_rescan() == ESP_OK) {
            printf("  [OK] 重新识别到 DS18B20 —— 温度功能已恢复，无需重启\n");
        } else {
            printf("  [--] 仍未识别到设备，按下面顺序查接线\n");
            printf("       1) DQ 是否插在 GPIO%d（最常松的就是这根）\n", BOARD_DS18B20_GPIO);
            printf("       2) VCC 接 3.3V、GND 接 GND（模块丝印顺序别接反）\n");
            printf("       3) DQ 与 VCC 之间要有 4.7k 上拉电阻\n");
        }
    }

    temp_sensor_diag(diag, sizeof(diag));
    printf("  设备状态: %s\n", diag);

    float t = 0.0f;
    esp_err_t err = temp_sensor_read(&t);
    if (err == ESP_OK) {
        printf("  转换结果: %.2f C\n", t);
    } else {
        printf("  转换结果: 失败 (%s)\n", esp_err_to_name(err));
    }

    uint8_t sp[9];
    if (temp_sensor_read_raw(sp) == ESP_OK) {
        int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
        printf("  Scratchpad:");
        for (int i = 0; i < 9; i++) {
            printf(" %02X", sp[i]);
        }
        printf("\n  原始值  : %d -> %.2f C （1360 = 85.0C 上电默认值）\n",
               (int)raw, (float)raw / 16.0f);
        printf("  分辨率  : 配置寄存器 0x%02X（0x7F = 12bit）\n", sp[4]);
    } else {
        printf("  Scratchpad 读取失败：总线无响应，检查接线与 4.7k 上拉\n");
    }
    printf("==================================\n\n");
    return ESP_OK;
}

/* ------------------------------- 网络 ------------------------------------- */
static esp_err_t cmd_prov(const char *args)
{
    (void)args;
    printf("[OK] 启动蓝牙配网...\n");
    printf("     POP 校验码: %s\n", CONFIG_PROV_POP);
    oled_ui_show_message("BLE Provision", "ESP BLE Prov app", 120000);
    return net_prov_start_ble();
}

static esp_err_t cmd_wifi(const char *args)
{
    if (strncasecmp(args, "RESET", 5) == 0) {
        printf("[OK] 已清除配网信息，3 秒后重启进入配网模式\n");
        oled_ui_show_message("WiFi Reset", "Rebooting...", 4000);
        net_prov_reset();
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else if (strncasecmp(args, "STATUS", 6) == 0 || args[0] == '\0') {
        char ip[16], ssid[33];
        net_prov_get_ip(ip, sizeof(ip));
        net_prov_get_ssid(ssid, sizeof(ssid));
        printf("WiFi: %s  SSID: %s  IP: %s\n",
               net_prov_is_connected() ? "已连接" : "未连接", ssid, ip);
    } else {
        printf("[ERR] WIFI STATUS|RESET\n");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------- OTA -------------------------------------- */
static esp_err_t cmd_ota(const char *args)
{
    char url[512];

    if (args[0] != '\0') {
        strncpy(url, args, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else {
#ifdef CONFIG_OTA_ENABLE
        strncpy(url, CONFIG_OTA_FIRMWARE_URL, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
#else
        printf("[ERR] 未启用 OTA（menuconfig -> OTA 远程升级）\n");
        printf("      也可临时指定：OTA http://<服务器IP>:8083/ota/smart_light.bin\n");
        return ESP_ERR_INVALID_STATE;
#endif
    }

    printf("\n========== OTA 远程升级 ==========\n");
    printf("  当前版本  : %s\n", cloud_ota_running_version());
    printf("  当前分区  : %s\n", cloud_ota_running_partition());
    printf("  固件地址  : %s\n", url);
    printf("  升级期间设备照常运行，进度见下方日志\n");
    printf("==================================\n\n");

    cloud_ota_config_t cfg = {
        .url        = url,
        .reboot     = true,
        .timeout_ms = 30000,
    };
    esp_err_t err = cloud_ota_start(&cfg);
    if (err != ESP_OK) {
        printf("[ERR] 启动失败: %s\n", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cmd_cloud(const char *args)
{
    if (strncasecmp(args, "POST", 4) == 0) {
        cloud_mqtt_post_now();
        printf("[OK] 已请求立即上报\n");
    } else if (strncasecmp(args, "OTA", 3) == 0) {
        /* CLOUD OTA [网址] 等价于 OTA 命令 */
        cmd_ota((args[3] == ' ') ? args + 4 : "");
    } else if (strncasecmp(args, "STATUS", 6) == 0 || args[0] == '\0') {
        char info[256];
        info[0] = '\0';
        cloud_mqtt_get_status(info, sizeof(info));
        printf("MQTT : %s\n", info);
    } else {
        printf("[ERR] CLOUD POST|STATUS|OTA\n");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ------------------------------- 系统 ------------------------------------- */
static esp_err_t cmd_status(const char *args)
{
    (void)args;
    sys_state_t st;
    sys_state_get(&st);

    printf("\n========== 系统状态 ==========\n");
    printf("  模式      : %s\n", (st.mode == LIGHT_MODE_AUTO) ? "AUTO" :
                                  (st.mode == LIGHT_MODE_MANUAL) ? "MANUAL" : "OFF");
    printf("  当前颜色  : %d,%d,%d\n", st.rgb[0], st.rgb[1], st.rgb[2]);
    printf("  手动颜色  : %d,%d,%d\n", st.manual_rgb[0], st.manual_rgb[1], st.manual_rgb[2]);
    if (st.temp_valid) {
        printf("  温度      : %.1f C\n", st.temperature);
    } else {
        printf("  温度      : -- （DS18B20 未读到有效值，用 TEMP 命令诊断）\n");
    }
    printf("  环境光    : %d%% (ADC %d)\n", st.light_percent, st.light_adc);
    printf("  WiFi      : %s   IP: %s\n", st.wifi_connected ? "已连接" : "未连接", st.ip_addr);
    printf("  SNTP      : %s\n", st.time_synced ? "已校时" : "未校时");
    printf("  云端      : %s\n", st.cloud_connected ? "已连接" : "未连接");
    printf("  固件版本  : v%s  (运行于 %s)\n",
           cloud_ota_running_version(), cloud_ota_running_partition());

    time_t now = time(NULL);
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    if (tinfo.tm_year > 100) {
        printf("  北京时间  : %04d-%02d-%02d %02d:%02d:%02d\n",
               tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
               tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
    }
    alarm_mgr_print();
    alarm_mgr_timer_print();
    printf("==============================\n\n");
    return ESP_OK;
}

static esp_err_t cmd_reboot(const char *args)
{
    (void)args;
    printf("[OK] 正在重启...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t cmd_help(const char *args);

/* ============================== 命令表 ==================================== */
static const cmd_entry_t s_commands[] = {
    {"AUTO",   "AUTO",                      "自动模式（温度+光照）",      cmd_auto},
    {"MANUAL", "MANUAL",                    "手动模式",                  cmd_manual},
    {"OFF",    "OFF",                       "关闭灯光",                  cmd_off},
    {"COLOR",  "COLOR R,G,B",               "设置颜色 (例 255,100,50)",   cmd_color},
    {"PAGE",   "PAGE 1|2|3",                "切换 OLED 页面",            cmd_page},
    {"BUZZ",   "BUZZ ON|OFF",               "蜂鸣器开关",                cmd_buzz},
    {"TEMP",   "TEMP",                      "温度诊断（含原始值）",      cmd_temp},
    {"ALARM",  "ALARM ADD|DEL|LIST|TEST",   "闹钟管理",                  cmd_alarm},
    {"TIMER",  "TIMER ON|OFF|REPEAT|SHOW",  "定时开关灯",                cmd_timer},
    {"PROV",   "PROV",                      "启动蓝牙 BluFi 配网",       cmd_prov},
    {"WIFI",   "WIFI STATUS|RESET",         "WiFi 状态 / 清除配网",      cmd_wifi},
    {"CLOUD",  "CLOUD POST|STATUS|OTA",     "云端上报 / 状态 / 升级",    cmd_cloud},
    {"OTA",    "OTA [网址]",                "OTA 远程升级固件",          cmd_ota},
    {"STATUS", "STATUS",                    "打印系统状态",              cmd_status},
    {"REBOOT", "REBOOT",                    "重启设备",                  cmd_reboot},
    {"HELP",   "HELP",                      "显示帮助",                  cmd_help},
};

static const int s_command_count = (int)(sizeof(s_commands) / sizeof(s_commands[0]));

static esp_err_t cmd_help(const char *args)
{
    (void)args;
    printf("\n========== 命令列表 ==========\n");
    for (int i = 0; i < s_command_count; i++) {
        printf("  %-28s %s\n", s_commands[i].usage, s_commands[i].desc);
    }
    printf("\n  闹钟示例: ALARM ADD 07:30 daily 1,3,5   (1=周一 ... 7=周日)\n");
    printf("  定时示例: TIMER ON 22:00-6:00          (跨天自动识别)\n");
    printf("            TIMER REPEAT 1,2,3,4,5\n");
    printf("==============================\n\n");
    return ESP_OK;
}

/* ============================== 分发 ====================================== */
static void dispatch(const char *line)
{
    char cmd[16] = {0};
    const char *args = "";

    /* 跳过前导空格 */
    while (*line == ' ') line++;
    if (*line == '\0') {
        return;
    }

    /* 取出命令名 */
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && i < sizeof(cmd) - 1) {
        cmd[i] = (char)toupper((unsigned char)line[i]);
        i++;
    }
    cmd[i] = '\0';
    if (line[i] == ' ') {
        args = line + i + 1;
    }

    for (int c = 0; c < s_command_count; c++) {
        if (strcmp(cmd, s_commands[c].name) == 0) {
            s_commands[c].handler(args);
            return;
        }
    }
    printf("[ERR] 未知命令: %s（输入 HELP 查看列表）\n", cmd);
}

/* ============================== 任务 ====================================== */
static void console_task(void *arg)
{
    char buf[CMD_BUF_SIZE];
    int idx = 0;

    printf("\n");
    printf("=========================================\n");
    printf("   Smart Light 串口控制台已就绪\n");
    printf("   输入 HELP 查看全部命令\n");
    printf("=========================================\n");
    cmd_help(NULL);

    while (1) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (idx > 0) {
                buf[idx] = '\0';
                idx = 0;
                if (strcmp(buf, "\x08\x08") != 0) {
                    dispatch(buf);
                }
            }
        } else if (idx < CMD_BUF_SIZE - 1) {
            buf[idx++] = (char)ch;
        } else {
            idx = 0;   /* 超长丢弃 */
        }
    }
}

esp_err_t console_cmd_start(void)
{
    if (xTaskCreatePinnedToCore(console_task, "console_cmd", 4096, NULL, 3,
                                NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "串口命令任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
